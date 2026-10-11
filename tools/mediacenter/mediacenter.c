// mediacenter - MiSTer MediaCenter, the single media player application.
//
// Runs on the HPS like vcd_player, whose code it contains: the framebuffer
// output, the on-screen controls, the gamepad input and the audio output are
// shared, and Video CDs are still played by the Video CD player.
//
//   mediacenter              file browser: disc, SD card and USB sources
//   mediacenter /dev/srN     play the disc in the drive: Video CD or DVD-Video
//   mediacenter <file>       play one file and exit
//
// Files are decoded in software with FFmpeg: MPEG-1/2, MPEG-4 Part 2 (DivX,
// Xvid), H.264, WMV and VC-1 video; MP1/MP2/MP3, AC-3, DTS, AAC, Vorbis, FLAC,
// WMA and PCM audio; AVI, Matroska, MP4, MPEG-PS/TS, Ogg and ASF containers.
// DVD-Video discs, menus included, play through libdvdnav (dvd.c).

#define main vcd_main
#include "../vcd_player/vcd_player.c"
#undef main

#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define MC_AUDIO_RATE 48000
#define MC_AUDIO_LEAD 0.3
#define MC_VQ_MAX 40  // pictures the queue can hold
#define MC_VQ_FILE 12 // pictures decoded ahead for files
#define MC_PENDING_SECONDS 4
#define MC_SKIP_SECONDS 60.0

// ---------------------------------------------------------------------------
// Full-screen text (browser and audio-only screen)
// ---------------------------------------------------------------------------

static void fb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > fb_w) w = fb_w - x;
	if (y + h > fb_h) h = fb_h - y;
	for (int j = 0; j < h; j++)
	{
		uint32_t *row = (uint32_t *)(fb_mem + (size_t)(y + j) * fb_stride) + x;
		for (int i = 0; i < w; i++) row[i] = color;
	}
}

static uint32_t fb_rgb(int r, int g, int b)
{
	return fb_bgr ? (0xFF000000u | (r << 16) | (g << 8) | b) : (0xFF000000u | (b << 16) | (g << 8) | r);
}

// Text at scale s (8*s pixels per glyph); returns the x after the text.
static int fb_text(int x, int y, int s, const char *str, uint32_t color)
{
	for (; *str; str++, x += GLYPH_W * s)
	{
		const unsigned char *g = vcd_font[(unsigned char)*str & 0x7F];
		for (int c = 0; c < 8; c++)
			for (int r = 0; r < 8; r++)
				if ((g[c] >> r) & 1) fb_fill_rect(x + c * s, y + r * s, s, s, color);
	}
	return x;
}

// Copies at most max glyphs of src, replacing non-ASCII bytes with '?'.
static void fit_text(char *dst, size_t len, const char *src, int max)
{
	size_t n = 0;
	int glyphs = 0;
	for (; *src && n + 4 < len && glyphs < max; src++)
	{
		unsigned char c = (unsigned char)*src;
		if (c >= 0x80)
		{
			// skip UTF-8 continuation bytes, one '?' per character
			if ((c & 0xC0) == 0x80) continue;
			c = '?';
		}
		dst[n++] = (char)c;
		glyphs++;
	}
	if (*src && n >= 3 && glyphs >= max)
	{
		dst[n - 3] = dst[n - 2] = dst[n - 1] = '.';
	}
	dst[n] = 0;
}

// ---------------------------------------------------------------------------
// File types
// ---------------------------------------------------------------------------

static const char *const video_ext[] = { "avi", "divx", "xvid", "mkv", "mp4", "m4v", "mov", "mpg", "mpeg", "vob", "dat", "ts", "m2ts", "mts", "ogm", "ogv", "wmv", "asf", "iso", NULL };
static const char *const audio_ext[] = { "mp3", "mp2", "mp1", "mpa", "ogg", "oga", "flac", "wav", "m4a", "aac", "ac3", "dts", "wma", "mka", NULL };

static int has_ext(const char *name, const char *const *list)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return 0;
	for (int i = 0; list[i]; i++)
		if (!strcasecmp(dot + 1, list[i])) return 1;
	return 0;
}

static int is_media(const char *name)
{
	return has_ext(name, video_ext) || has_ext(name, audio_ext);
}

static int is_audio_file(const char *name)
{
	return has_ext(name, audio_ext);
}

// ---------------------------------------------------------------------------
// File player (FFmpeg)
// ---------------------------------------------------------------------------

typedef struct
{
	AVFrame *frame;
	double pts;
} vq_item_t;

typedef struct
{
	AVFormatContext *fmt;
	int vstream, astream;
	AVCodecContext *vdec, *adec;
	struct SwsContext *sws;
	SwrContext *swr;
	AVPacket *pkt;
	AVFrame *frame;

	double start;     // stream start time, subtracted from every timestamp
	double duration;
	double clock;     // playing position
	double last_vpts;
	int eof, flushed;

	vq_item_t vq[MC_VQ_MAX];
	int vq_count;

	// decoded audio not yet sent to the output (pushed MC_AUDIO_LEAD early)
	int16_t *pend;
	int pend_len, pend_cap; // stereo frames
	double pend_pts;        // time of pend[0]
	double audio_end;       // time just after the last decoded audio sample

	int audio_tracks[16];
	int num_audio_tracks, audio_track;
	char audio_label[32];
	int audio_only;
	char title[256];
} media_t;

static media_t mc;

static double ts_seconds(int64_t ts, AVRational tb)
{
	return ts == AV_NOPTS_VALUE ? NAN : ts * av_q2d(tb);
}

// Pictures the video decoder always skips (light mode for 50/60 fps HD).
static enum AVDiscard skip_base = AVDISCARD_DEFAULT;

static AVCodecContext *open_decoder(int stream)
{
	AVStream *st = mc.fmt->streams[stream];
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec)
	{
		fprintf(stderr, "mediacenter: no decoder for %s\n", avcodec_get_name(st->codecpar->codec_id));
		return NULL;
	}
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) return NULL;
	avcodec_parameters_to_context(ctx, st->codecpar);
	ctx->pkt_timebase = st->time_base;
	ctx->thread_count = 2; // both Cortex-A9 cores
	ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	// Light mode: H.264/HEVC above DVD size is beyond the Cortex-A9. The
	// deblocking filter is a large part of the work and its absence is hard
	// to see at TV distance; MC_FULLQUALITY keeps it.
	int heavy = (codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_HEVC || codec->id == AV_CODEC_ID_VP9) &&
		st->codecpar->width * st->codecpar->height > 720 * 576;
	if (heavy && !getenv("MC_FULLQUALITY"))
	{
		ctx->skip_loop_filter = AVDISCARD_ALL;
		ctx->flags2 |= AV_CODEC_FLAG2_FAST;
		// Above 30 pictures per second, show only the pictures other
		// pictures refer to: about half of them, still a smooth 25-30 fps.
		double fps = av_q2d(st->avg_frame_rate);
		if (fps > 31) skip_base = AVDISCARD_NONREF;
		ctx->skip_frame = skip_base;
		fprintf(stderr, "mediacenter: %dx%d %s, light decoding\n", st->codecpar->width, st->codecpar->height, codec->name);
	}
	if (avcodec_open2(ctx, codec, NULL) < 0)
	{
		avcodec_free_context(&ctx);
		return NULL;
	}
	return ctx;
}

static void vq_clear(void)
{
	for (int i = 0; i < mc.vq_count; i++) av_frame_free(&mc.vq[i].frame);
	mc.vq_count = 0;
}

static void set_audio_label(void)
{
	mc.audio_label[0] = 0;
	if (mc.num_audio_tracks > 1)
	{
		AVDictionaryEntry *lang = av_dict_get(mc.fmt->streams[mc.astream]->metadata, "language", NULL, 0);
		snprintf(mc.audio_label, sizeof(mc.audio_label), "Audio %d/%d%s%s", mc.audio_track + 1, mc.num_audio_tracks,
			lang ? " " : "", lang ? lang->value : "");
	}
}

static int open_audio_track(int index)
{
	if (mc.adec) avcodec_free_context(&mc.adec);
	swr_free(&mc.swr);
	mc.astream = -1;
	if (index < 0 || index >= mc.num_audio_tracks) return -1;

	int stream = mc.audio_tracks[index];
	mc.adec = open_decoder(stream);
	if (!mc.adec) return -1;

	AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
	if (swr_alloc_set_opts2(&mc.swr, &out, AV_SAMPLE_FMT_S16, MC_AUDIO_RATE,
		&mc.adec->ch_layout, mc.adec->sample_fmt, mc.adec->sample_rate, 0, NULL) < 0 || swr_init(mc.swr) < 0)
	{
		swr_free(&mc.swr);
		avcodec_free_context(&mc.adec);
		return -1;
	}
	mc.astream = stream;
	mc.audio_track = index;
	set_audio_label();
	return 0;
}

static void media_close(void)
{
	vq_clear();
	if (mc.vdec) avcodec_free_context(&mc.vdec);
	if (mc.adec) avcodec_free_context(&mc.adec);
	swr_free(&mc.swr);
	sws_freeContext(mc.sws);
	av_packet_free(&mc.pkt);
	av_frame_free(&mc.frame);
	if (mc.fmt) avformat_close_input(&mc.fmt);
	free(mc.pend);
	memset(&mc, 0, sizeof(mc));
	mc.vstream = mc.astream = -1;
}

static int media_open(const char *path)
{
	skip_base = AVDISCARD_DEFAULT;
	media_close();
	if (avformat_open_input(&mc.fmt, path, NULL, NULL) < 0)
	{
		fprintf(stderr, "mediacenter: cannot open %s\n", path);
		return -1;
	}
	if (avformat_find_stream_info(mc.fmt, NULL) < 0)
	{
		fprintf(stderr, "mediacenter: no stream information in %s\n", path);
		media_close();
		return -1;
	}

	mc.vstream = av_find_best_stream(mc.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (mc.vstream >= 0 && (mc.fmt->streams[mc.vstream]->disposition & AV_DISPOSITION_ATTACHED_PIC)) mc.vstream = -1;
	if (mc.vstream >= 0)
	{
		mc.vdec = open_decoder(mc.vstream);
		if (!mc.vdec) mc.vstream = -1;
	}

	int best_audio = av_find_best_stream(mc.fmt, AVMEDIA_TYPE_AUDIO, -1, mc.vstream, NULL, 0);
	for (unsigned i = 0; i < mc.fmt->nb_streams && mc.num_audio_tracks < 16; i++)
	{
		if (mc.fmt->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
		if ((int)i == best_audio) mc.audio_track = mc.num_audio_tracks;
		mc.audio_tracks[mc.num_audio_tracks++] = i;
	}
	if (mc.num_audio_tracks) open_audio_track(mc.audio_track);

	if (mc.vstream < 0 && mc.astream < 0)
	{
		fprintf(stderr, "mediacenter: nothing playable in %s\n", path);
		media_close();
		return -1;
	}

	for (unsigned i = 0; i < mc.fmt->nb_streams; i++)
	{
		if ((int)i != mc.vstream && (int)i != mc.astream) mc.fmt->streams[i]->discard = AVDISCARD_ALL;
	}

	mc.pkt = av_packet_alloc();
	mc.frame = av_frame_alloc();
	mc.pend_cap = MC_AUDIO_RATE * MC_PENDING_SECONDS;
	mc.pend = (int16_t *)malloc((size_t)mc.pend_cap * 4);
	mc.start = mc.fmt->start_time != AV_NOPTS_VALUE ? mc.fmt->start_time / (double)AV_TIME_BASE : 0;
	mc.duration = mc.fmt->duration != AV_NOPTS_VALUE ? mc.fmt->duration / (double)AV_TIME_BASE : 0;
	mc.audio_only = mc.vstream < 0;
	mc.last_vpts = NAN;

	const char *base = strrchr(path, '/');
	AVDictionaryEntry *title = av_dict_get(mc.fmt->metadata, "title", NULL, 0);
	AVDictionaryEntry *artist = av_dict_get(mc.fmt->metadata, "artist", NULL, 0);
	if (title && artist) snprintf(mc.title, sizeof(mc.title), "%s - %s", artist->value, title->value);
	else snprintf(mc.title, sizeof(mc.title), "%s", title ? title->value : (base ? base + 1 : path));

	if (mc.vstream >= 0)
	{
		AVStream *st = mc.fmt->streams[mc.vstream];
		AVRational sar = av_guess_sample_aspect_ratio(mc.fmt, st, NULL);
		double w = mc.vdec->width, h = mc.vdec->height;
		if (sar.num > 0 && sar.den > 0) w *= av_q2d(sar);
		fb_aspect = (h > 0 && w > 0) ? w / h : 4.0 / 3.0;
		printf("mediacenter: %s, video %s %dx%d aspect %.3f, audio %s, %.0f s\n", path,
			avcodec_get_name(st->codecpar->codec_id), mc.vdec->width, mc.vdec->height, fb_aspect,
			mc.adec ? avcodec_get_name(mc.adec->codec_id) : "none", mc.duration);
	}
	else
	{
		printf("mediacenter: %s, audio %s, %.0f s\n", path, avcodec_get_name(mc.adec->codec_id), mc.duration);
	}
	return 0;
}

// Converts decoded audio to the output format and keeps it until it is due;
// pts is the time of its first sample (NAN: right after the previous one).
static void push_audio_at(AVFrame *f, double pts)
{
	if (isnan(pts)) pts = mc.audio_end;

	int room = mc.pend_cap - mc.pend_len;
	if (room <= 0) return;
	if (!mc.pend_len) mc.pend_pts = pts;
	uint8_t *out = (uint8_t *)(mc.pend + (size_t)mc.pend_len * 2);
	int n = swr_convert(mc.swr, &out, room, (const uint8_t **)f->extended_data, f->nb_samples);
	if (n <= 0) return;
	mc.pend_len += n;
	mc.audio_end = mc.pend_pts + mc.pend_len / (double)MC_AUDIO_RATE;
}

static void push_audio(AVFrame *f)
{
	push_audio_at(f, ts_seconds(f->best_effort_timestamp, mc.fmt->streams[mc.astream]->time_base) - mc.start);
}

// Sends the decoded audio that is due (up to MC_AUDIO_LEAD ahead) to aplay.
static void feed_audio_output(void)
{
	if (!mc.pend_len) return;

	// drop what is already late (after a seek or a stall)
	int late = (int)((mc.clock - 0.05 - mc.pend_pts) * MC_AUDIO_RATE);
	if (late > 0)
	{
		if (late > mc.pend_len) late = mc.pend_len;
		memmove(mc.pend, mc.pend + (size_t)late * 2, (size_t)(mc.pend_len - late) * 4);
		mc.pend_len -= late;
		mc.pend_pts += late / (double)MC_AUDIO_RATE;
	}

	int due = (int)((mc.clock + MC_AUDIO_LEAD - mc.pend_pts) * MC_AUDIO_RATE);
	if (due <= 0) return;
	if (due > mc.pend_len) due = mc.pend_len;

	if (audio_pipe)
	{
		pthread_mutex_lock(&audio_lock);
		for (int i = 0; i < due; i++)
		{
			if (audio_wr - audio_rd >= AUDIO_RING) audio_rd++;
			unsigned j = audio_wr % AUDIO_RING;
			int16_t l = mc.pend[i * 2], r = mc.pend[i * 2 + 1];
			if (mc.num_audio_tracks <= 1)
			{
				if (audio_mode == 1) r = l;
				else if (audio_mode == 2) l = r;
			}
			audio_ring[j * 2] = l;
			audio_ring[j * 2 + 1] = r;
			audio_wr++;
		}
		pthread_cond_signal(&audio_cond);
		pthread_mutex_unlock(&audio_lock);
	}

	memmove(mc.pend, mc.pend + (size_t)due * 2, (size_t)(mc.pend_len - due) * 4);
	mc.pend_len -= due;
	mc.pend_pts += due / (double)MC_AUDIO_RATE;
}

// Queues a decoded picture to show at pts (NAN: one frame after the previous).
static void queue_video_at(AVFrame *f, double pts, double frame_time)
{
	if (isnan(pts)) pts = isnan(mc.last_vpts) ? 0 : mc.last_vpts + frame_time;
	mc.last_vpts = pts;
	if (mc.vq_count >= MC_VQ_MAX) return;
	AVFrame *copy = av_frame_clone(f);
	if (!copy) return;
	mc.vq[mc.vq_count].frame = copy;
	mc.vq[mc.vq_count].pts = pts;
	mc.vq_count++;
}

static void queue_video(AVFrame *f)
{
	AVRational fr = mc.fmt->streams[mc.vstream]->avg_frame_rate;
	double step = (fr.num > 0 && fr.den > 0) ? 1.0 / av_q2d(fr) : 0.04;
	queue_video_at(f, ts_seconds(f->best_effort_timestamp, mc.fmt->streams[mc.vstream]->time_base) - mc.start, step);
}

static void receive_frames(AVCodecContext *ctx, int video)
{
	while (avcodec_receive_frame(ctx, mc.frame) == 0)
	{
		if (video) queue_video(mc.frame);
		else push_audio(mc.frame);
		av_frame_unref(mc.frame);
	}
}

// Reads and decodes one packet. Returns 0 at the end of the file.
static int decode_step(void)
{
	if (mc.eof)
	{
		if (!mc.flushed)
		{
			mc.flushed = 1;
			if (mc.vdec) { avcodec_send_packet(mc.vdec, NULL); receive_frames(mc.vdec, 1); }
			if (mc.adec) { avcodec_send_packet(mc.adec, NULL); receive_frames(mc.adec, 0); }
		}
		return 0;
	}

	int r = av_read_frame(mc.fmt, mc.pkt);
	if (r < 0)
	{
		mc.eof = 1;
		return 1;
	}
	if (mc.pkt->stream_index == mc.vstream && mc.vdec)
	{
		if (avcodec_send_packet(mc.vdec, mc.pkt) == AVERROR(EAGAIN))
		{
			receive_frames(mc.vdec, 1);
			avcodec_send_packet(mc.vdec, mc.pkt);
		}
		receive_frames(mc.vdec, 1);
	}
	else if (mc.pkt->stream_index == mc.astream && mc.adec)
	{
		if (avcodec_send_packet(mc.adec, mc.pkt) == AVERROR(EAGAIN))
		{
			receive_frames(mc.adec, 0);
			avcodec_send_packet(mc.adec, mc.pkt);
		}
		receive_frames(mc.adec, 0);
	}
	av_packet_unref(mc.pkt);
	return 1;
}

static int need_more(void)
{
	if (mc.vstream >= 0)
	{
		if (mc.vq_count >= MC_VQ_FILE) return 0;
		if (!mc.vq_count || mc.vq[mc.vq_count - 1].pts <= mc.clock + 0.1) return 1;
	}
	if (mc.astream >= 0 && mc.pend_len < mc.pend_cap - MC_AUDIO_RATE / 2 && mc.audio_end < mc.clock + 0.5) return 1;
	return 0;
}

static void fill(void)
{
	// bounded so the controls stay responsive on a very slow stream
	for (int i = 0; i < 200 && need_more(); i++)
	{
		if (!decode_step()) break;
		// a slow video picture must not leave the sound output empty
		feed_audio_output();
	}
}

// Called after a picture is converted and before it is shown: the DVD player
// draws subtitles and menu buttons on frame_rgb here.
static void (*frame_overlay)(void) = NULL;

// Interlaced pictures (DVD, MPEG-2) carry two fields taken at different
// moments: in fast motion the lines of one field do not match the other and
// show as combs, made worse by the vertical scaling dropping lines. Each line
// is averaged with the next one, which hides the combs at little cost.
static void deinterlace_rgb(void)
{
	for (int y = 0; y + 1 < src_h; y++)
	{
		uint32_t *a = (uint32_t *)(frame_rgb + (size_t)y * src_w * 4);
		const uint32_t *b = a + src_w;
		for (int x = 0; x < src_w; x++)
		{
			uint32_t p = a[x], q = b[x];
			a[x] = (p & q) + (((p ^ q) & 0xFEFEFEFEu) >> 1);
		}
	}
}

// The picture is written straight into the visible framebuffer: starting
// right after the vertical blank keeps the copy ahead of the scan, so the
// screen never shows half of one picture and half of the next (tearing).
static int fb_vsync_fd = -2;

static void fb_wait_vsync(void)
{
	if (fb_vsync_fd == -2)
	{
		fb_vsync_fd = getenv("MC_NOVSYNC") ? -1 : open("/dev/fb0", O_RDWR | O_CLOEXEC);
	}
	if (fb_vsync_fd < 0) return;
	int zero = 0;
	if (ioctl(fb_vsync_fd, FBIO_WAITFORVSYNC, &zero) == -1)
	{
		fprintf(stderr, "mediacenter: no vsync (%s), drawing without it\n", strerror(errno));
		close(fb_vsync_fd);
		fb_vsync_fd = -1;
	}
}

// DVD pictures are drawn at their own size (720x576 or 720x480) and the
// FPGA scaler enlarges the framebuffer to the screen with the picture's
// aspect: the ARM does much less work and the scaler filters better than
// the line repetition of fb_present. Needs a Main that knows fb_cmd3; with
// another Main the framebuffer just stays as it is.
static int native_fb = 0;        // set while a DVD plays
static int native_w, native_h;   // current special mode, 0 for the normal one
static double native_aspect;
static int native_ok = 1;        // cleared when Main ignores the command
static int fb_fake = 0;          // test mode: memory framebuffer
static int def_w, def_h;         // the normal framebuffer

static void screen_rebuild(void)
{
	free(ovl_a); free(ovl_v); free(full_line);
	ovl_a = ovl_v = NULL;
	full_line = NULL;
	osd_layout();
	src_w = src_h = 0;
	fb_layout_aspect = 0;
	have_frame = 0;
}

static void fb_native(int w, int h, double aspect)
{
	if (!native_ok || !fb_mem) return;
	if (w == native_w && h == native_h && (!w || aspect == native_aspect)) return;
	if (!native_w)
	{
		def_w = fb_w;
		def_h = fb_h;
	}
	int want_w = w ? w : def_w, want_h = w ? h : def_h;

	if (fb_fake)
	{
		free(fb_mem);
		fb_w = want_w;
		fb_h = want_h;
		fb_stride = fb_w * 4;
		fb_len = (size_t)fb_stride * fb_h;
		fb_mem = (uint8_t *)calloc(1, fb_len);
	}
	else
	{
		int fd = open("/dev/MiSTer_cmd", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
		{
			native_ok = 0;
			return;
		}
		char cmd[96];
		if (w) snprintf(cmd, sizeof(cmd), "fb_cmd3 8888 1 %d %d %d 1000\n", w, h, (int)(aspect * 1000 + 0.5));
		else snprintf(cmd, sizeof(cmd), "fb_cmd3 default\n");
		int ok = write(fd, cmd, strlen(cmd)) > 0;
		close(fd);

		// Main applies the mode in its own loop: wait for Linux to report it.
		int changed = 0;
		for (int i = 0; ok && i < 100 && !changed; i++)
		{
			usleep(10000);
			int f = open("/dev/fb0", O_RDWR | O_CLOEXEC);
			struct fb_var_screeninfo var;
			if (f >= 0 && !ioctl(f, FBIOGET_VSCREENINFO, &var))
				changed = (int)var.xres == want_w && (int)var.yres == want_h;
			if (f >= 0) close(f);
		}
		if (!changed)
		{
			fprintf(stderr, "mediacenter: framebuffer %dx%d not available, scaling on the ARM\n", want_w, want_h);
			native_ok = 0;
			return;
		}
		munmap(fb_mem, fb_len);
		fb_mem = NULL;
		if (fb_open())
		{
			fprintf(stderr, "mediacenter: framebuffer lost\n");
			quit_requested = 1;
			return;
		}
	}
	fprintf(stderr, "mediacenter: framebuffer %dx%d%s\n", fb_w, fb_h, w ? " stretched by the scaler" : "");
	native_w = w;
	native_h = h;
	native_aspect = aspect;
	fb_par = w ? aspect * h / w : 1.0;
	screen_rebuild();
}

// Converts and draws a decoded picture.
static void draw_video_frame(AVFrame *f)
{
	if (native_fb)
	{
		// Small pictures (DivX, Video CD) get whole multiples of their lines
		// so the controls still fit and the scaler has less to enlarge.
		int mx = f->width < 480 ? 2 : 1, my = 1;
		while (f->height * (my + 1) <= 1080 && f->height * my < 400) my++;
		fb_native(f->width * mx, f->height * my, fb_aspect);
	}
	if (!fb_mem) return;
	fb_layout(f->width, f->height);
	if (!frame_rgb || !line_buf || !x_map) return;
	mc.sws = sws_getCachedContext(mc.sws, f->width, f->height, (enum AVPixelFormat)f->format,
		f->width, f->height, fb_bgr ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA, SWS_POINT, NULL, NULL, NULL);
	if (!mc.sws) return;
	int stride = src_w * 4;
	if (!(stride & 15))
	{
		uint8_t *dst[4] = { frame_rgb, NULL, NULL, NULL };
		int dst_stride[4] = { stride, 0, 0, 0 };
		sws_scale(mc.sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dst_stride);
	}
	else
	{
		// The NEON conversion needs 16-byte aligned rows: convert into an
		// aligned buffer and copy the rows.
		static uint8_t *tmp = NULL;
		static int tmp_size = 0;
		int tstride = (stride + 63) & ~63;
		if (tmp_size < tstride * f->height)
		{
			av_free(tmp);
			tmp = (uint8_t *)av_malloc((size_t)tstride * f->height);
			tmp_size = tmp ? tstride * f->height : 0;
		}
		if (!tmp) return;
		uint8_t *dst[4] = { tmp, NULL, NULL, NULL };
		int dst_stride[4] = { tstride, 0, 0, 0 };
		sws_scale(mc.sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dst_stride);
		for (int y = 0; y < f->height; y++) memcpy(frame_rgb + (size_t)y * stride, tmp + (size_t)y * tstride, stride);
	}
	if (f->flags & AV_FRAME_FLAG_INTERLACED) deinterlace_rgb();
	if (frame_overlay) frame_overlay();
	have_frame = 1;
	fb_wait_vsync();
	fb_present();
}

// Takes the frames that are due; returns the most recent one (owned by the
// caller) or NULL.
static int frames_shown, frames_dropped; // reported in the log

static AVFrame *due_frame(void)
{
	AVFrame *show = NULL;
	int n = 0;
	while (n < mc.vq_count && mc.vq[n].pts <= mc.clock)
	{
		if (show) { av_frame_free(&show); frames_dropped++; }
		show = mc.vq[n].frame;
		n++;
	}
	if (n)
	{
		memmove(mc.vq, mc.vq + n, (mc.vq_count - n) * sizeof(mc.vq[0]));
		mc.vq_count -= n;
		frames_shown++;
	}
	return show;
}

static void media_seek(double t)
{
	if (t < 0) t = 0;
	if (mc.duration > 0 && t > mc.duration - 0.5) t = mc.duration - 0.5;
	if (t < 0) t = 0;
	int64_t target = (int64_t)((t + mc.start) * AV_TIME_BASE);
	avformat_seek_file(mc.fmt, -1, INT64_MIN, target, target, AVSEEK_FLAG_BACKWARD);
	if (mc.vdec) avcodec_flush_buffers(mc.vdec);
	if (mc.adec) avcodec_flush_buffers(mc.adec);
	vq_clear();
	mc.pend_len = 0;
	mc.audio_end = t;
	mc.last_vpts = NAN;
	mc.eof = mc.flushed = 0;
	mc.clock = t;
	audio_flush();
}

// Shows the first picture at or after t (fast forward / rewind).
static void media_preview(double t)
{
	media_seek(t);
	if (mc.vstream < 0) return;
	for (int i = 0; i < 400 && !mc.vq_count; i++)
	{
		if (!decode_step()) break;
	}
	if (mc.vq_count)
	{
		AVFrame *f = mc.vq[0].frame;
		draw_video_frame(f);
		vq_clear();
	}
	mc.pend_len = 0;
}

static int chapter_index(double t)
{
	int c = -1;
	for (unsigned i = 0; i < mc.fmt->nb_chapters; i++)
	{
		AVChapter *ch = mc.fmt->chapters[i];
		if (ts_seconds(ch->start, ch->time_base) - mc.start <= t + 0.5) c = i;
	}
	return c;
}

static double chapter_start(int c)
{
	AVChapter *ch = mc.fmt->chapters[c];
	return ts_seconds(ch->start, ch->time_base) - mc.start;
}

static void draw_audio_screen(void)
{
	memset(fb_mem, 0, fb_len);
	int s = fb_w >= 1280 ? 3 : 2;
	char line[128];
	fit_text(line, sizeof(line), mc.title, fb_w / (GLYPH_W * s) - 4);
	int w = (int)strlen(line) * GLYPH_W * s;
	fb_text((fb_w - w) / 2, fb_h / 3, s, line, fb_rgb(255, 255, 255));
	if (mc.adec)
	{
		snprintf(line, sizeof(line), "%s  %d Hz", avcodec_get_name(mc.adec->codec_id), mc.adec->sample_rate);
		w = (int)strlen(line) * GLYPH_W * (s - 1);
		fb_text((fb_w - w) / 2, fb_h / 3 + 16 * s, s - 1, line, fb_rgb(170, 170, 170));
	}
}

enum { END_DONE, END_NEXT, END_PREV, END_BACK, END_QUIT };

// Plays the open file. Returns how playback ended.
static int media_play(int in_playlist)
{
	player_t p;
	memset(&p, 0, sizeof(p));
	p.state = ST_PLAY;
	p.osd_until = now_seconds() + 3.0;
	have_frame = 0;
	exit_question = "Tornare all'elenco?";

	if (mc.astream >= 0 && audio_open(MC_AUDIO_RATE)) fprintf(stderr, "mediacenter: playing without audio\n");
	memset(fb_mem, 0, fb_len);
	if (mc.audio_only) draw_audio_screen();

	int result = END_DONE;
	double last = now_seconds(), osd_built = 0;
	while (!quit_requested)
	{
		double t = now_seconds();
		double elapsed = t - last;
		last = t;
		if (elapsed > 0.5) elapsed = 0.5;

		for (int code; (code = input_key(t));)
		{
			p.osd_until = t + OSD_TIMEOUT;
			p.dirty = 1;
			if (p.confirm_exit)
			{
				p.confirm_exit = 0;
				if (code == KEY_ENTER) result = END_BACK;
				continue;
			}
			switch (code)
			{
			case KEY_PLAY:
			case KEY_PLAYCD:
				if (p.state == ST_STOP) media_seek(0);
				else if (p.state == ST_FF || p.state == ST_REW) media_seek(p.ff_time);
				p.state = ST_PLAY;
				break;
			case KEY_PAUSE:
			case KEY_PAUSECD:
			case KEY_PLAYPAUSE:
				if (p.state == ST_PLAY) p.state = ST_PAUSE;
				else if (p.state == ST_PAUSE) p.state = ST_PLAY;
				else if (p.state == ST_FF || p.state == ST_REW) { media_seek(p.ff_time); p.state = ST_PAUSE; }
				break;
			case KEY_ESC:
			case KEY_STOP:
			case KEY_STOPCD:
				p.state = ST_STOP;
				media_seek(0);
				audio_flush();
				break;
			case KEY_RIGHT:
			case KEY_NEXTSONG:
				if (in_playlist && mc.audio_only) result = END_NEXT;
				else if (mc.fmt->nb_chapters)
				{
					int c = chapter_index(mc.clock) + 1;
					if (c < (int)mc.fmt->nb_chapters) media_seek(chapter_start(c));
				}
				else media_seek(mc.clock + MC_SKIP_SECONDS);
				break;
			case KEY_LEFT:
			case KEY_PREVIOUSSONG:
				if (in_playlist && mc.audio_only && mc.clock < 3.0) result = END_PREV;
				else if (mc.audio_only) media_seek(0);
				else if (mc.fmt->nb_chapters)
				{
					int c = chapter_index(mc.clock);
					if (c >= 0 && mc.clock - chapter_start(c) < 3.0) c--;
					media_seek(c >= 0 ? chapter_start(c) : 0);
				}
				else media_seek(mc.clock - MC_SKIP_SECONDS);
				break;
			case KEY_PAGEDOWN:
			case KEY_FASTFORWARD:
			case KEY_PAGEUP:
			case KEY_REWIND:
			{
				int want = (code == KEY_PAGEDOWN || code == KEY_FASTFORWARD) ? ST_FF : ST_REW;
				if (p.state == ST_STOP) break;
				if (p.state == want) { media_seek(p.ff_time); p.state = ST_PLAY; break; }
				if (p.state != ST_FF && p.state != ST_REW) p.ff_time = mc.clock;
				audio_flush();
				p.state = want;
				p.ff_step = 0;
				break;
			}
			case KEY_UP:
			case KEY_DOWN:
				if (mc.num_audio_tracks > 1)
				{
					int dir = code == KEY_DOWN ? 1 : mc.num_audio_tracks - 1;
					double at = mc.clock;
					open_audio_track((mc.audio_track + dir) % mc.num_audio_tracks);
					media_seek(at);
				}
				else audio_mode = (audio_mode + (code == KEY_DOWN ? 1 : 2)) % 3;
				break;
			case KEY_TAB:
				p.confirm_exit = 1;
				break;
			}
			if (result != END_DONE) break;
		}
		if (result != END_DONE) break;

		AVFrame *show = NULL;
		if (p.state == ST_PLAY)
		{
			mc.clock += elapsed;
			fill();
			feed_audio_output();
			show = due_frame();

			// A stream too heavy for the CPU: skip pictures nothing refers to
			// until the decoder catches up with the clock.
			if (mc.vdec)
			{
				// Far behind: decode only key pictures until it catches up, so
				// the audio keeps being decoded in time.
				double lag = isnan(mc.last_vpts) ? 0 : mc.clock - mc.last_vpts;
				enum AVDiscard skip = skip_base;
				if (mc.vq_count == 0 && lag > 0.3 && skip < AVDISCARD_NONREF) skip = AVDISCARD_NONREF;
				if (mc.vq_count == 0 && lag > 1.5) skip = AVDISCARD_NONKEY;
				mc.vdec->skip_frame = skip;
			}

			int video_done = mc.vstream < 0 || (mc.flushed && !mc.vq_count);
			int audio_done = mc.astream < 0 || (mc.flushed && !mc.pend_len);
			if (mc.eof && mc.flushed && video_done && audio_done)
			{
				if (show) draw_video_frame(show);
				av_frame_free(&show);
				result = in_playlist && mc.audio_only ? END_NEXT : END_DONE;
				break;
			}
		}
		else if (p.state == ST_FF || p.state == ST_REW)
		{
			p.ff_step += elapsed;
			if (p.ff_step >= 0.2)
			{
				double step = p.ff_step * 10.0;
				p.ff_step = 0;
				p.ff_time += p.state == ST_FF ? step : -step;
				if (p.ff_time < 0) { p.ff_time = 0; media_seek(0); p.state = ST_PLAY; }
				else if (mc.duration > 0 && p.ff_time > mc.duration - 1.0) { media_seek(mc.duration - 1.0); p.state = ST_PLAY; }
				else if (mc.vstream >= 0) media_preview(p.ff_time);
				else mc.clock = p.ff_time;
			}
		}

		if (p.confirm_exit && t >= p.osd_until) p.confirm_exit = 0;
		int show_osd = p.state == ST_STOP || t < p.osd_until || mc.audio_only;
		if (show_osd && (p.dirty || t - osd_built >= 0.25))
		{
			osd_info_t info;
			memset(&info, 0, sizeof(info));
			info.state = p.state;
			info.confirm_exit = p.confirm_exit;
			info.time = (p.state == ST_FF || p.state == ST_REW) ? p.ff_time : mc.clock;
			info.duration = mc.duration;
			info.chapters = mc.fmt->nb_chapters;
			info.chapter = info.chapters ? chapter_index(info.time) + 1 : 0;
			info.audio_mode = audio_mode;
			info.audio_label = mc.audio_label[0] ? mc.audio_label : NULL;
			if (mc.astream < 0) info.audio_label = "Senza audio";
			osd_build(&info);
			osd_built = t;
			p.dirty = 1;
		}
		if (show_osd != ovl_shown)
		{
			ovl_shown = show_osd;
			p.dirty = 1;
		}

		if (show)
		{
			draw_video_frame(show);
			av_frame_free(&show);
		}
		else if (p.dirty)
		{
			fb_present();
		}
		p.dirty = 0;

		usleep(p.state == ST_PLAY ? 2000 : 10000);
	}

	audio_close();
	exit_question = "Uscire dal Video CD?";
	return quit_requested ? END_QUIT : result;
}

#include "dvd.c"

// ---------------------------------------------------------------------------
// Browser
// ---------------------------------------------------------------------------

#define MAX_ITEMS 2048

typedef struct
{
	char name[256];
	int dir;
} item_t;

static item_t items[MAX_ITEMS];
static int num_items = 0;

static int item_cmp(const void *a, const void *b)
{
	const item_t *x = (const item_t *)a, *y = (const item_t *)b;
	if (x->dir != y->dir) return y->dir - x->dir;
	return strcasecmp(x->name, y->name);
}

static int list_dir(const char *path)
{
	num_items = 0;
	DIR *d = opendir(path);
	if (!d) return -1;
	struct dirent *de;
	while ((de = readdir(d)) && num_items < MAX_ITEMS)
	{
		if (de->d_name[0] == '.') continue;
		char full[1024];
		snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
		struct stat st;
		if (stat(full, &st)) continue;
		int dir = S_ISDIR(st.st_mode);
		if (!dir && !is_media(de->d_name)) continue;
		snprintf(items[num_items].name, sizeof(items[0].name), "%s", de->d_name);
		items[num_items].dir = dir;
		num_items++;
	}
	closedir(d);
	qsort(items, num_items, sizeof(items[0]), item_cmp);
	return 0;
}

// Sources shown on the first page.
typedef struct
{
	char label[64];
	char path[256]; // empty for the disc
} source_t;

static source_t sources[16];
static int num_sources = 0;

static int dir_exists(const char *path)
{
	struct stat st;
	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static int dir_has_entries(const char *path)
{
	DIR *d = opendir(path);
	if (!d) return 0;
	struct dirent *de;
	int found = 0;
	while (!found && (de = readdir(d))) found = de->d_name[0] != '.';
	closedir(d);
	return found;
}

static void add_source(const char *label, const char *path)
{
	if (num_sources >= 16) return;
	snprintf(sources[num_sources].label, sizeof(sources[0].label), "%s", label);
	snprintf(sources[num_sources].path, sizeof(sources[0].path), "%s", path);
	num_sources++;
}

static void scan_sources(void)
{
	num_sources = 0;
	add_source("Disco nel lettore", "");
	if (dir_exists("/media/fat/Video")) add_source("SD: Video", "/media/fat/Video");
	if (dir_exists("/media/fat/Music")) add_source("SD: Musica", "/media/fat/Music");
	add_source("SD: tutta la scheda", "/media/fat");
	for (int i = 0; i < 8; i++)
	{
		char path[32], label[32];
		snprintf(path, sizeof(path), "/media/usb%d", i);
		if (!dir_has_entries(path)) continue;
		snprintf(label, sizeof(label), "USB %d", i);
		add_source(label, path);
	}
	if (dir_has_entries("/media/fat/cifs")) add_source("Rete (CIFS)", "/media/fat/cifs");
}

static void draw_browser(const char *title, const char *const *labels, const int *dirs, int count, int sel, const char *message)
{
	memset(fb_mem, 0, fb_len);
	int s = fb_w >= 1280 ? 2 : 1;
	int line_h = 12 * s;
	int mx = 24 * s;
	uint32_t white = fb_rgb(255, 255, 255), grey = fb_rgb(150, 150, 150), accent = fb_rgb(60, 110, 200);

	fb_text(mx, 16 * s, s + 1, "MiSTer MediaCenter", white);
	char line[256];
	fit_text(line, sizeof(line), title, (fb_w - 2 * mx) / (GLYPH_W * s));
	fb_text(mx, 40 * s, s, line, grey);

	int top = 60 * s;
	int bottom = fb_h - 30 * s;
	int rows = (bottom - top) / line_h;
	if (rows < 1) rows = 1;
	int first = sel - rows / 2;
	if (first > count - rows) first = count - rows;
	if (first < 0) first = 0;

	if (!count) fb_text(mx, top, s, message ? message : "Nessun file video o audio qui", grey);
	for (int i = first; i < count && i < first + rows; i++)
	{
		int y = top + (i - first) * line_h;
		if (i == sel) fb_fill_rect(mx - 4 * s, y - 2 * s, fb_w - 2 * mx + 8 * s, line_h, accent);
		char name[260];
		snprintf(name, sizeof(name), "%s%s", labels[i], dirs && dirs[i] ? "/" : "");
		fit_text(line, sizeof(line), name, (fb_w - 2 * mx) / (GLYPH_W * s) - 1);
		fb_text(mx, y, s, line, white);
	}

	const char *help = "A Apri   B Indietro   \x10 \x11 Pagina   X Esci";
	fb_text(mx, fb_h - 18 * s, s, help, grey);
	if (count > rows)
	{
		snprintf(line, sizeof(line), "%d/%d", sel + 1, count);
		fb_text(fb_w - mx - (int)strlen(line) * GLYPH_W * s, fb_h - 18 * s, s, line, grey);
	}
}

// Moves the selection with the D-pad. Returns the key when it is not a
// movement key, 0 otherwise.
static int menu_nav(int code, int *sel, int count, int page)
{
	switch (code)
	{
	case KEY_UP: if (*sel > 0) (*sel)--; else *sel = count - 1; return 0;
	case KEY_DOWN: if (*sel < count - 1) (*sel)++; else *sel = 0; return 0;
	case KEY_LEFT: *sel -= page; if (*sel < 0) *sel = 0; return 0;
	case KEY_RIGHT: *sel += page; if (*sel >= count) *sel = count - 1; return 0;
	default: return code;
	}
}

static int wait_key(void)
{
	while (!quit_requested)
	{
		int code = input_key(now_seconds());
		if (code) return code;
		usleep(20000);
	}
	return 0;
}

static int reopen_screen(void);

// Plays the disc in the drive: a Video CD with the Video CD player, a
// DVD-Video with the DVD player. Returns 0 when it was one of them.
static int browse(const char *root);

// A data disc (CD or DVD, -R or +R) with video and music files: the files
// are browsed like on a USB stick. The disc is mounted read-only unless
// Linux already did it.
#define DISC_MOUNT "/tmp/mediacenter_disc"

static int browse_data_disc(const char *device)
{
	char dir[256] = "";
	FILE *m = fopen("/proc/mounts", "r");
	if (m)
	{
		char dev[256], mnt[256];
		while (fscanf(m, "%255s %255s %*[^\n]", dev, mnt) == 2)
		{
			if (!strcmp(dev, device)) { snprintf(dir, sizeof(dir), "%s", mnt); break; }
		}
		fclose(m);
	}
	int mounted = 0;
	if (!dir[0])
	{
		mkdir(DISC_MOUNT, 0755);
		// UDF first: it keeps long names that the ISO 9660 side may cut
		static const char *const fs[] = { "udf", "iso9660" };
		for (unsigned i = 0; i < sizeof(fs) / sizeof(fs[0]) && !mounted; i++)
		{
			mounted = !mount(device, DISC_MOUNT, fs[i], MS_RDONLY, NULL);
		}
		if (!mounted)
		{
			fprintf(stderr, "mediacenter: %s is not a Video CD, a DVD or a data disc (%s)\n", device, strerror(errno));
			return -1;
		}
		snprintf(dir, sizeof(dir), "%s", DISC_MOUNT);
	}
	if (reopen_screen()) return -1;
	browse(dir);
	if (mounted) umount(DISC_MOUNT);
	return 0;
}

static int play_disc_in_drive(const char *device)
{
	char path[32];
	if (!device || !*device)
	{
		// the first drive present
		device = NULL;
		for (int i = 0; i < 8 && !device; i++)
		{
			snprintf(path, sizeof(path), "/dev/sr%d", i);
			if (!access(path, R_OK)) device = path;
		}
		if (!device) return -1;
	}
	int fd = open_drive(device);
	if (fd < 0) return -1;
	cd_fd = fd;
	const char *kind = (read_toc() == 0) ? detect_vcd_kind() : NULL;
	close(cd_fd);
	cd_fd = -1;

	if (!kind)
	{
		if (dvd_detect(device))
		{
			if (reopen_screen()) return -1;
			dvd_play(device);
			return 0;
		}
		return browse_data_disc(device);
	}

	// The Video CD player opens the framebuffer and the controls itself and
	// releases them when it ends.
	if (fb_mem) munmap(fb_mem, fb_len);
	fb_mem = NULL;
	if (input_fd >= 0) close(input_fd);
	input_fd = -1;

	char *argv[3] = { (char *)"vcd_player", (char *)device, NULL };
	int argc = 2;
	fb_aspect = 4.0 / 3.0;
	vcd_main(argc, argv);

	fb_mem = NULL;
	input_fd = -1;
	src_w = src_h = 0;
	return 0;
}

static int reopen_screen(void)
{
	if (!fb_mem && fb_open()) return -1;
	if (!ovl_a) osd_layout();
	input_open();
	return 0;
}

// A DVD copied to the card: an .iso image or a folder holding VIDEO_TS.
static int is_dvd_path(const char *path)
{
	char ifo[1100];
	struct stat st;
	if (stat(path, &st)) return 0;
	if (!S_ISDIR(st.st_mode)) return has_ext(path, (const char *const[]){ "iso", NULL });
	snprintf(ifo, sizeof(ifo), "%s/VIDEO_TS/VIDEO_TS.IFO", path);
	if (!access(ifo, R_OK)) return 1;
	snprintf(ifo, sizeof(ifo), "%s/VIDEO_TS.IFO", path);
	return !access(ifo, R_OK);
}

static int play_file(const char *path, int in_playlist)
{
	if (is_dvd_path(path)) return dvd_play(path);
	if (media_open(path)) return END_DONE;
	native_fb = !mc.audio_only && !getenv("MC_ARMSCALE");
	int r = media_play(in_playlist);
	native_fb = 0;
	fb_native(0, 0, 0);
	media_close();
	return r;
}

// Browses a directory tree below root. Returns 1 to quit the application.
static int browse(const char *root)
{
	char cwd[1024];
	snprintf(cwd, sizeof(cwd), "%s", root);
	int sel = 0;

	for (;;)
	{
		if (list_dir(cwd)) return 0;
		if (sel >= num_items) sel = num_items ? num_items - 1 : 0;

		const char *labels[MAX_ITEMS];
		int dirs[MAX_ITEMS];
		for (int i = 0; i < num_items; i++)
		{
			labels[i] = items[i].name;
			dirs[i] = items[i].dir;
		}

		int s = fb_w >= 1280 ? 2 : 1;
		int page = (fb_h - 90 * s) / (12 * s);
		draw_browser(cwd, labels, dirs, num_items, sel, NULL);
		int code = wait_key();
		if (!code || quit_requested) return 1;
		code = menu_nav(code, &sel, num_items, page > 1 ? page : 1);
		if (!code) continue;

		if (code == KEY_TAB) return 1;
		if (code == KEY_ESC || code == KEY_BACKSPACE)
		{
			if (!strcmp(cwd, root)) return 0;
			char *slash = strrchr(cwd, '/');
			char last[256];
			snprintf(last, sizeof(last), "%s", slash + 1);
			*slash = 0;
			list_dir(cwd);
			sel = 0;
			for (int i = 0; i < num_items; i++)
				if (!strcmp(items[i].name, last)) sel = i;
			continue;
		}
		if ((code == KEY_ENTER || code == KEY_PLAY || code == KEY_SPACE) && num_items)
		{
			if (items[sel].dir)
			{
				char path[1280];
				snprintf(path, sizeof(path), "%s/%s", cwd, items[sel].name);
				if (is_dvd_path(path))
				{
					if (play_file(path, 0) == END_QUIT) return 1;
					continue;
				}
				size_t len = strlen(cwd);
				snprintf(cwd + len, sizeof(cwd) - len, "/%s", items[sel].name);
				sel = 0;
				continue;
			}

			// Audio files play as an album: the following audio files of the
			// folder play next.
			int i = sel;
			while (i >= 0 && i < num_items && !quit_requested)
			{
				char path[1280];
				snprintf(path, sizeof(path), "%s/%s", cwd, items[i].name);
				int audio = is_audio_file(items[i].name);
				int r = play_file(path, audio);
				if (r == END_QUIT) return 1;
				sel = i;
				if (!audio || r == END_BACK || r == END_DONE) break;
				int dir = r == END_PREV ? -1 : 1;
				do i += dir; while (i >= 0 && i < num_items && (items[i].dir || !is_audio_file(items[i].name)));
			}
		}
	}
}

static int run_browser(void)
{
	int sel = 0;
	while (!quit_requested)
	{
		scan_sources();
		const char *labels[16];
		for (int i = 0; i < num_sources; i++) labels[i] = sources[i].label;
		if (sel >= num_sources) sel = 0;
		draw_browser("Scegli la sorgente", labels, NULL, num_sources, sel, NULL);

		int code = wait_key();
		if (!code) break;
		code = menu_nav(code, &sel, num_sources, 1);
		if (!code) continue;
		if (code == KEY_TAB || code == KEY_ESC) break;
		if (code != KEY_ENTER && code != KEY_PLAY && code != KEY_SPACE) continue;

		if (!sources[sel].path[0])
		{
			if (play_disc_in_drive(NULL))
			{
				draw_browser("Scegli la sorgente", labels, NULL, num_sources, sel, NULL);
				fb_text(24, fb_h / 2, fb_w >= 1280 ? 2 : 1, "Nessun disco leggibile nel lettore", fb_rgb(255, 200, 80));
				sleep(2);
			}
			quit_requested = 0;
			if (reopen_screen()) return 1;
			continue;
		}
		if (browse(sources[sel].path)) break;
	}
	return 0;
}

// mediacenter --bench <file>: decodes every picture and sample as fast as
// possible and reports the speed against real time, to tell whether a format
// can play on this CPU.
static int bench(const char *path)
{
	if (!strncmp(path, "/dev/", 5) || is_dvd_path(path)) return dvd_bench(path);
	if (media_open(path)) return 1;
	int frames = 0;
	double t0 = now_seconds();
	uint8_t *rgb = NULL;
	for (int more = 1; more;)
	{
		more = decode_step();
		// include the colour conversion, which the player does for every
		// picture it shows
		for (int i = 0; i < mc.vq_count; i++)
		{
			AVFrame *f = mc.vq[i].frame;
			if (!rgb) rgb = (uint8_t *)av_malloc((size_t)((f->width * 4 + 63) & ~63) * f->height);
			mc.sws = sws_getCachedContext(mc.sws, f->width, f->height, (enum AVPixelFormat)f->format,
				f->width, f->height, AV_PIX_FMT_BGRA, SWS_POINT, NULL, NULL, NULL);
			uint8_t *dst[4] = { rgb, NULL, NULL, NULL };
			int stride[4] = { (f->width * 4 + 63) & ~63, 0, 0, 0 };
			if (mc.sws && rgb) sws_scale(mc.sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, stride);
		}
		frames += mc.vq_count;
		vq_clear();
		mc.pend_len = 0;
	}
	av_free(rgb);
	double secs = now_seconds() - t0;
	double media = !isnan(mc.last_vpts) ? mc.last_vpts : mc.audio_end;
	printf("mediacenter: decoded %d pictures, %.1f s of media in %.1f s: %.0f%% of real time%s\n",
		frames, media, secs, secs > 0 ? 100.0 * media / secs : 0,
		secs > 0 && media / secs >= 1.0 ? " (plays)" : " (too slow)");
	media_close();
	return 0;
}

// Headless test mode for development (no screen, no controller):
//   MC_TEST="<seconds>;<t>:<key>,...;<t>,..." mediacenter <file|dvd>
// plays for <seconds> into a 640x480 memory framebuffer, presses the keys at
// the given times (enter, esc, up, down, left, right, space, tab, play, pause,
// pgup, pgdn) and saves the screen as /tmp/mc_<t>.ppm at the listed times.
static double test_start, test_end;
static char test_keys[512], test_shots[512];

static int test_key_code(const char *name)
{
	static const struct { const char *name; int code; } keys[] = {
		{ "enter", KEY_ENTER }, { "esc", KEY_ESC }, { "up", KEY_UP }, { "down", KEY_DOWN },
		{ "left", KEY_LEFT }, { "right", KEY_RIGHT }, { "space", KEY_SPACE }, { "tab", KEY_TAB },
		{ "play", KEY_PLAY }, { "pause", KEY_PAUSE }, { "pgup", KEY_PAGEUP }, { "pgdn", KEY_PAGEDOWN },
	};
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (!strcmp(name, keys[i].name)) return keys[i].code;
	return 0;
}

static int test_input(double now)
{
	double t = now - test_start;
	if (t >= test_end) quit_requested = 1;

	// screenshots
	char *p = test_shots;
	while (*p)
	{
		char *end;
		double at = strtod(p, &end);
		if (end == p) break;
		char *next = *end == ',' ? end + 1 : end;
		if (t >= at)
		{
			char name[64];
			snprintf(name, sizeof(name), "/tmp/mc_%g.ppm", at);
			FILE *f = fopen(name, "wb");
			if (f)
			{
				fprintf(f, "P6 %d %d 255\n", fb_w, fb_h);
				for (int y = 0; y < fb_h; y++)
					for (int x = 0; x < fb_w; x++)
					{
						const uint8_t *px = fb_mem + (size_t)y * fb_stride + x * 4;
						uint8_t rgb[3] = { px[fb_bgr ? 2 : 0], px[1], px[fb_bgr ? 0 : 2] };
						fwrite(rgb, 1, 3, f);
					}
				fclose(f);
				printf("test: %.1f s screen saved to %s\n", t, name);
			}
			memmove(p, next, strlen(next) + 1);
			continue;
		}
		p = next;
	}

	// keys, in order
	char *end;
	double at = strtod(test_keys, &end);
	if (end != test_keys && *end == ':' && t >= at)
	{
		char name[16];
		int n = 0;
		end++;
		while (*end && *end != ',' && n < 15) name[n++] = *end++;
		name[n] = 0;
		memmove(test_keys, *end ? end + 1 : end, strlen(*end ? end + 1 : end) + 1);
		printf("test: %.1f s key %s\n", t, name);
		return test_key_code(name);
	}
	return 0;
}

static int test_setup(const char *spec)
{
	if (sscanf(spec, "%lf", &test_end) != 1) return -1;
	const char *k = strchr(spec, ';');
	const char *sh = k ? strchr(k + 1, ';') : NULL;
	if (k) snprintf(test_keys, sizeof(test_keys), "%.*s", (int)(sh ? sh - k - 1 : (int)strlen(k + 1)), k + 1);
	if (sh) snprintf(test_shots, sizeof(test_shots), "%s", sh + 1);
	fb_w = 640;
	fb_h = 480;
	fb_stride = fb_w * 4;
	fb_len = (size_t)fb_stride * fb_h;
	fb_bgr = 1;
	fb_mem = (uint8_t *)calloc(1, fb_len);
	fb_fake = 1;
	test_start = now_seconds();
	input_hook = test_input;
	return fb_mem ? 0 : -1;
}

int main(int argc, char *argv[])
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
	av_log_set_level(AV_LOG_ERROR);
	mc.vstream = mc.astream = -1;

	const char *arg = argc > 1 ? argv[1] : NULL;

	// Decoding speed test: decodes the whole file without output.
	if (arg && !strcmp(arg, "--bench") && argc > 2) return bench(argv[2]);

	// A drive: Auto Disc Detection launched us for the inserted disc.
	if (arg && !strncmp(arg, "/dev/", 5))
	{
		return play_disc_in_drive(arg) ? 1 : 0;
	}

	const char *test = getenv("MC_TEST");
	if (test)
	{
		if (test_setup(test)) return 1;
		osd_layout();
		int r = play_file(arg, 0);
		printf("test: ended with %d\n", r);
		return 0;
	}

	if (fb_open())
	{
		fprintf(stderr, "mediacenter: 32-bit /dev/fb0 not available\n");
		return 1;
	}
	osd_layout();
	input_open();

	int rc = 0;
	if (arg) rc = play_file(arg, 0) == END_DONE ? 0 : 0;
	else rc = run_browser();

	if (fb_mem)
	{
		memset(fb_mem, 0, fb_len);
		munmap(fb_mem, fb_len);
	}
	if (input_fd >= 0) close(input_fd);
	return rc;
}
