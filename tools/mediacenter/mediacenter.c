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

// The display aspect of the video playing (fb_aspect is the one drawn: this
// one, or the format fixed in Regola CRT).
static double video_aspect = 4.0 / 3.0;
#undef main

#include <dirent.h>
#include <sched.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "accel.c"

#define MC_AUDIO_RATE 48000
#define MC_AUDIO_LEAD 0.3
#define MC_VQ_MAX 64  // pictures the queue can hold
#define MC_VQ_FILE 12 // pictures decoded ahead for files
#define MC_VQ_ACC 48  // with the FPGA accelerator (1080p): a deeper reserve for the heavy moments
#define MC_ACC_SPARE 10 // FPGA picture slots kept free for the pictures being decoded
#define MC_PREROLL 2.0 // seconds the start waits for the reserve, at most
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

static const char *const video_ext[] = { "avi", "divx", "xvid", "mkv", "mp4", "m4v", "mov", "mpg", "mpeg", "vob", "dat", "ts", "m2ts", "mts", "ogm", "ogv", "wmv", "asf", "iso", "cue", NULL };
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
	double read_vpts; // time of the last video packet handed to the thread
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
// Light mode (pictures taller than 720 lines): only every other line is
// converted and drawn; the FPGA scaler enlarges the half-height picture.
static int half_lines = 0;

// --zctest: the decoder's pictures in a pool of locked memory (zctest.c)
static int (*zc_get_buffer2)(AVCodecContext *, AVFrame *, int) = NULL;

// zero-copy (zerocopy.c)
static void zcp_open(AVCodecContext *ctx, int width, int half);
static void zcp_close(void);
static void zcp_mark(const AVFrame *f);
static int zcp_get_buffer(AVCodecContext *s, AVFrame *f, int flags);

// FPGA accelerator (accel.c, accel_link.c)
// it gave up on this file (1), or only while the picture format is one the
// core cannot draw (2: it comes back when the format can be drawn again)
static int acc_file_off;
static int acc_by_layout;      // the failure is the picture format's
static int acc_layout_ok;      // the format drawn last can be drawn by the core
static double acc_layout_t;    // since when
static void acc_layout_note(int ok)
{
	if (ok && !acc_layout_ok) acc_layout_t = now_seconds();
	acc_layout_ok = ok;
}
// the accelerator stops because of the picture format (the Start key)
static void acc_fail_layout(const char *why)
{
	if (!acc.failed) acc_by_layout = 1;
	acc_fail(NULL, why);
}
static void acc_try(AVCodecContext *ctx, const AVStream *st);
static void acc_close(void);
static void acc_seek(void);
static void acc_report(void);

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
	if (zc_get_buffer2 && ctx->codec_type == AVMEDIA_TYPE_VIDEO) ctx->get_buffer2 = zc_get_buffer2;
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
		// 720p keeps all its lines; 1080p is drawn with every other line
		half_lines = st->codecpar->height > 720;
		fprintf(stderr, "mediacenter: %dx%d %s, light decoding\n", st->codecpar->width, st->codecpar->height, codec->name);
		if (!zc_get_buffer2) zcp_open(ctx, st->codecpar->width, half_lines);
		acc_try(ctx, st);
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

// Super Video CD: the track being played is read from the disc through
// media_io instead of a file, and its chapters come from ENTRIES.SVD.
static AVIOContext *media_io = NULL;
static int svcd_on = 0;
// The disc in the drive played through FFmpeg: "Super Video CD" or "Video CD".
static const char *svcd_kind = "Super Video CD";
static double svcd_chapter[MAX_ENTRIES];
static int svcd_chapters = 0;

// Time of the end of the sound sent to aplay; NAN before any.
static double sent_end = NAN;
static double av_pause; // seconds left without sync (after an audio track change)
static double av_waited; // clock time held back for the sound, in the log
static double audio_late_drop, audio_full_drop; // sound thrown away, seconds, in the log
static int av_gaps, av_giveups; // sync skipped for a gap / gave up waiting, in the log

// The picture shown in a YUV framebuffer (a reference to the decoded frame)
// and whether it is shown every other line (light mode).
static AVFrame *yuv_last;
static int yuv_half;
static int yuv_stream = -1;  // the file's pictures go out as YUV: -1 not decided yet

// A DVD picture in a YUV framebuffer (yuv_dvd): copied line by line by
// yuv_dvd_draw with the chroma of each line's own field (yuv_ilace), each
// line blended with the next (yuv_deint, unless MC_FIELDS keeps the fields
// apart), and the DVD's subtitles and buttons blended in YUV by dvd.c.
static int yuv_dvd, yuv_ilace, yuv_deint;
static void (*yuv_ovl_begin)(void), (*yuv_ovl_end)(void);
static void (*yuv_ovl_row)(uint8_t *row, int y, int w, int shift);

static void media_close(void)
{
	if (yuv_last) av_frame_unref(yuv_last);
	vq_clear();
	if (mc.vdec) avcodec_free_context(&mc.vdec);
	if (mc.adec) avcodec_free_context(&mc.adec);
	swr_free(&mc.swr);
	sws_freeContext(mc.sws);
	av_packet_free(&mc.pkt);
	av_frame_free(&mc.frame);
	acc_close();
	zcp_close();
	if (mc.fmt) avformat_close_input(&mc.fmt);
	free(mc.pend);
	memset(&mc, 0, sizeof(mc));
	mc.vstream = mc.astream = -1;
}

static int media_open(const char *path)
{
	yuv_stream = -1;
	sent_end = NAN;
	skip_base = AVDISCARD_DEFAULT;
	half_lines = 0;
	media_close();
	acc_file_off = 0;
	acc_by_layout = 0;
	acc_layout_ok = 0;
	if (media_io)
	{
		mc.fmt = avformat_alloc_context();
		if (mc.fmt) mc.fmt->pb = media_io;
	}
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
	mc.last_vpts = mc.read_vpts = NAN;

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
		// Video CD pictures are 4:3 whatever their MPEG-1 header says, as in
		// the Video CD player.
		if (svcd_on && st->codecpar->codec_id == AV_CODEC_ID_MPEG1VIDEO) w = h * 4.0 / 3.0;
		video_aspect = fb_aspect = (h > 0 && w > 0) ? w / h : 4.0 / 3.0;
		printf("mediacenter: %s, video %s %dx%d aspect %.3f, audio %s, %.0f s", path,
			avcodec_get_name(st->codecpar->codec_id), mc.vdec->width, mc.vdec->height, video_aspect,
			mc.adec ? avcodec_get_name(mc.adec->codec_id) : "none", mc.duration);
		// the bitrate the file declares: all streams, and the video alone
		// when the file says it
		if (mc.fmt->bit_rate > 0) printf(", bitrate %.2f Mbit/s", mc.fmt->bit_rate / 1e6);
		if (st->codecpar->bit_rate > 0) printf(" (video %.2f Mbit/s)", st->codecpar->bit_rate / 1e6);
		printf("\n");
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
	if (room <= 0) { audio_full_drop += f->nb_samples / (double)f->sample_rate; return; }
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
// Pictures decoded ahead: with the FPGA accelerator (1080p, the ARM close
// to its limit at 30 pictures a second) a deeper queue absorbs the runs of
// slow pictures, which otherwise end in pictures skipped or dropped late.
static int vq_depth(void)
{
	return acc.on ? MC_VQ_ACC : MC_VQ_FILE;
}

// The queue is full. With the accelerator every picture in it holds a slot
// of the FPGA memory (64 slots, the decoder's reference pictures take some
// too): it is also full while fewer than MC_ACC_SPARE slots are free.
static int vq_full(void)
{
	if (mc.vq_count >= vq_depth()) return 1;
	return acc.on && ACC_SLOTS - __builtin_popcountll(__atomic_load_n(&acc.slots_used, __ATOMIC_SEQ_CST)) < MC_ACC_SPARE;
}

// At the start (and after fast forward / rewind) with the accelerator the
// clock and the sound wait until the queue is full (MC_VQ_ACC pictures
// ahead), for MC_PREROLL seconds at most: the queue starts full instead of
// empty.
static int preroll;
static double preroll_t0;

static void feed_audio_output(void)
{
	if (!mc.pend_len || preroll) return;

	// drop what is already late (after a seek or a stall)
	int late = (int)((mc.clock - 0.05 - mc.pend_pts) * MC_AUDIO_RATE);
	if (late > 0)
	{
		if (late > mc.pend_len) late = mc.pend_len;
		audio_late_drop += late / (double)MC_AUDIO_RATE;
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
	if (audio_pipe) sent_end = mc.pend_pts;
}

// The sound is the reference. The clock runs on the system time, but the
// sound comes out only after everything queued before it: aplay starting,
// the sound still queued after a seek, a slow output. When the sound heard
// now is behind the clock, the clock waits for it (the pictures wait, the
// sound is never cut). MC_AV_OFFSET (seconds) moves the sound later (+) or
// earlier (-) for a TV that delays the picture.
static void av_sync(double elapsed)
{
	static double offset = NAN;
	if (isnan(offset))
	{
		const char *e = getenv("MC_AV_OFFSET");
		offset = e ? atof(e) : 0;
	}
	static double held; // seconds the clock has been held without a break
	if (av_pause > 0)
	{
		// a new audio track: its sound settles first (aplay restarting
		// after the silence, the decoder warming up). Holding the pictures
		// meanwhile made them stutter for seconds.
		av_pause -= elapsed;
		held = 0;
		return;
	}
	if (!audio_pipe || mc.astream < 0 || isnan(sent_end) || (mc.eof && !mc.pend_len)) { held = 0; return; }
	// a gap in the sound (a new audio track starting later, a pause in the
	// stream) is silence: the clock runs through it, otherwise the sound
	// after the gap would never become due and playback would stop
	if (mc.pend_len && mc.pend_pts > sent_end + 0.1) { held = 0; av_gaps++; return; }
	double heard = sent_end - audio_queued() - offset;
	double late = mc.clock - heard;
	if (late > 0.03)
	{
		// slow the pictures down (by a quarter at most) rather than stop
		// them: a stop is seen as a stutter, a slowdown is not
		double c = late - 0.02;
		if (c > elapsed * 0.25) c = elapsed * 0.25;
		held += c;
		if (held > 0.5)
		{
			// no sound is coming: stop waiting until some is sent again
			sent_end = NAN;
			held = 0;
			av_giveups++;
			return;
		}
		mc.clock -= c;
		av_waited += c;
	}
	else held = 0;
}

static void av_report(void)
{
	printf("mediacenter: pictures waited %.2f s for the sound\n", av_waited);
	printf("mediacenter: sound dropped late %.2f s, dropped with buffer full %.2f s, gaps %d, gave up waiting %d\n",
		audio_late_drop, audio_full_drop, av_gaps, av_giveups);
}

// Queues a decoded picture to show at pts (NAN: one frame after the previous).
static void queue_video_at(AVFrame *f, double pts, double frame_time)
{
	if (isnan(pts)) pts = isnan(mc.last_vpts) ? 0 : mc.last_vpts + frame_time;
	mc.last_vpts = pts;
	zcp_mark(f);
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

// Video decoding thread for files. The main loop reads the file and decodes
// the sound, which is cheap; the pictures, which with HD H.264 can take longer
// than they last, are decoded here so they never hold the sound up.
#define VT_MAX 400 // video packets waiting to be decoded

static struct
{
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int on, run;
	int direct;           // decode in the caller (fast forward previews)
	AVPacket *q[VT_MAX + 1]; // a NULL entry ends the stream
	int head, count;
	int busy;             // a packet is being decoded outside the lock
	int drained;          // the end of the stream has been decoded
	int wait_key;         // queue overflowed: drop packets until a key picture
	int key_sent;         // a key picture was sent while skipping all others
	int frames, key_frames; // pictures queued; their count when it was sent
	double clock;         // copy of mc.clock for the thread
} vt = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void preroll_start(void)
{
	preroll = acc.on && vt.on;
	preroll_t0 = now_seconds();
}

// Diagnosis of the decoder's time (MC_DIAG=0 turns it off; nothing here
// changes the playback): the time each packet spends in the decoder, counted
// only for packets decoded normally (not while skipping to catch up), with
// a line every 10 s and a summary at the end.
static struct
{
	int on;
	double budget;             // time per picture (1 / fps)
	double sum, max;           // whole video
	int n, over, skipped;
	double w_sum, w_max, w_start; // current 10 s window
	int w_n, w_over, w_skipped, w_shown, w_dropped;
	// video bitrate: bytes of the packets sent to the decoder over their
	// running time (one picture each), per window and its highest value
	double bytes, w_bytes, rate_max;
	int pkts, w_pkts;
	// peaks: pictures over twice the time per picture, and the longest run
	// of pictures in a row over the time per picture
	int peaks, run, run_max, w_peaks, w_run_max;
} dd;

static void dec_diag_reset(void)
{
	const char *e = getenv("MC_DIAG");
	memset(&dd, 0, sizeof(dd));
	dd.on = !(e && !strcmp(e, "0"));
	double fps = mc.vstream >= 0 ? av_q2d(mc.fmt->streams[mc.vstream]->avg_frame_rate) : 0;
	dd.budget = fps > 1 ? 1.0 / fps : 0.04;
}

static int frames_shown, frames_dropped; // reported in the log

static void dec_diag_add(double t0, double t1, int skipping, int size)
{
	if (!dd.on) return;
	if (!dd.w_start) dd.w_start = t0;
	dd.bytes += size;
	dd.w_bytes += size;
	dd.pkts++;
	dd.w_pkts++;
	if (skipping)
	{
		dd.skipped++;
		dd.w_skipped++;
	}
	else
	{
		double d = t1 - t0;
		dd.sum += d;
		dd.w_sum += d;
		dd.n++;
		dd.w_n++;
		if (d > dd.max) dd.max = d;
		if (d > dd.w_max) dd.w_max = d;
		if (d > dd.budget) { dd.over++; dd.w_over++; dd.run++; }
		else dd.run = 0;
		if (d > 2 * dd.budget) { dd.peaks++; dd.w_peaks++; }
		if (dd.run > dd.run_max) dd.run_max = dd.run;
		if (dd.run > dd.w_run_max) dd.w_run_max = dd.run;
	}
	if (t1 - dd.w_start >= 10)
	{
		double rate = dd.w_pkts ? 8 * dd.w_bytes / (dd.w_pkts * dd.budget) : 0;
		if (rate > dd.rate_max) dd.rate_max = rate;
		printf("mediacenter: diag decoder %.0f s: %d immagini decodificate, media %.1f ms, peggiore %.1f ms, %d oltre %.1f ms, "
			"%d saltate per recuperare; mostrate %d, scartate in ritardo %d; picchi oltre %.1f ms %d, serie piu' lunga oltre %.1f ms %d; "
			"bitrate video %.2f Mbit/s\n",
			mc.clock, dd.w_n, dd.w_n ? 1000 * dd.w_sum / dd.w_n : 0, 1000 * dd.w_max, dd.w_over, 1000 * dd.budget,
			dd.w_skipped, frames_shown - dd.w_shown, frames_dropped - dd.w_dropped,
			2000 * dd.budget, dd.w_peaks, 1000 * dd.budget, dd.w_run_max, rate / 1e6);
		dd.w_bytes = 0;
		dd.w_pkts = 0;
		dd.w_peaks = dd.w_run_max = 0;
		dd.w_shown = frames_shown;
		dd.w_dropped = frames_dropped;
		dd.w_sum = dd.w_max = 0;
		dd.w_n = dd.w_over = dd.w_skipped = 0;
		dd.w_start = t1;
	}
}

// Diagnosis of the main loop, which shows the pictures (MC_DIAG=0: off;
// nothing here changes the playback): a picture is "dropped late" when two
// are due in the same round, so a round longer than a picture's time costs
// one. Every 10 s: the rounds, where the drawing time goes (waiting for a
// free framebuffer page, the drawing itself) and how many decoded pictures
// were ready ahead when one was shown.
static struct
{
	double w_start;
	int rounds, slow;          // rounds; those longer than a picture's time
	double round_max;
	int drawn;
	double draw_sum, draw_max, wait_sum, wait_max;
	int ready_min, ready_sum, ready_n, empty;
	int s_slow, s_drawn, s_empty; // whole video
	double s_draw, s_wait;
} ld;
static double fb_page_waited; // set by fb_present_paged

static void loop_diag_reset(void)
{
	memset(&ld, 0, sizeof(ld));
	ld.ready_min = -1;
}

static void loop_diag_round(double t, double round)
{
	if (!dd.on) return;
	if (!ld.w_start) ld.w_start = t;
	ld.rounds++;
	if (round > dd.budget) { ld.slow++; ld.s_slow++; }
	if (round > ld.round_max) ld.round_max = round;
	if (t - ld.w_start < 10) return;
	printf("mediacenter: diag ciclo %.0f s: %d giri, %d oltre %.1f ms (peggiore %.1f ms); disegnate %d, disegno medio %.1f ms "
		"(attesa pagina libera %.1f ms, peggiore disegno %.1f ms, peggiore attesa %.1f ms); pronte in coda minimo %d, media %.1f, vuota %d volte\n",
		mc.clock, ld.rounds, ld.slow, 1000 * dd.budget, 1000 * ld.round_max, ld.drawn,
		ld.drawn ? 1000 * ld.draw_sum / ld.drawn : 0, ld.drawn ? 1000 * ld.wait_sum / ld.drawn : 0,
		1000 * ld.draw_max, 1000 * ld.wait_max, ld.ready_min < 0 ? 0 : ld.ready_min,
		ld.ready_n ? (double)ld.ready_sum / ld.ready_n : 0, ld.empty);
	ld.w_start = t;
	ld.rounds = ld.slow = ld.drawn = ld.ready_n = ld.ready_sum = ld.empty = 0;
	ld.round_max = ld.draw_sum = ld.draw_max = ld.wait_sum = ld.wait_max = 0;
	ld.ready_min = -1;
}

// a picture was shown, with `ready` decoded pictures left in the queue
static void loop_diag_shown(int ready)
{
	if (!dd.on) return;
	if (ld.ready_min < 0 || ready < ld.ready_min) ld.ready_min = ready;
	ld.ready_sum += ready;
	ld.ready_n++;
	if (!ready) { ld.empty++; ld.s_empty++; }
}

static void loop_diag_drawn(double d)
{
	if (!dd.on) return;
	ld.drawn++;
	ld.s_drawn++;
	ld.draw_sum += d;
	ld.s_draw += d;
	if (d > ld.draw_max) ld.draw_max = d;
	ld.wait_sum += fb_page_waited;
	ld.s_wait += fb_page_waited;
	if (fb_page_waited > ld.wait_max) ld.wait_max = fb_page_waited;
}

static void *video_thread(void *arg)
{
	(void)arg;
	AVFrame *frame = av_frame_alloc();
	pthread_mutex_lock(&vt.lock);
	while (vt.run && frame)
	{
		if (!vt.count || vq_full())
		{
			pthread_cond_wait(&vt.cond, &vt.lock);
			continue;
		}
		AVPacket *pkt = vt.q[vt.head];
		vt.head = (vt.head + 1) % (VT_MAX + 1);
		vt.count--;
		// Behind the sound: skip pictures nothing refers to, and when far
		// behind decode only key pictures until the pictures catch up.
		double lag = isnan(mc.last_vpts) ? 0 : vt.clock - mc.last_vpts;
		enum AVDiscard skip = skip_base;
		// Light mode at half the lines (1080p) has no time to spare: skip
		// them sooner, before decoding pictures that would be dropped late.
		if (lag > (half_lines ? 0.1 : 0.3) && skip < AVDISCARD_NONREF) skip = AVDISCARD_NONREF;
		// The decoder hands a key picture over only after decoding the
		// pictures that follow it: after sending one, decode those again.
		if (vt.key_sent && vt.frames != vt.key_frames) vt.key_sent = 0;
		if (lag > 1.5 && !vt.key_sent)
		{
			skip = AVDISCARD_NONKEY;
			if (pkt && (pkt->flags & AV_PKT_FLAG_KEY))
			{
				vt.key_sent = 1;
				vt.key_frames = vt.frames;
			}
		}
		vt.busy = 1;
		pthread_mutex_unlock(&vt.lock);

		double dec_t0 = now_seconds();
		mc.vdec->skip_frame = skip;
		if (avcodec_send_packet(mc.vdec, pkt) == AVERROR(EAGAIN))
		{
			// the decoder holds pictures it must hand over first
			while (avcodec_receive_frame(mc.vdec, frame) == 0)
			{
				pthread_mutex_lock(&vt.lock);
				queue_video(frame);
				vt.frames++;
				pthread_mutex_unlock(&vt.lock);
				av_frame_unref(frame);
			}
			avcodec_send_packet(mc.vdec, pkt);
		}
		while (avcodec_receive_frame(mc.vdec, frame) == 0)
		{
			pthread_mutex_lock(&vt.lock);
			queue_video(frame);
			vt.frames++;
			pthread_mutex_unlock(&vt.lock);
			av_frame_unref(frame);
		}
		if (pkt) dec_diag_add(dec_t0, now_seconds(), skip != skip_base, pkt->size);

		pthread_mutex_lock(&vt.lock);
		if (!pkt) vt.drained = 1;
		av_packet_free(&pkt);
		vt.busy = 0;
		pthread_cond_broadcast(&vt.cond);
	}
	pthread_mutex_unlock(&vt.lock);
	av_frame_free(&frame);
	return NULL;
}

// Hands a video packet (NULL: the end of the stream) to the thread.
static void vt_push(AVPacket *pkt)
{
	AVPacket *copy = NULL;
	if (pkt)
	{
		copy = av_packet_alloc();
		if (!copy) return;
		av_packet_move_ref(copy, pkt);
	}
	pthread_mutex_lock(&vt.lock);
	if (copy && vt.count >= VT_MAX) vt.wait_key = 1;
	if (copy && vt.wait_key && (!(copy->flags & AV_PKT_FLAG_KEY) || vt.count >= VT_MAX))
	{
		pthread_mutex_unlock(&vt.lock);
		av_packet_free(&copy);
		return;
	}
	vt.wait_key = 0;
	if (vt.count <= VT_MAX)
	{
		vt.q[(vt.head + vt.count) % (VT_MAX + 1)] = copy;
		vt.count++;
	}
	else av_packet_free(&copy);
	pthread_cond_broadcast(&vt.cond);
	pthread_mutex_unlock(&vt.lock);
}

// Drops the queued packets and waits for the thread to be idle. Returns with
// the lock held, so the caller can reset the decoder and the picture queue.
static void vt_stop_and_lock(void)
{
	pthread_mutex_lock(&vt.lock);
	while (vt.busy) pthread_cond_wait(&vt.cond, &vt.lock);
	while (vt.count)
	{
		av_packet_free(&vt.q[vt.head]);
		vt.head = (vt.head + 1) % (VT_MAX + 1);
		vt.count--;
	}
	vt.wait_key = 0;
	vt.key_sent = 0;
	vt.drained = 0;
}

static void vt_start(void)
{
	if (!mc.vdec || vt.on) return;
	vt.head = vt.count = vt.busy = vt.drained = vt.wait_key = vt.direct = vt.key_sent = 0;
	vt.clock = mc.clock;
	vt.run = 1;
	if (!pthread_create(&vt.thread, NULL, video_thread, NULL)) vt.on = 1;
}

static void vt_end(void)
{
	if (!vt.on) return;
	vt_stop_and_lock();
	vt.run = 0;
	pthread_cond_broadcast(&vt.cond);
	pthread_mutex_unlock(&vt.lock);
	pthread_join(vt.thread, NULL);
	vt.on = 0;
}

// Reads and decodes one packet. Returns 0 at the end of the file.
static int decode_step(void)
{
	if (mc.eof)
	{
		if (!mc.flushed)
		{
			mc.flushed = 1;
			if (mc.vdec && vt.on && !vt.direct) vt_push(NULL);
			else if (mc.vdec) { avcodec_send_packet(mc.vdec, NULL); receive_frames(mc.vdec, 1); }
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
	if (mc.pkt->stream_index == mc.vstream && mc.vdec && vt.on && !vt.direct)
	{
		int64_t ts = mc.pkt->dts != AV_NOPTS_VALUE ? mc.pkt->dts : mc.pkt->pts;
		if (ts != AV_NOPTS_VALUE) mc.read_vpts = ts_seconds(ts, mc.fmt->streams[mc.vstream]->time_base) - mc.start;
		vt_push(mc.pkt);
	}
	else if (mc.pkt->stream_index == mc.vstream && mc.vdec)
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
	if (mc.vstream >= 0 && vt.on)
	{
		// keep a few packets ready for the video thread; reading further
		// ahead is driven by the sound
		// (not beyond a second ahead, 2.5 s with the accelerator's deeper
		// queue: when only key pictures are decoded the thread empties its
		// queue at once)
		pthread_mutex_lock(&vt.lock);
		int more = vt.count < 8 && !vq_full() && !(mc.read_vpts > mc.clock + (acc.on ? 2.5 : 1.0));
		pthread_mutex_unlock(&vt.lock);
		if (more) return 1;
	}
	else if (mc.vstream >= 0)
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
static int overlay_soften = 0; // the overlay blends each of its lines with the one above

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
// Main answers a mode late while it is busy (switching the CRT mode, say):
// that turns the special frame buffers off only until the next video
// (native_retry), and for good after 3 misses in a row (a Main without them).
static int native_fails, yuv_fails;
static double crt_mode_sent_t;   // when the CRT mode was last changed
static int fb_fake = 0;          // test mode: memory framebuffer
static int def_w, def_h;         // the normal framebuffer
static int fb_menu, menu_asking; // the lists' own frame buffer is in use (menu_fb)

// With a MiSTer_MediaCenter Main, a DVD-size or light-mode frame buffer has
// three pages: each picture is drawn into a page the scaler is not showing
// and then shown at the next vsync, so the picture never tears. fb_mem is
// the page shown last.
#define FB_PAGES 3
static uint8_t *fb_map;          // the whole mapping, all the pages
static size_t fb_map_len;
static int fb_pages = 1, fb_page;
static int fb_cmd_fd = -1;

static int fb_open_pages(int page_h)
{
	if (fb_open()) return -1;
	fb_map = fb_mem;
	fb_map_len = fb_len;
	fb_pages = 1;
	fb_page = 0;
	if (page_h && fb_h == page_h * FB_PAGES)
	{
		fb_pages = FB_PAGES;
		fb_h = page_h;
		fb_len = (size_t)fb_stride * fb_h;
	}
	return 0;
}

static void fb_close(void)
{
	if (fb_mem && !fb_fake) munmap(fb_map ? fb_map : fb_mem, fb_map ? fb_map_len : fb_len);
	fb_mem = fb_map = NULL;
	fb_pages = 1;
}

static void fb_clear(void)
{
	if (fb_map && !fb_fake) fb_blank(fb_map, fb_map_len);
	else if (fb_mem) fb_blank(fb_mem, fb_len);
}

// The picture shown is interlaced and drawn line for line, and MC_FIELDS=1
// asks to keep its two fields as they are (otherwise they are blended, which
// on a CRT avoids the flicker of fine details). The Main is told with the page ("page <n> 1"): the
// menu core then sends each field to its own field of the 576i/480i CRT
// output, as a DVD player does, and the HDMI scaler blends them.
static int pic_fields = 0;

// The length of a frame of the 15 kHz CRT output (two fields when
// interlaced), from the mode the Main measured (/tmp/mediacenter_crt, read
// again every 2 s); 0 while it is not known.
static double crt_frame_time(void)
{
	static double frame, read_at = -10;
	double t = now_seconds();
	if (t - read_at < 2) return frame;
	read_at = t;
	frame = 0;
	FILE *f = fopen("/tmp/mediacenter_crt", "re");
	if (!f) return 0;
	unsigned w = 0, h = 0;
	int il = 0;
	double hz = 0, vz = 0;
	if (fscanf(f, "%u %u %d %lf %lf", &w, &h, &il, &hz, &vz) == 5 && vz >= 40 && vz <= 70)
		frame = (il ? 2.0 : 1.0) / vz;
	fclose(f);
	return frame;
}

// Draws the picture into the next page and shows it.
// Set while fb_present_paged draws the same picture again (the controls
// changed): such a switch is left out of the time between pictures.
static int fb_redraw;
// When a video picture was last drawn (draw_video_frame).
static double video_drawn_t;

static void fb_present_paged(void)
{
	fb_page_waited = 0;
	if (fb_pages < 2)
	{
		fb_wait_vsync();
		fb_present();
		return;
	}
	// The CRT output keeps showing a page for up to a frame and a half after
	// the next one is switched to (interlaced: whole frames, and half a frame
	// of wait to stay clear of the frame start): a page is drawn again only
	// after that, or the CRT shows half of the new picture (a horizontal tear)
	// when pictures come close together.
	// When the picture's fields are not kept apart (pic_fields 0) the core
	// takes the page at every field, so a page is free a field after the
	// switch: a frame and a half (50 ms at 480i, 60 ms at 576i) leaves room
	// to spare. The fixed 65 ms of before is kept for kept fields and when
	// the CRT mode is not known. At 30 pictures a second with 3 pages a page
	// comes back 3 pictures (100 ms) after it was shown, but the switch to a
	// picture comes after its drawing (about 10 ms): with 65 ms each wait
	// carried over to two pictures later and grew until a picture was
	// dropped (1080p 30 fps lost 12% of its pictures that way).
	static double slot_free[8], last_flip, interval = 0.04;
	int next = (fb_page + 1) % fb_pages;
	double wait = slot_free[next] - now_seconds();
	if (wait > 0 && wait < 0.1)
	{
		double w0 = now_seconds();
		usleep((useconds_t)(wait * 1e6));
		fb_page_waited = now_seconds() - w0;
	}
	fb_page = next;
	fb_mem = fb_map + (size_t)fb_page * fb_len;
	fb_present();
	if (fb_cmd_fd < 0) fb_cmd_fd = open("/dev/MiSTer_cmd", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	char cmd[32];
	int n = snprintf(cmd, sizeof(cmd), "fb_cmd3 page %d %d\n", fb_page, pic_fields);
	double t = now_seconds();
	if (!fb_redraw)
	{
		if (t - last_flip < 0.2) interval = 0.9 * interval + 0.1 * (t - last_flip);
		last_flip = t;
	}
	double keep = 0.03;
	if (interval >= 0.03)
	{
		double frame = crt_frame_time();
		keep = (pic_fields || frame <= 0) ? 0.065 : 1.5 * frame;
	}
	slot_free[(fb_page + fb_pages - 1) % fb_pages] = t + keep;
	if (fb_cmd_fd < 0 || write(fb_cmd_fd, cmd, n) != n)
	{
		// Main does not take it: back to a single page
		fb_page = 0;
		fb_mem = fb_map;
		fb_pages = 1;
		fb_present();
	}
}

// Tells the Main the player is playing (1) or has stopped (0): meanwhile
// the Main rests between its rounds instead of keeping a CPU core busy, so
// the decoder has both cores. A Main that does not know it ignores it.
static void main_idle(int on)
{
	if (fb_cmd_fd < 0) fb_cmd_fd = open("/dev/MiSTer_cmd", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	char cmd[24];
	int n = snprintf(cmd, sizeof(cmd), "fb_cmd3 idle %d\n", on ? 1 : 0);
	if (fb_cmd_fd >= 0 && write(fb_cmd_fd, cmd, n) != n)
		fprintf(stderr, "mediacenter: Main not told it can rest (%d)\n", on);
}

// Programs that take the CPU away from the decoder are paused while a video
// plays (1) and started again after it (0): MiSTer Remote (remote.sh, from
// update_all) catalogues the games for minutes after power-on and took a
// fifth of the CPU, enough to make 1080p stutter. MC_NOPAUSE=1: left alone.
// They are looked for again while a video plays, every second in its first
// 20 s and every 10 s after: MiSTer Remote starts only once the wifi is up,
// half a minute after power-on, and may start just as the video does.
// mediacenter.sh starts them again also if the player does not end normally.
static const char *const bg_names[] = { "remote.sh" };
static pid_t bg_pids[8];
static int bg_count;

static void bg_pause(int on)
{
	if (!on)
	{
		for (int i = 0; i < bg_count; i++)
			if (kill(bg_pids[i], SIGCONT) == 0)
				printf("mediacenter: ripartito dopo il video: pid %d\n", (int)bg_pids[i]);
		bg_count = 0;
		return;
	}
	const char *e = getenv("MC_NOPAUSE");
	if (e && atoi(e)) return;
	DIR *d = opendir("/proc");
	if (!d) return;
	for (struct dirent *de; (de = readdir(d)) && bg_count < (int)(sizeof(bg_pids) / sizeof(bg_pids[0]));)
	{
		int pid = atoi(de->d_name);
		if (pid <= 1 || pid == getpid()) continue;
		int paused = 0;
		for (int i = 0; i < bg_count; i++) paused |= bg_pids[i] == pid;
		if (paused) continue;
		char path[64], comm[32] = "";
		snprintf(path, sizeof(path), "/proc/%d/comm", pid);
		FILE *f = fopen(path, "re");
		if (!f) continue;
		if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0;
		fclose(f);
		for (size_t k = 0; k < sizeof(bg_names) / sizeof(bg_names[0]); k++)
			if (!strcmp(comm, bg_names[k]) && kill(pid, SIGSTOP) == 0)
			{
				bg_pids[bg_count++] = pid;
				printf("mediacenter: in pausa durante il video: %s (pid %d)\n", comm, pid);
			}
	}
	closedir(d);
}

// Diag: every 10 s of a video, the other programs that took more than 5% of
// the CPU (both cores = 100%, as top shows it), to find what slows the
// decoder (MC_DIAG=0: off). Also the CPU all the other programs took
// together, from /proc/stat, which counts the short ones that come and go
// between two looks too. In the first 20 s, right after power-on the
// busiest time, it looks every 2 s and names the 3 busiest programs even
// under 5%, with how many programs were started in between (also from
// /proc/stat: the short ones never show by name).
#define CPU_PIDS 1024
static struct { int pid; unsigned long long ticks; } cpu_prev[CPU_PIDS], cpu_cur[CPU_PIDS];
static int cpu_nprev;
static double cpu_t;
static unsigned long long cpu_all_prev, cpu_busy_prev, cpu_own_prev, cpu_forks_prev;
static double cpu_start;
// early: names of the programs started, "name<parent", seen by spawn_scan
static struct { char name[40]; int count; } spawn[16];
static int spawn_n, spawn_maxpid;
static double spawn_t;

static void cpu_diag(double t, int reset)
{
	if (reset) { cpu_nprev = 0; cpu_t = 0; cpu_start = t; spawn_n = 0; spawn_maxpid = 0; spawn_t = 0; }
	int early = t - cpu_start < 20;
	if (!dd.on || (cpu_t && t - cpu_t < (early ? 2 : 10))) return;
	// early: the 3 busiest, whatever their share
	char top_name[3][20] = { "" };
	double top_pct[3] = { 0 };
	unsigned long long forks = 0;
	DIR *d = opendir("/proc");
	if (!d) return;
	int n = 0;
	static long hz;
	static int ncpu;
	if (!hz) { hz = sysconf(_SC_CLK_TCK); ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN); }
	if (hz <= 0) hz = 100;
	if (ncpu <= 0) ncpu = 1;
	char line[512] = "";
	int len = 0, shown = 0;
	unsigned long long own = 0, all = 0, busy = 0;
	FILE *ps = fopen("/proc/stat", "re");
	if (ps)
	{
		// cpu  user nice system idle iowait irq softirq steal
		unsigned long long v[8] = { 0 };
		if (fscanf(ps, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4)
		{
			for (int i = 0; i < 8; i++) all += v[i];
			busy = all - v[3] - v[4];
		}
		// "processes N": programs started since power-on
		char sl[256];
		while (fgets(sl, sizeof(sl), ps))
			if (sscanf(sl, "processes %llu", &forks) == 1) break;
		fclose(ps);
	}
	for (struct dirent *de; (de = readdir(d)) && n < CPU_PIDS;)
	{
		int pid = atoi(de->d_name);
		if (pid <= 0) continue;
		char path[64], buf[512];
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) continue;
		ssize_t r = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (r <= 0) continue;
		buf[r] = 0;
		// pid (comm) state ppid ... utime is field 14, stime 15
		char *open_p = strchr(buf, '('), *close_p = strrchr(buf, ')');
		if (!open_p || !close_p) continue;
		unsigned long long ut = 0, st = 0;
		if (sscanf(close_p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
		cpu_cur[n].pid = pid;
		cpu_cur[n].ticks = ut + st;
		n++;
		if (pid == getpid()) own = ut + st;
		if (!cpu_t || pid == getpid()) continue;
		long long before = -1;  // started since the last look: next time
		for (int i = 0; i < cpu_nprev; i++)
			if (cpu_prev[i].pid == pid) { before = (long long)cpu_prev[i].ticks; break; }
		if (before < 0) continue;
		double pct = 100.0 * (double)(ut + st - (unsigned long long)before) / ((t - cpu_t) * hz * ncpu);
		if (early)
		{
			for (int k = 0; k < 3; k++)
				if (pct > top_pct[k])
				{
					for (int j = 2; j > k; j--) { top_pct[j] = top_pct[j - 1]; memcpy(top_name[j], top_name[j - 1], sizeof(top_name[j])); }
					top_pct[k] = pct;
					size_t nl = (size_t)(close_p - open_p - 1);
					if (nl >= sizeof(top_name[k])) nl = sizeof(top_name[k]) - 1;
					memcpy(top_name[k], open_p + 1, nl);
					top_name[k][nl] = 0;
					break;
				}
			continue;
		}
		if (pct >= 5 && len < (int)sizeof(line) - 64)
		{
			*close_p = 0;
			len += snprintf(line + len, sizeof(line) - len, "%s%s %.0f%%", shown ? ", " : "", open_p + 1, pct);
			shown++;
		}
	}
	closedir(d);
	if (cpu_t)
	{
		char tot[48] = "";
		if (all > cpu_all_prev && busy >= cpu_busy_prev && cpu_all_prev)
		{
			long long other = (long long)(busy - cpu_busy_prev) - (long long)(own - cpu_own_prev);
			if (other < 0) other = 0;
			snprintf(tot, sizeof(tot), " (altri in tutto %.0f%%)", 100.0 * (double)other / (double)(all - cpu_all_prev));
		}
		if (early)
		{
			for (int k = 0; k < 3 && top_pct[k] >= 0.5; k++)
				len += snprintf(line + len, sizeof(line) - len, "%s%s %.0f%%", k ? ", " : "", top_name[k], top_pct[k]);
			// the names seen, most started first ("name<parent")
			char names[320] = "";
			int nl = 0;
			for (int k = 0; k < 6 && spawn_n; k++)
			{
				int b = 0;
				for (int i = 1; i < spawn_n; i++) if (spawn[i].count > spawn[b].count) b = i;
				if (!spawn[b].count) break;
				nl += snprintf(names + nl, sizeof(names) - nl, "%s%s x%d", k ? ", " : " (", spawn[b].name, spawn[b].count);
				spawn[b].count = 0;
			}
			if (nl) snprintf(names + nl, sizeof(names) - nl, ")");
			spawn_n = 0;
			printf("mediacenter: diag altri programmi %.0f s: %s%s, programmi partiti %llu%s\n", t - cpu_start,
				len ? line : "nessuno", tot, cpu_forks_prev && forks >= cpu_forks_prev ? forks - cpu_forks_prev : 0ULL, names);
		}
		else
			printf("mediacenter: diag altri programmi %.0f s: %s%s\n", mc.clock, shown ? line : "nessuno sopra il 5%", tot);
	}
	cpu_all_prev = all;
	cpu_busy_prev = busy;
	cpu_own_prev = own;
	cpu_forks_prev = forks;
	memcpy(cpu_prev, cpu_cur, n * sizeof(cpu_cur[0]));
	cpu_nprev = n;
	cpu_t = t;
}

// The name of a program and its parent from /proc/<pid>/stat; 0 if gone.
static int proc_name(int pid, char *name, size_t len, int *ppid)
{
	char path[64], buf[256];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return 0;
	ssize_t r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0) return 0;
	buf[r] = 0;
	char *o = strchr(buf, '('), *c = strrchr(buf, ')');
	if (!o || !c) return 0;
	size_t nl = (size_t)(c - o - 1);
	if (nl >= len) nl = len - 1;
	memcpy(name, o + 1, nl);
	name[nl] = 0;
	if (ppid && sscanf(c + 2, "%*c %d", ppid) != 1) *ppid = 0;
	return 1;
}

// Diag, first 20 s of a video: every 0.1 s, the programs started since the
// last look (higher pids), counted by name and parent for the "altri
// programmi" lines; most of them last less than the 2 s between those.
static void spawn_scan(double t)
{
	if (!dd.on || t - cpu_start >= 20 || t - spawn_t < 0.1) return;
	spawn_t = t;
	DIR *d = opendir("/proc");
	if (!d) return;
	int first = !spawn_maxpid, top = spawn_maxpid;
	for (struct dirent *de; (de = readdir(d));)
	{
		int pid = atoi(de->d_name);
		if (pid <= spawn_maxpid) continue;
		if (pid > top) top = pid;
		if (first) continue;    // the first look only notes the highest pid
		char name[20], parent[20] = "?", key[40];
		int ppid = 0;
		if (!proc_name(pid, name, sizeof(name), &ppid)) continue;
		if (ppid > 0) proc_name(ppid, parent, sizeof(parent), NULL);
		snprintf(key, sizeof(key), "%s<%s", name, parent);
		int i = 0;
		while (i < spawn_n && strcmp(spawn[i].name, key)) i++;
		if (i == spawn_n)
		{
			if (spawn_n == (int)(sizeof(spawn) / sizeof(spawn[0]))) continue;
			snprintf(spawn[spawn_n].name, sizeof(spawn[0].name), "%s", key);
			spawn[spawn_n++].count = 0;
		}
		spawn[i].count++;
	}
	closedir(d);
	spawn_maxpid = top;
}

// While a video plays the player comes before the other programs (1), and
// back to normal after it (0): right after power-on the start-up scripts
// took up to 39% of the CPU and the 1080p stuttered for 25 s. All the
// player's threads (decoder ones included) go to nice -10, the loop that
// reads the file and sends the sound and aplay, its sound, to -15 so they
// stay ahead of the decoders. Looked at again with bg_pause, for the threads
// and the aplay started later (a new audio track). MC_NOPRIO=1: left alone.
static void prio_apply(int on)
{
	const char *e = getenv("MC_NOPRIO");
	if (on && e && atoi(e)) return;
	static int said;
	int me = getpid(), main_tid = (int)syscall(SYS_gettid), n = 0;
	DIR *d = opendir("/proc/self/task");
	if (d)
	{
		for (struct dirent *de; (de = readdir(d));)
		{
			int tid = atoi(de->d_name);
			if (tid <= 0) continue;
			int want = !on ? 0 : tid == main_tid ? -15 : -10;
			if (getpriority(PRIO_PROCESS, (id_t)tid) != want && !setpriority(PRIO_PROCESS, (id_t)tid, want)) n++;
		}
		closedir(d);
	}
	// aplay, started by popen: our child, or the child of its shell
	d = opendir("/proc");
	if (d)
	{
		for (struct dirent *de; (de = readdir(d));)
		{
			int pid = atoi(de->d_name), ppid = 0, pp2 = 0;
			char name[20], pname[20];
			if (pid <= 1 || pid == me || !proc_name(pid, name, sizeof(name), &ppid) || strcmp(name, "aplay")) continue;
			if (ppid != me && !(proc_name(ppid, pname, sizeof(pname), &pp2) && pp2 == me)) continue;
			int want = on ? -15 : 0;
			if (getpriority(PRIO_PROCESS, (id_t)pid) != want && !setpriority(PRIO_PROCESS, (id_t)pid, want)) n++;
		}
		closedir(d);
	}
	// with autogroup scheduling the kernel shares the CPU between sessions
	// first, whatever the nice of their programs: our session's share too
	int ag = -1, fd = open("/proc/self/autogroup", O_WRONLY | O_CLOEXEC);
	if (fd >= 0)
	{
		ag = dprintf(fd, "%d", on ? -10 : 0) > 0;
		close(fd);
	}
	if (on && n && !said++)
		printf("mediacenter: precedenza al video sugli altri programmi (%d processi e thread, gruppo %s)\n", n,
			ag < 0 ? "non usato" : ag ? "si" : "no");
	if (!on) said = 0;
}

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

// A YUV 4:2:2 framebuffer needs the MediaCenter menu core (version "MCY")
// and a Main that knows it; cleared the first time it is refused.
static int yuv_ok = 1;

static void fb_native_fmt(int w, int h, double aspect, int yuv);
static double aspect_used(int sw, int sh);
static void crt_fb_changed(void);
static void crt_video_std(const AVFrame *f);

// At the start of a video: the special frame buffers (and YUV) are asked
// again after a Main that answered late, unless it missed 3 times in a row.
static void native_retry(void)
{
	if (!native_ok && native_fails && native_fails < 3)
	{
		fprintf(stderr, "mediacenter: special frame buffers asked again\n");
		native_ok = 1;
	}
	if (!yuv_ok && yuv_fails && yuv_fails < 3)
	{
		fprintf(stderr, "mediacenter: YUV frame buffer asked again\n");
		yuv_ok = 1;
	}
}

static void fb_native(int w, int h, double aspect)
{
	fb_native_fmt(w, h, aspect, 0);
}

static void fb_native_fmt(int w, int h, double aspect, int yuv)
{
	if (!native_ok || !fb_mem) return;
	if (!w) yuv = 0;
	if (w == native_w && h == native_h && (!w || aspect == native_aspect) && yuv == fb_yuv) return;
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
		fb_yuv = yuv;
		fb_stride = fb_w * (yuv ? 2 : 4);
		fb_len = (size_t)fb_stride * fb_h;
		fb_mem = (uint8_t *)calloc(1, fb_len);
	}
	else
	{
		int fd = open("/dev/MiSTer_cmd", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
		{
			native_ok = 0;
			native_fails = 3; // no Main to ask
			return;
		}
		char cmd[96];
		if (w) snprintf(cmd, sizeof(cmd), "fb_cmd3 %s %d %d %d 1000\n", yuv ? "422 0" : "8888 1", w, h, (int)(aspect * 1000 + 0.5));
		else snprintf(cmd, sizeof(cmd), "fb_cmd3 default\n");
		int ok = write(fd, cmd, strlen(cmd)) > 0;
		close(fd);

		// Main applies the mode in its own loop: wait for Linux to report it
		// (longer just after a CRT mode change, which keeps the Main busy).
		int changed = 0, tries = now_seconds() - crt_mode_sent_t < 5.0 ? 400 : 100;
		for (int i = 0; ok && i < tries && !changed; i++)
		{
			usleep(10000);
			int f = open("/dev/fb0", O_RDWR | O_CLOEXEC);
			struct fb_var_screeninfo var;
			if (f >= 0 && !ioctl(f, FBIOGET_VSCREENINFO, &var))
				changed = (int)var.xres == want_w && var.bits_per_pixel == (yuv ? 16u : 32u) &&
					((int)var.yres == want_h || (w && (int)var.yres == want_h * FB_PAGES));
			if (f >= 0) close(f);
		}
		if (!changed && yuv)
		{
			fprintf(stderr, "mediacenter: YUV framebuffer not available (needs the MediaCenter menu core and Main), colours converted on the ARM\n");
			yuv_ok = 0;
			yuv_fails++;
			return;
		}
		if (!changed)
		{
			fprintf(stderr, "mediacenter: framebuffer %dx%d not available, scaling on the ARM (until the next video)\n", want_w, want_h);
			native_ok = 0;
			native_fails++;
			return;
		}
		fb_close();
		fb_yuv = yuv;
		if (fb_open_pages(w ? want_h : 0))
		{
			fprintf(stderr, "mediacenter: framebuffer lost\n");
			quit_requested = 1;
			return;
		}
	}
	native_fails = 0;
	if (fb_yuv) yuv_fails = 0;
	fprintf(stderr, "mediacenter: framebuffer %dx%d%s%s%s\n", fb_w, fb_h, w ? " stretched by the scaler" : "",
		fb_pages > 1 ? ", 3 pages" : "", fb_yuv ? ", YUV converted by the FPGA" : "");
	native_w = w;
	native_h = h;
	native_aspect = aspect;
	fb_menu = menu_asking;
	fb_par = w ? aspect * h / w : 1.0;
	screen_rebuild();
	crt_fb_changed();
}

// Time spent drawing pictures, reported in the log.
static double draw_convert, draw_present;
static int draw_count;

#if defined(__arm__)
#pragma GCC push_options
#pragma GCC target("fpu=neon")
#include <arm_neon.h>
#endif

// One row of 4:2:0 planes as 4:2:2 bytes Y0 U Y1 V.
static void pack_yuyv(uint8_t *dst, const uint8_t *y, const uint8_t *u, const uint8_t *v, int w)
{
	int x = 0;
#if defined(__arm__)
	for (; x + 32 <= w; x += 32)
	{
		uint8x16x2_t yy = vld2q_u8(y + x);
		uint8x16x4_t o;
		o.val[0] = yy.val[0];
		o.val[1] = vld1q_u8(u + x / 2);
		o.val[2] = yy.val[1];
		o.val[3] = vld1q_u8(v + x / 2);
		vst4q_u8(dst + 2 * x, o);
	}
#endif
	for (; x + 1 < w; x += 2)
	{
		dst[2 * x] = y[x];
		dst[2 * x + 1] = u[x / 2];
		dst[2 * x + 2] = y[x + 1];
		dst[2 * x + 3] = v[x / 2];
	}
}

// pack_yuyv for a picture wider than the framebuffer: output pixel x takes
// source pixel map[x] (the same nearest-pixel choice as the RGB path), and
// each output pair takes the colour of its first pixel's source pair.
static void pack_yuyv_map(uint8_t *dst, const uint8_t *y, const uint8_t *u, const uint8_t *v, const int *map, int w)
{
	for (int x = 0; x + 1 < w; x += 2)
	{
		int s = map[x];
		dst[2 * x] = y[s];
		dst[2 * x + 1] = u[s / 2];
		dst[2 * x + 2] = y[map[x + 1]];
		dst[2 * x + 3] = v[s / 2];
	}
}

// pack_yuyv narrowing by exactly 3:2 (a 1920-wide picture on a 1280-wide
// screen): every 3 source pixels give 2, each weighing the nearer pixel 2/3
// and its neighbour 1/3; the colour likewise (3 samples give 2). w is the
// output width.
static inline uint8_t third2(int a, int b) { return (uint8_t)((a * 171 + b * 85) >> 8); }

#if defined(__arm__)
// third2 on 8 lanes: a * 171/256 + b * 85/256
static inline uint8x8_t third2_8(uint8x8_t a, uint8x8_t b)
{
	return vshrn_n_u16(vmlal_u8(vmull_u8(a, vdup_n_u8(171)), b, vdup_n_u8(85)), 8);
}
#endif

static void pack_yuyv_32(uint8_t *dst, const uint8_t *y, const uint8_t *u, const uint8_t *v, int w)
{
	int x = 0; // output pixel
#if defined(__arm__)
	for (; x + 32 <= w; x += 32)
	{
		// 48 source pixels -> 32 output pixels = 16 pairs; 24 colour samples -> 16
		uint8x16x3_t s = vld3q_u8(y + x / 2 * 3);
		uint8x8x3_t cu = vld3_u8(u + x / 4 * 3), cv = vld3_u8(v + x / 4 * 3);
		uint8x16x4_t o;
		o.val[0] = vcombine_u8(third2_8(vget_low_u8(s.val[0]), vget_low_u8(s.val[1])),
			third2_8(vget_high_u8(s.val[0]), vget_high_u8(s.val[1])));
		o.val[2] = vcombine_u8(third2_8(vget_low_u8(s.val[2]), vget_low_u8(s.val[1])),
			third2_8(vget_high_u8(s.val[2]), vget_high_u8(s.val[1])));
		uint8x8x2_t zu = vzip_u8(third2_8(cu.val[0], cu.val[1]), third2_8(cu.val[2], cu.val[1]));
		uint8x8x2_t zv = vzip_u8(third2_8(cv.val[0], cv.val[1]), third2_8(cv.val[2], cv.val[1]));
		o.val[1] = vcombine_u8(zu.val[0], zu.val[1]);
		o.val[3] = vcombine_u8(zv.val[0], zv.val[1]);
		vst4q_u8(dst + 2 * x, o);
	}
#endif
	for (; x + 1 < w; x += 2)
	{
		int j = x / 2, sy = 3 * j, sc = 3 * (j / 2);
		dst[2 * x] = third2(y[sy], y[sy + 1]);
		dst[2 * x + 2] = third2(y[sy + 2], y[sy + 1]);
		if (j & 1)
		{
			dst[2 * x + 1] = third2(u[sc + 2], u[sc + 1]);
			dst[2 * x + 3] = third2(v[sc + 2], v[sc + 1]);
		}
		else
		{
			dst[2 * x + 1] = third2(u[sc], u[sc + 1]);
			dst[2 * x + 3] = third2(v[sc], v[sc + 1]);
		}
	}
}

// Two packed lines averaged (rounded down, as deinterlace_rgb does).
static void yuv_mean(uint8_t *a, const uint8_t *b, int n)
{
	int i = 0;
#if defined(__arm__)
	for (; i + 16 <= n; i += 16) vst1q_u8(a + i, vhaddq_u8(vld1q_u8(a + i), vld1q_u8(b + i)));
#endif
	for (; i < n; i++) a[i] = (uint8_t)((a[i] + b[i]) >> 1);
}

#if defined(__arm__)
#pragma GCC pop_options
#endif

// The controls blended in grey into framebuffer row y (YUYV): Y toward their
// level, U and V toward 128.
static void yuv_band(uint8_t *row_buf, int y)
{
	const uint8_t *a = ovl_a + (size_t)(y - ovl_y) * fb_w;
	const uint8_t *v = ovl_v + (size_t)(y - ovl_y) * fb_w;
	for (int x = 0; x + 1 < fb_w; x += 2)
	{
		if (!a[x] && !a[x + 1]) continue;
		uint8_t *p = row_buf + 2 * x;
		for (int k = 0; k < 2; k++)
		{
			unsigned al = a[x + k], vy = 16 + v[x + k] * 219 / 255;
			p[2 * k] = (uint8_t)((p[2 * k] * (255 - al) + vy * al + 127) / 255);
		}
		unsigned ac = (a[x] + a[x + 1] + 1) / 2;
		p[1] = (uint8_t)((p[1] * (255 - ac) + 128 * ac + 127) / 255);
		p[3] = (uint8_t)((p[3] * (255 - ac) + 128 * ac + 127) / 255);
	}
}

// The controls over a picture of the FPGA accelerator: the picture is only
// in the frame buffer (no cache, slow to read), so each row of the band is
// read back in 64-byte blocks into row_buf, blended there 16 pixels at a
// time with NEON (same bytes as yuv_band) and written back.
// Time spent, for "diag comandi sopra l'acceleratore".
// With a core that blends them (accel_link.c, acc_ovl_prepare) only the
// preparation of what changed is counted.
static struct { int pics, w_pics, core, w_core; double read, blend, w_read, w_blend, prep, w_prep; } od;

#if defined(__arm__)
#pragma GCC push_options
#pragma GCC target("fpu=neon")
// x / 255 rounded down, exact for x <= 65407 (all values used here)
static inline uint16x8_t div255_16(uint16x8_t x)
{
	return vshrq_n_u16(vaddq_u16(vaddq_u16(x, vdupq_n_u16(1)), vshrq_n_u16(x, 8)), 8);
}
// (p * (255 - a) + q * a + 127) / 255 on 8 lanes
static inline uint8x8_t mix255(uint8x8_t p, uint8x8_t q, uint8x8_t a)
{
	uint16x8_t n = vmull_u8(p, vsub_u8(vdup_n_u8(255), a));
	n = vmlal_u8(n, q, a);
	n = vaddq_u16(n, vdupq_n_u16(127));
	return vmovn_u16(div255_16(n));
}
// 16 + v * 219 / 255 on 8 lanes
static inline uint8x8_t grey_y(uint8x8_t v)
{
	return vadd_u8(vmovn_u16(div255_16(vmull_u8(v, vdup_n_u8(219)))), vdup_n_u8(16));
}
#endif

static void fb_read_row(uint8_t *dst, const uint8_t *src, int n)
{
	int i = 0;
#if defined(__arm__)
	const uint8_t *s = src;
	uint8_t *d = dst;
	for (; i + 64 <= n; i += 64)
		__asm__ volatile(
			"vld1.8 {d0-d3}, [%0]!\n\t"
			"vld1.8 {d4-d7}, [%0]!\n\t"
			"vst1.8 {d0-d3}, [%1]!\n\t"
			"vst1.8 {d4-d7}, [%1]!\n\t"
			: "+r"(s), "+r"(d) : : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "memory");
#endif
	if (i < n) memcpy(dst + i, src + i, (size_t)(n - i));
}

static void yuv_band_fast(uint8_t *row_buf, int y)
{
	int x = 0;
#if defined(__arm__)
	const uint8_t *a = ovl_a + (size_t)(y - ovl_y) * fb_w;
	const uint8_t *v = ovl_v + (size_t)(y - ovl_y) * fb_w;
	for (; x + 16 <= fb_w; x += 16)
	{
		// 8 pairs Y0 U Y1 V; coverage and grey of the even and odd pixels
		uint8x8x4_t p = vld4_u8(row_buf + 2 * x);
		uint8x8x2_t aa = vld2_u8(a + x), vv = vld2_u8(v + x);
		p.val[0] = mix255(p.val[0], grey_y(vv.val[0]), aa.val[0]);
		p.val[2] = mix255(p.val[2], grey_y(vv.val[1]), aa.val[1]);
		uint8x8_t ac = vrhadd_u8(aa.val[0], aa.val[1]); // (a0 + a1 + 1) / 2
		p.val[1] = mix255(p.val[1], vdup_n_u8(128), ac);
		p.val[3] = mix255(p.val[3], vdup_n_u8(128), ac);
		vst4_u8(row_buf + 2 * x, p);
	}
	if (x + 1 < fb_w)
	{
		// the last pixels: yuv_band's own loop from x
		for (; x + 1 < fb_w; x += 2)
		{
			if (!a[x] && !a[x + 1]) continue;
			uint8_t *q = row_buf + 2 * x;
			for (int k = 0; k < 2; k++)
			{
				unsigned al = a[x + k], vy = 16 + v[x + k] * 219 / 255;
				q[2 * k] = (uint8_t)((q[2 * k] * (255 - al) + vy * al + 127) / 255);
			}
			unsigned acs = (a[x] + a[x + 1] + 1) / 2;
			q[1] = (uint8_t)((q[1] * (255 - acs) + 128 * acs + 127) / 255);
			q[3] = (uint8_t)((q[3] * (255 - acs) + 128 * acs + 127) / 255);
		}
	}
#else
	(void)x;
	yuv_band(row_buf, y);
#endif
}
#if defined(__arm__)
#pragma GCC pop_options
#endif

// Picture line l of a DVD picture packed as YUYV. An interlaced picture's
// 4:2:0 colour rows alternate between its two fields: line l takes the row
// of its own field (2 * (l / 4) + l % 2), or the colour of fast motion
// smears across the fields.
static void yuv_dvd_line(uint8_t *dst, const AVFrame *f, int l, int w)
{
	int ch = (f->height + 1) / 2;
	int c = yuv_ilace ? 2 * (l >> 2) + (l & 1) : l / 2;
	if (c >= ch) c = ch - 1;
	pack_yuyv(dst, f->data[0] + (size_t)l * f->linesize[0], f->data[1] + (size_t)c * f->linesize[1],
		f->data[2] + (size_t)c * f->linesize[2], w);
}

// fb_present for a DVD picture in a YUV framebuffer (as big as the picture).
// Without MC_FIELDS each line is blended with the next, like deinterlace_rgb;
// the subtitles, made soft (each line the mean of two), then take their line
// below, so they are blended the same way.
static void yuv_dvd_draw(void)
{
	static uint8_t *rows;
	static int rows_size;
	if (rows_size < 2 * fb_stride)
	{
		free(rows);
		rows = (uint8_t *)malloc((size_t)2 * fb_stride);
		rows_size = rows ? 2 * fb_stride : 0;
		if (!rows) return;
	}
	uint8_t *row_buf = rows, *next = rows + fb_stride;
	AVFrame *f = yuv_last;
	int ok = have_frame && f && f->data[0];
	int w = ok ? (fb_w < f->width ? fb_w : f->width) & ~1 : 0;
	int ovl = ok && yuv_ovl_row && yuv_ovl_begin && yuv_ovl_end;
	if (ovl) yuv_ovl_begin();
	for (int y = 0; y < fb_h; y++)
	{
		uint8_t *out = fb_mem + (size_t)y * fb_stride;
		int band = y >= ovl_y && ovl_shown && ovl_a;
		if (!ok || y >= f->height)
		{
			fb_blank(row_buf, (size_t)fb_w * 2);
		}
		else
		{
			fb_blank(row_buf + w * 2, (size_t)(fb_w - w) * 2);
			yuv_dvd_line(row_buf, f, y, w);
			if (yuv_deint && y + 1 < f->height)
			{
				yuv_dvd_line(next, f, y + 1, w);
				yuv_mean(row_buf, next, w * 2);
			}
			if (ovl) yuv_ovl_row(row_buf, y, w, yuv_deint ? 1 : 0);
		}
		if (band) yuv_band(row_buf, y);
		memcpy(out, row_buf, (size_t)fb_w * 2);
	}
	if (ovl) yuv_ovl_end();
}

#include "zerocopy.c"
#include "accel_link.c"

// fb_present for a YUV framebuffer: the picture is the size of the
// framebuffer; the controls are blended in grey (Y toward their level, U and
// V toward 128).
static void yuv_draw(void)
{
	if (yuv_dvd)
	{
		yuv_dvd_draw();
		return;
	}
	static uint8_t *row_buf;
	static int row_size;
	if (row_size < fb_stride)
	{
		free(row_buf);
		row_buf = (uint8_t *)malloc(fb_stride);
		row_size = row_buf ? fb_stride : 0;
		if (!row_buf) return;
	}
	AVFrame *f = yuv_last;
	int w = fb_w < (f ? f->width : 0) ? fb_w : (f ? f->width : 0);
	w &= ~1;
	// A picture wider than the framebuffer (only when it is wider than the
	// screen) is narrowed here, while it is copied.
	static int *map, map_src, map_w;
	int narrow = f && f->width > fb_w;
	// exactly 3:2 (1920 on a 1280 screen): narrowed with NEON, averaging
	int narrow32 = narrow && f->width * 2 == w * 3 && !(w & 3);
	if (narrow && (map_src != f->width || map_w != w))
	{
		free(map);
		map = (int *)malloc((size_t)w * sizeof(int));
		map_src = map ? f->width : 0;
		map_w = map ? w : 0;
		if (!map) return;
		for (int x = 0; x < w; x++) map[x] = x * f->width / w;
	}
	// 1080p: the core draws the lines above the controls (zerocopy.c)
	int core_rows = 0;
	// a picture of the FPGA accelerator is only in the FPGA's memory: the
	// core draws all of it and the controls are blended over it here
	int accf = have_frame && f && acc.on && acc_buf_of(f);
	// the core blends the controls over it (accel_link.c)
	acc_ovl_on = accf && ovl_shown && ovl_a && ovl_y < fb_h && acc_ovl_prepare();
	// exactly 2:1 (1920 on 960, the 4:3 format): the core narrows the
	// accelerator's pictures too, when it can (acc_can21)
	int narrow21 = narrow && f->width == w * 2 && !(w & 31) && acc_can21();
	if (have_frame && f) acc_layout_note((narrow32 || narrow21) && yuv_half && w == fb_w && !(fb_w & 31));
	if ((narrow32 || (accf && narrow21)) && yuv_half && have_frame && f && f->data[0] && w == fb_w)
	{
		int rows = (ovl_shown && ovl_a && ovl_y < fb_h && !accf) ? (ovl_y > 0 ? ovl_y : 0) : fb_h;
		if (zcp_draw(f, rows)) core_rows = rows;
	}
	else if (accf) acc_fail_layout("the picture format is not drawn by the core");
	int core_ovl = acc_ovl_on && core_rows;
	acc_ovl_on = 0;
	if (core_ovl) { od.core++; od.w_core++; }
	if (accf)
	{
		if (!core_rows) fb_blank(fb_mem, (size_t)fb_stride * fb_h);
		else if (ovl_shown && ovl_a && !core_ovl)
		{
			double tr = 0, tb = 0;
			for (int y = ovl_y > 0 ? ovl_y : 0; y < fb_h; y++)
			{
				uint8_t *out = fb_mem + (size_t)y * fb_stride;
				double t0 = now_seconds();
				fb_read_row(row_buf, out, fb_w * 2);
				double t1 = now_seconds();
				yuv_band_fast(row_buf, y);
				memcpy(out, row_buf, (size_t)fb_w * 2);
				tb += now_seconds() - t1;
				tr += t1 - t0;
			}
			od.pics++; od.read += tr; od.blend += tb;
			od.w_pics++; od.w_read += tr; od.w_blend += tb;
		}
		return;
	}
	for (int y = core_rows; y < fb_h; y++)
	{
		uint8_t *out = fb_mem + (size_t)y * fb_stride;
		int band = y >= ovl_y && ovl_shown && ovl_a;
		int ly = yuv_half ? 2 * y : y, cy = yuv_half ? y : y / 2;
		// rows are packed straight into the framebuffer; only rows under the
		// controls go through row_buf, as the blend reads them back
		uint8_t *dst = band ? row_buf : out;
		if (!have_frame || !f || !f->data[0] || ly >= f->height)
		{
			fb_blank(dst, (size_t)fb_w * 2);
		}
		else
		{
			fb_blank(dst + w * 2, (size_t)(fb_w - w) * 2);
			if (narrow32)
				pack_yuyv_32(dst, f->data[0] + (size_t)ly * f->linesize[0], f->data[1] + (size_t)cy * f->linesize[1],
					f->data[2] + (size_t)cy * f->linesize[2], w);
			else if (narrow)
				pack_yuyv_map(dst, f->data[0] + (size_t)ly * f->linesize[0], f->data[1] + (size_t)cy * f->linesize[1],
					f->data[2] + (size_t)cy * f->linesize[2], map, w);
			else
				pack_yuyv(dst, f->data[0] + (size_t)ly * f->linesize[0], f->data[1] + (size_t)cy * f->linesize[1],
					f->data[2] + (size_t)cy * f->linesize[2], w);
		}
		if (band)
		{
			yuv_band(row_buf, y);
			memcpy(out, row_buf, (size_t)fb_w * 2);
		}
	}
}

// Converts and draws a decoded picture.
static void draw_video_frame(AVFrame *f)
{
	// the video's own aspect, or the format fixed in Regola CRT
	fb_aspect = aspect_used(native_w ? def_w : fb_w, native_w ? def_h : fb_h);
	// Light mode: every other line, as a picture of half the height.
	int h = f->height;
	int linesize[AV_NUM_DATA_POINTERS];
	memcpy(linesize, f->linesize, sizeof(linesize));
	int half = half_lines && native_fb && !(f->flags & AV_FRAME_FLAG_INTERLACED);
	if (half)
	{
		h /= 2;
		for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) linesize[i] *= 2;
	}
	if (native_fb)
	{
		// Small pictures (DivX, Video CD) get whole multiples of their lines
		// so the controls still fit and the scaler has less to enlarge.
		// Pictures narrower than 640 (Super Video CD 480, some DivX) are
		// doubled across too, so the controls fit across the screen.
		int mx = f->width < 640 ? 2 : 1, my = 1;
		while (!half && h * (my + 1) <= 1080 && h * my < 400) my++;
		// The scaler only enlarges: a picture wider (or taller) than the
		// screen is narrowed on the ARM, or its right side would be cut off.
		int fw = f->width * mx, fh = h * my;
		int sw = native_w ? def_w : fb_w, sh = native_w ? def_h : fb_h;
		// The scaler shows the frame buffer in a window of the picture's
		// aspect (4:3 on a 16:9 screen is narrower than the screen) and
		// cannot shrink it: the picture must fit that window.
		if (sw >= 320 && sh >= 240 && fb_aspect > 0)
		{
			int ww = (int)(sh * fb_aspect + 0.5);
			if (ww <= sw) sw = ww;
			else sh = (int)(sw / fb_aspect + 0.5);
		}
		if (sw >= 320 && fw > sw) fw = sw;
		if (sh >= 240 && fh > sh) fh = sh;
		pic_fields = (f->flags & AV_FRAME_FLAG_INTERLACED) && fh == h && getenv("MC_FIELDS") && atoi(getenv("MC_FIELDS"));
		// A picture drawn at its own size goes to the framebuffer as YUV and
		// the FPGA converts the colours: the ARM only copies it. Decided on
		// the first picture of a file (interlaced video stays in RGB, for
		// the deinterlacing).
		// A picture wider than the screen is narrowed while it is copied, still
		// in YUV (before, it went to RGB on the ARM: about 90 ms a picture).
		int narrow = mx == 1 && fw < f->width;
		if (yuv_stream < 0)
			yuv_stream = yuv_ok && !getenv("MC_NOYUV") && !frame_overlay && f->format == AV_PIX_FMT_YUV420P &&
				!(f->flags & AV_FRAME_FLAG_INTERLACED) && mx == 1 && (fw == f->width || narrow) && fh == h && !(fw & 1);
		int yuv = yuv_stream && yuv_ok && (fw == f->width || narrow) && fh == h && f->format == AV_PIX_FMT_YUV420P;
		// DVD pictures too (step 3), when they are drawn at their own size: the
		// subtitles, buttons and line blending are done while copying.
		int dvd_yuv = frame_overlay && yuv_ovl_row && yuv_ok && !getenv("MC_NOYUV") && f->format == AV_PIX_FMT_YUV420P &&
			mx == 1 && fw == f->width && fh == h && !(fw & 1);
		if (dvd_yuv) yuv = 1;
		crt_video_std(f);
		fb_native_fmt(fw, fh, fb_aspect, yuv);
		if (yuv && !fb_yuv) fb_native_fmt(fw, fh, fb_aspect, 0); // refused
		if (fb_yuv && fb_mem)
		{
			double t_start = now_seconds();
			fb_layout(f->width, h);
			if (!yuv_last) yuv_last = av_frame_alloc();
			if (!yuv_last) return;
			av_frame_unref(yuv_last);
			if (av_frame_ref(yuv_last, f) < 0) return;
			yuv_half = half;
			yuv_dvd = dvd_yuv;
			yuv_ilace = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
			yuv_deint = dvd_yuv && yuv_ilace && !pic_fields;
			if (dvd_yuv) overlay_soften = 1;
			have_frame = 1;
			fb_present_paged();
			video_drawn_t = now_seconds();
			draw_present += now_seconds() - t_start;
			draw_count++;
			loop_diag_drawn(now_seconds() - t_start);
			return;
		}
	}
	else pic_fields = 0;
	acc_layout_note(0);
	if (acc.on && acc_buf_of(f))
	{
		// only the core can draw the accelerator's pictures
		acc_fail_layout("the picture format is not drawn by the core");
		return;
	}
	if (!fb_mem) return;
	// The rows outside the picture are not drawn again: on a new layout all
	// the pages are made black (fb_layout clears only the one shown), or the
	// bars keep what the other pages showed before (a list, say).
	int relayout = f->width != src_w || h != src_h || fb_aspect / fb_par != fb_layout_aspect;
	fb_layout(f->width, h);
	if (relayout) fb_clear();
	if (!frame_rgb || !line_buf || !x_map) return;
	mc.sws = sws_getCachedContext(mc.sws, f->width, h, (enum AVPixelFormat)f->format,
		f->width, h, fb_bgr ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA, SWS_POINT, NULL, NULL, NULL);
	if (!mc.sws) return;
	double t_start = now_seconds();
	int stride = src_w * 4;
	if (!(stride & 15))
	{
		uint8_t *dst[4] = { frame_rgb, NULL, NULL, NULL };
		int dst_stride[4] = { stride, 0, 0, 0 };
		sws_scale(mc.sws, (const uint8_t *const *)f->data, linesize, 0, h, dst, dst_stride);
	}
	else
	{
		// The NEON conversion needs 16-byte aligned rows: convert into an
		// aligned buffer and copy the rows.
		static uint8_t *tmp = NULL;
		static int tmp_size = 0;
		int tstride = (stride + 63) & ~63;
		if (tmp_size < tstride * h)
		{
			av_free(tmp);
			tmp = (uint8_t *)av_malloc((size_t)tstride * h);
			tmp_size = tmp ? tstride * h : 0;
		}
		if (!tmp) return;
		uint8_t *dst[4] = { tmp, NULL, NULL, NULL };
		int dst_stride[4] = { tstride, 0, 0, 0 };
		sws_scale(mc.sws, (const uint8_t *const *)f->data, linesize, 0, h, dst, dst_stride);
		for (int y = 0; y < h; y++) memcpy(frame_rgb + (size_t)y * stride, tmp + (size_t)y * tstride, stride);
	}
	// Subtitles and menu buttons are drawn before the deinterlacing, which
	// blends them like the picture; otherwise the overlay softens them.
	int deint = (f->flags & AV_FRAME_FLAG_INTERLACED) && !pic_fields;
	overlay_soften = !deint;
	if (frame_overlay) frame_overlay();
	if (deint) deinterlace_rgb();
	have_frame = 1;
	double t_conv = now_seconds();
	fb_present_paged();
	draw_convert += t_conv - t_start;
	draw_present += now_seconds() - t_conv;
	draw_count++;
}

// Takes the frames that are due; returns the most recent one (owned by the
// caller) or NULL.

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
		loop_diag_shown(mc.vq_count);
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
	if (vt.on) vt_stop_and_lock();
	if (mc.vdec) avcodec_flush_buffers(mc.vdec);
	if (mc.adec) avcodec_flush_buffers(mc.adec);
	vq_clear();
	acc_seek();
	mc.pend_len = 0;
	mc.audio_end = t;
	mc.last_vpts = mc.read_vpts = NAN;
	mc.eof = mc.flushed = 0;
	mc.clock = vt.clock = t;
	if (vt.on) pthread_mutex_unlock(&vt.lock);
	audio_flush();
}

// The FPGA accelerator gave up on this file (accel.c, acc_fail): the normal
// decoder goes on from the same moment.
static void acc_fallback(void)
{
	if (!acc.on || !acc.failed) return;
	int layout = acc_by_layout;
	acc_by_layout = 0;
	if (layout)
		printf("mediacenter: diag acceleratore: spento a %.1f s (%s), decoder normale finche' il formato non torna disegnabile\n", mc.clock, acc.why);
	else
		printf("mediacenter: diag acceleratore: spento per questo video a %.1f s (%s), riprende il decoder normale\n", mc.clock, acc.why);
	double t = mc.clock;
	int was_on = vt.on;
	vt_end();
	vq_clear();
	if (yuv_last) av_frame_unref(yuv_last);
	if (mc.vdec) avcodec_free_context(&mc.vdec);
	acc_close();
	zcp_close();
	acc_file_off = layout ? 2 : 1;
	acc_layout_ok = 0;
	mc.vdec = open_decoder(mc.vstream);
	if (!mc.vdec) mc.vstream = -1;
	else if (was_on) vt_start();
	media_seek(t);
}

// The accelerator stopped only for the picture format: once the format can
// be drawn by the core again (for a second, so a quick run through the
// formats with Start does not reopen it each time) it comes back.
static void acc_resume(void)
{
	if (acc_file_off != 2 || !acc_layout_ok || now_seconds() - acc_layout_t < 1.0 || mc.vstream < 0 || !mc.vdec) return;
	double t = mc.clock;
	printf("mediacenter: diag acceleratore: il formato si puo' di nuovo disegnare dal core, riacceso a %.1f s\n", t);
	int was_on = vt.on;
	vt_end();
	vq_clear();
	if (yuv_last) av_frame_unref(yuv_last);
	avcodec_free_context(&mc.vdec);
	zcp_close();
	acc_file_off = 0;
	acc_layout_ok = 0;
	mc.vdec = open_decoder(mc.vstream);
	if (!mc.vdec) mc.vstream = -1;
	else if (was_on) vt_start();
	media_seek(t);
}

// Shows the first picture at or after t (fast forward / rewind). Returns its
// time, NAN when there is none.
static double media_preview(double t)
{
	double shown = NAN;
	media_seek(t);
	if (mc.vstream < 0) return shown;
	// the video thread is idle after the seek: decode here
	vt.direct = 1;
	for (int i = 0; i < 400 && !mc.vq_count; i++)
	{
		if (!decode_step()) break;
	}
	vt.direct = 0;
	if (mc.vq_count)
	{
		AVFrame *f = mc.vq[0].frame;
		shown = mc.vq[0].pts;
		draw_video_frame(f);
		vq_clear();
	}
	mc.pend_len = 0;
	return shown;
}

// Plays on from t after fast forward / rewind. The seek lands on the key
// picture before t: play from there, or every picture up to the next key
// one (seconds away in H.264) would count as late and be skipped while the
// sound plays on.
static void media_resume(double t)
{
	double shown = media_preview(t);
	if (!isnan(shown) && shown < t && shown > t - 30)
	{
		mc.clock = vt.clock = shown;
		mc.audio_end = shown;
	}
	preroll_start();
}

static int num_chapters(void)
{
	return svcd_on ? svcd_chapters : (int)mc.fmt->nb_chapters;
}

static int chapter_index(double t)
{
	int c = -1;
	if (svcd_on)
	{
		for (int i = 0; i < svcd_chapters; i++) if (svcd_chapter[i] <= t + 0.5) c = i;
		return c;
	}
	for (unsigned i = 0; i < mc.fmt->nb_chapters; i++)
	{
		AVChapter *ch = mc.fmt->chapters[i];
		if (ts_seconds(ch->start, ch->time_base) - mc.start <= t + 0.5) c = i;
	}
	return c;
}

static double chapter_start(int c)
{
	if (svcd_on) return svcd_chapter[c];
	AVChapter *ch = mc.fmt->chapters[c];
	return ts_seconds(ch->start, ch->time_base) - mc.start;
}

static void draw_audio_screen(void)
{
	fb_clear();
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

enum { END_DONE, END_NEXT, END_PREV, END_BACK, END_QUIT, END_GOTO };

// The format box during the film (format_key, after Regola CRT).
static int format_key(int code, double t, int state, int menu);
static int format_tick(double t);
static const char *format_label(void);
static void fmt_close(void);

// Hooks for the Super Video CD menus (svcd_pbc): keys, the end of the
// stream and the on-screen controls.
static int (*play_key_hook)(int code);          // 1: key used; 2: used, playback ends with END_GOTO
static int (*play_end_hook)(void);              // 0: end; 1: play again; 2: keep the last picture
static void (*play_osd_hook)(osd_info_t *info); // menu choice on the controls
static int play_osd_keep = 0;                   // controls always on screen (a menu)

// Plays the open file. Returns how playback ended.
static int media_play(int in_playlist)
{
	player_t p;
	memset(&p, 0, sizeof(p));
	p.state = ST_PLAY;
	p.osd_until = now_seconds() + 3.0;
	have_frame = 0;
	static char svcd_question[48];
	snprintf(svcd_question, sizeof(svcd_question), "Uscire dal %s?", svcd_kind);
	exit_question = svcd_on ? svcd_question : "Tornare all'elenco?";

	if (mc.astream >= 0 && audio_open(MC_AUDIO_RATE)) fprintf(stderr, "mediacenter: playing without audio\n");
	fb_clear();
	if (mc.audio_only) draw_audio_screen();
	dec_diag_reset();
	loop_diag_reset();
	main_idle(1);
	bg_pause(1);
	cpu_diag(now_seconds(), 1);
	vt_start();
	preroll_start();
	frames_shown = frames_dropped = 0;
	draw_convert = draw_present = 0;
	draw_count = 0;
	av_waited = 0;
	av_pause = 0;
	audio_late_drop = audio_full_drop = 0;
	av_gaps = av_giveups = 0;
	double played = 0;
	// The loop that reads the file and sends the sound runs ahead of the
	// video decoding threads, so the sound keeps flowing on a heavy stream.
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -5);
	prio_apply(1);

	int result = END_DONE;
	double last = now_seconds(), osd_built = 0, bg_look = last, bg_t0 = last;
	while (!quit_requested)
	{
		double t = now_seconds();
		double elapsed = t - last;
		last = t;
		if (p.state == ST_PLAY) loop_diag_round(t, elapsed);
		cpu_diag(t, 0);
		spawn_scan(t);
		// every second in the first 20 s of the video, then every 10 s
		if (t - bg_look >= (t - bg_t0 < 20 ? 1 : 10)) { bg_look = t; bg_pause(1); prio_apply(1); }
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
			if (format_key(code, t, p.state, mc.audio_only || play_osd_keep)) continue;
			if (play_key_hook)
			{
				int h = play_key_hook(code);
				if (h == 2)
				{
					result = END_GOTO;
					break;
				}
				if (h) continue;
			}
			switch (code)
			{
			case KEY_PLAY:
			case KEY_PLAYCD:
				if (p.state == ST_STOP) media_seek(0);
				else if (p.state == ST_FF || p.state == ST_REW) media_resume(p.ff_time);
				p.state = ST_PLAY;
				break;
			case KEY_PAUSE:
			case KEY_PAUSECD:
			case KEY_PLAYPAUSE:
				if (p.state == ST_PLAY) p.state = ST_PAUSE;
				else if (p.state == ST_PAUSE) p.state = ST_PLAY;
				else if (p.state == ST_FF || p.state == ST_REW) { media_resume(p.ff_time); p.state = ST_PAUSE; }
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
				else if (num_chapters())
				{
					int c = chapter_index(mc.clock) + 1;
					if (c < num_chapters()) media_seek(chapter_start(c));
					else if (in_playlist && svcd_on) result = END_NEXT; // next track
				}
				else media_seek(mc.clock + MC_SKIP_SECONDS);
				break;
			case KEY_LEFT:
			case KEY_PREVIOUSSONG:
				if (in_playlist && mc.audio_only && mc.clock < 3.0) result = END_PREV;
				else if (mc.audio_only) media_seek(0);
				else if (num_chapters())
				{
					int c = chapter_index(mc.clock);
					if (c >= 0 && mc.clock - chapter_start(c) < 3.0) c--;
					if (c < 0 && in_playlist && svcd_on && mc.clock < 3.0) result = END_PREV; // previous track
					else media_seek(c >= 0 ? chapter_start(c) : 0);
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
				if (p.state == want) { media_resume(p.ff_time); p.state = ST_PLAY; break; }
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

		acc_fallback();
		acc_resume();
		AVFrame *show = NULL;
		if (p.state == ST_PLAY)
		{
			if (preroll)
			{
				pthread_mutex_lock(&vt.lock);
				int ready = mc.vq_count;
				int full = vq_full();
				pthread_mutex_unlock(&vt.lock);
				if (ready >= MC_VQ_ACC || full || t - preroll_t0 > MC_PREROLL || mc.eof || !acc.on || !vt.on)
				{
					preroll = 0;
					if (dd.on) printf("mediacenter: diag partenza: %d immagini pronte dopo %.0f ms\n", ready, 1000 * (t - preroll_t0));
				}
			}
			if (!preroll)
			{
				mc.clock += elapsed;
				played += elapsed;
			}
			fill();
			feed_audio_output();
			if (!preroll) av_sync(elapsed);
			if (vt.on)
			{
				pthread_mutex_lock(&vt.lock);
				vt.clock = mc.clock;
				show = due_frame();
				pthread_cond_broadcast(&vt.cond);
				pthread_mutex_unlock(&vt.lock);
			}
			else show = due_frame();

			// A stream too heavy for the CPU: skip pictures nothing refers to
			// until the decoder catches up with the clock.
			if (mc.vdec && !vt.on)
			{
				// Far behind: decode only key pictures until it catches up, so
				// the audio keeps being decoded in time.
				double lag = isnan(mc.last_vpts) ? 0 : mc.clock - mc.last_vpts;
				enum AVDiscard skip = skip_base;
				if (mc.vq_count == 0 && lag > 0.3 && skip < AVDISCARD_NONREF) skip = AVDISCARD_NONREF;
				if (mc.vq_count == 0 && lag > 1.5) skip = AVDISCARD_NONKEY;
				mc.vdec->skip_frame = skip;
			}

			if (vt.on) pthread_mutex_lock(&vt.lock);
			int video_done = mc.vstream < 0 || (mc.flushed && (!vt.on || vt.drained) && !mc.vq_count);
			if (vt.on) pthread_mutex_unlock(&vt.lock);
			int audio_done = mc.astream < 0 || (mc.flushed && !mc.pend_len);
			if (mc.eof && mc.flushed && video_done && audio_done)
			{
				if (show) draw_video_frame(show);
				av_frame_free(&show);
				int e = play_end_hook ? play_end_hook() : 0;
				if (e == 1) media_seek(0);           // a moving menu picture plays again
				else if (e == 2) mc.clock -= elapsed; // a menu: the last picture stays
				else
				{
					result = in_playlist && (mc.audio_only || svcd_on) ? END_NEXT : END_DONE;
					break;
				}
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
		if (format_tick(t)) p.dirty = 1;
		int show_osd = p.state == ST_STOP || t < p.osd_until || mc.audio_only || play_osd_keep;
		if (show_osd && (p.dirty || t - osd_built >= 0.25))
		{
			osd_info_t info;
			memset(&info, 0, sizeof(info));
			info.state = p.state;
			info.confirm_exit = p.confirm_exit;
			info.time = (p.state == ST_FF || p.state == ST_REW) ? p.ff_time : mc.clock;
			info.duration = mc.duration;
			info.chapters = num_chapters();
			info.chapter = info.chapters ? chapter_index(info.time) + 1 : 0;
			info.audio_mode = audio_mode;
			info.audio_label = mc.audio_label[0] ? mc.audio_label : NULL;
			if (mc.astream < 0) info.audio_label = "Senza audio";
			if (play_osd_hook) play_osd_hook(&info);
			info.format_label = format_label();
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
			// A picture of the FPGA accelerator: the core draws all of it,
			// the band under the controls first without them, and the ARM
			// blends the controls after; drawn again on the page on screen
			// that band would flash. While pictures keep coming the next one
			// brings the controls; otherwise it is drawn on a new page.
			int accp = have_frame && fb_present_yuv && yuv_last && acc.on && acc_buf_of(yuv_last);
			if (!accp) fb_present();
			else if (!(p.state == ST_PLAY && t - video_drawn_t < 0.1))
			{
				fb_redraw = 1;
				fb_present_paged();
				fb_redraw = 0;
			}
		}
		p.dirty = 0;

		usleep(p.state == ST_PLAY ? 2000 : 10000);
	}

	vt_end();
	fmt_close();
	main_idle(0);
	bg_pause(0);
	if (mc.vstream >= 0 && played > 1)
	{
		double fps = av_q2d(mc.fmt->streams[mc.vstream]->avg_frame_rate);
		printf("mediacenter: %.0f s played, pictures shown %d (%.1f per second", played, frames_shown, frames_shown / played);
		if (fps > 0) printf(" of %.2f, %.1f%% not shown", fps, 100.0 * (1.0 - frames_shown / (played * fps)));
		printf("), dropped late %d\n", frames_dropped);
		if (draw_count)
			printf("mediacenter: per picture: colour conversion %.1f ms, drawing on screen %.1f ms\n",
				1000 * draw_convert / draw_count, 1000 * draw_present / draw_count);
		zcp_report();
		acc_report();
		if (dd.on && dd.n)
			printf("mediacenter: diag decoder: %d immagini decodificate, media %.1f ms, peggiore %.1f ms, %d oltre %.1f ms (%.1f%%), %d saltate per recuperare\n",
				dd.n, 1000 * dd.sum / dd.n, 1000 * dd.max, dd.over, 1000 * dd.budget, 100.0 * dd.over / dd.n, dd.skipped);
		if (dd.on && dd.n)
			printf("mediacenter: diag picchi: %d immagini decodificate oltre %.1f ms, serie piu' lunga oltre %.1f ms %d\n",
				dd.peaks, 2000 * dd.budget, 1000 * dd.budget, dd.run_max);
		if (dd.on && dd.pkts)
			printf("mediacenter: diag bitrate video: medio %.2f Mbit/s, massimo in 10 s %.2f Mbit/s\n",
				8 * dd.bytes / (dd.pkts * dd.budget) / 1e6, dd.rate_max / 1e6);
		if (dd.on && ld.s_drawn)
			printf("mediacenter: diag ciclo: %d giri oltre %.1f ms; disegno medio %.1f ms, di cui attesa pagina libera %.1f ms; coda vuota %d volte\n",
				ld.s_slow, 1000 * dd.budget, 1000 * ld.s_draw / ld.s_drawn, 1000 * ld.s_wait / ld.s_drawn, ld.s_empty);
		av_report();
	}
	prio_apply(0);
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 0);
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
	// the same disk mounted on two /media/usbN (it happens) is listed once
	dev_t seen[8];
	int nseen = 0;
	for (int i = 0; i < 8; i++)
	{
		char path[32], label[32];
		snprintf(path, sizeof(path), "/media/usb%d", i);
		if (!dir_has_entries(path)) continue;
		struct stat st;
		if (!stat(path, &st))
		{
			int dup = 0;
			for (int k = 0; k < nseen; k++) dup |= seen[k] == st.st_dev;
			if (dup)
			{
				static unsigned told; // said once in the log
				if (!(told & 1u << i)) printf("mediacenter: %s e' lo stesso disco di una USB gia' in lista, non ripetuto\n", path);
				told |= 1u << i;
				continue;
			}
			seen[nseen++] = st.st_dev;
		}
		snprintf(label, sizeof(label), "USB %d", i);
		add_source(label, path);
	}
	if (dir_has_entries("/media/fat/cifs")) add_source("Rete (CIFS)", "/media/fat/cifs");
	add_source("Regola CRT (dimensioni e posizione)", "crt:");
}

// ---------------------------------------------------------------------------
// The lists and the screens of the player
//
// The menu core shows a frame buffer 720 pixels wide on the 15 kHz CRT one
// pixel for one, like a DVD, and its lines one for one when they are as many
// as those of the output mode. The lists are drawn in such a frame buffer
// (menu_fb, 720x576 or 720x480): every stroke of the letters stays on the
// CRT. In the 1280x720 one every other column was lost, and one line in five
// at 576i, three in five at 288p, and the letters came out broken. HDMI
// enlarges it like a video. The lists keep to its middle 640 pixels
// (menu_ox): the sides of a CRT are hidden under its frame.
// ---------------------------------------------------------------------------

static void menu_fb(void);

// Text with glyph pixels sx wide and sy tall; bold widens every stroke by
// one pixel. Returns the x after the text.
static int fb_text_xy(int x, int y, int sx, int sy, int bold, const char *str, uint32_t color)
{
	for (; *str; str++, x += GLYPH_W * sx)
	{
		const unsigned char *g = vcd_font[(unsigned char)*str & 0x7F];
		for (int c = 0; c < 8; c++)
			for (int r = 0; r < 8; r++)
				if ((g[c] >> r) & 1) fb_fill_rect(x + c * sx, y + r * sy, sx + bold, sy, color);
	}
	return x;
}

static int text_w(const char *str, int sx)
{
	return (int)strlen(str) * GLYPH_W * sx;
}

static void text_centre(int y, int sx, int sy, int bold, const char *str, uint32_t color)
{
	fb_text_xy((fb_w - text_w(str, sx)) / 2, y, sx, sy, bold, str, color);
}

// Glyph pixel of the list text: 2x2 at 720x480 and 720x576; 1x1 at 288 and
// 240 lines, whose lines are twice as tall on the CRT.
static int menu_sx(void) { int v = fb_w / 320; return v < 1 ? 1 : v; }
// Left edge and width of the part of the screen the lists use: the middle
// 640 pixels of the lists' 720 wide frame buffer, all of any other.
static int menu_ox(void) { return fb_menu && fb_w == 720 ? (fb_w - 640) / 2 : 0; }
static int menu_w(void) { return fb_w - 2 * menu_ox(); }
static int menu_sy(void) { int v = fb_h / 240; return v < 1 ? 1 : v; }

// A screen is drawn into a copy and then copied to the frame buffer: the
// CRT never shows it cleared or half drawn.
static uint8_t *menu_copy, *menu_screen;
static size_t menu_copy_len;

static void menu_begin(void)
{
	menu_fb();
	if (!fb_mem) return;
	if (menu_copy_len < fb_len)
	{
		free(menu_copy);
		menu_copy = (uint8_t *)malloc(fb_len);
		menu_copy_len = menu_copy ? fb_len : 0;
	}
	if (menu_copy)
	{
		menu_screen = fb_mem;
		fb_mem = menu_copy;
	}
	fb_fill_rect(0, 0, fb_w, fb_h, fb_rgb(0, 0, 0));
}

static void menu_end(void)
{
	if (!menu_screen) return;
	memcpy(menu_screen, fb_mem, fb_len);
	fb_mem = menu_screen;
	menu_screen = NULL;
}

// Where the list rows go; rows is also the page of the Left/Right keys.
static void browser_layout(int *top, int *row_h, int *rows)
{
	int sy = menu_sy();
	*top = 48 * sy;
	*row_h = 12 * sy;
	*rows = (fb_h - 20 * sy - *top) / *row_h;
	if (*rows < 1) *rows = 1;
}

static void draw_browser(const char *title, const char *const *labels, const int *dirs, int count, int sel, const char *message)
{
	menu_begin();
	int sx = menu_sx(), sy = menu_sy(), gw = GLYPH_W * sx;
	int ox = menu_ox(), mw = menu_w();
	int mx = 12 * sx;
	uint32_t white = fb_rgb(255, 255, 255), grey = fb_rgb(160, 160, 160), accent = fb_rgb(40, 90, 190);

	// the name: centred, twice as wide and three times as tall as the list
	const char *name = "MiSTer MediaCenter";
	int tsx = text_w(name, 2 * sx) <= mw - 2 * mx ? 2 * sx : sx;
	text_centre(6 * sy, tsx, 3 * sy, 1, name, white);

	// where we are, and the place in a long list
	int top, row_h, rows;
	browser_layout(&top, &row_h, &rows);
	char line[256], pos[32] = "";
	if (count > rows) snprintf(pos, sizeof(pos), "%d/%d", sel + 1, count);
	fit_text(line, sizeof(line), title, (mw - 2 * mx - text_w(pos, sx)) / gw - (pos[0] ? 1 : 0));
	fb_text_xy(ox + mx, 34 * sy, sx, sy, 0, line, grey);
	if (pos[0]) fb_text_xy(ox + mw - mx - text_w(pos, sx), 34 * sy, sx, sy, 0, pos, grey);

	int first = sel - rows / 2;
	if (first > count - rows) first = count - rows;
	if (first < 0) first = 0;
	if (!count) fb_text_xy(ox + mx, top + 2 * sy, sx, sy, 1, message ? message : "Nessun file video o audio qui", grey);
	for (int i = first; i < count && i < first + rows; i++)
	{
		int y = top + (i - first) * row_h;
		if (i == sel) fb_fill_rect(ox + mx / 2, y, mw - mx, row_h, accent);
		char item[260];
		snprintf(item, sizeof(item), "%s%s", labels[i], dirs && dirs[i] ? "/" : "");
		fit_text(line, sizeof(line), item, (mw - 2 * mx) / gw);
		fb_text_xy(ox + mx, y + 2 * sy, sx, sy, 1, line, white);
	}

	// the keys: centred, as large as the list and bold
	const char *help = "A Apri  B Indietro  \x10\x11 Pagina  X Esci";
	if (text_w(help, sx) > mw) help = "A Apri  B Indietro  X Esci";
	text_centre(fb_h - 14 * sy, sx, sy, 1, help, fb_rgb(230, 230, 230));
	menu_end();
}

// A message in a box over the screen just drawn.
static void menu_notice(const char *text)
{
	int sx = menu_sx(), sy = menu_sy();
	int w = text_w(text, sx) + 4 * GLYPH_W * sx / 2, h = 16 * sy;
	if (w > menu_w()) w = menu_w();
	int x = (fb_w - w) / 2, y = (fb_h - h) / 2;
	fb_fill_rect(x, y, w, h, fb_rgb(255, 200, 80));
	fb_fill_rect(x + sx, y + sy, w - 2 * sx, h - 2 * sy, fb_rgb(0, 0, 0));
	text_centre(y + 4 * sy, sx, sy, 1, text, fb_rgb(255, 200, 80));
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
static int svcd_play(void);

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

static int wait_key_for(double secs);

// Power-on: the MiSTer's start-up scripts (rcS and the S.. scripts it
// starts: USB mounting, udev, dbus, network) run for some 30-40 s after the
// Main is up. A video started meanwhile shared the CPU with them and
// stuttered for its first minute, so it waits for them to end.
// The rcS that started us, if any, never ends while we run: not counted.
static int boot_scripts_running(void)
{
	int mine[16], n = 0;
	for (int pid = getppid(); pid > 1 && n < 16;)
	{
		mine[n++] = pid;
		char name[32];
		int pp = 0;
		if (!proc_name(pid, name, sizeof(name), &pp)) break;
		pid = pp;
	}
	DIR *d = opendir("/proc");
	if (!d) return 0;
	int found = 0;
	for (struct dirent *de; !found && (de = readdir(d));)
	{
		int pid = atoi(de->d_name);
		if (pid <= 1) continue;
		char name[32];
		if (!proc_name(pid, name, sizeof(name), NULL) || strcmp(name, "rcS")) continue;
		// an ended one not yet collected by init is a zombie: state Z
		char path[64], st[256];
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		ssize_t r = fd >= 0 ? read(fd, st, sizeof(st) - 1) : -1;
		if (fd >= 0) close(fd);
		if (r <= 0) continue;
		st[r] = 0;
		char *c = strrchr(st, ')');
		if (c && c[1] == ' ' && c[2] == 'Z') continue;
		found = 1;
		for (int i = 0; i < n; i++) if (mine[i] == pid) found = 0;
	}
	closedir(d);
	return found;
}

// Returns 1 when B (Back) was pressed: the video is not started.
// MC_NOBOOTWAIT=1 starts it at once as before.
static int boot_wait(void)
{
	const char *e = getenv("MC_NOBOOTWAIT");
	if (e && atoi(e)) return 0;
	double up = 0;
	FILE *f = fopen("/proc/uptime", "re");
	if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
	if (up <= 0 || up > 180) return 0;
	if (!boot_scripts_running())
	{
		printf("mediacenter: avvio della MiSTer gia' finito (accesa da %.0f s)\n", up);
		return 0;
	}
	if (reopen_screen()) return 0;
	printf("mediacenter: la MiSTer sta finendo l'avvio (accesa da %.0f s): il video aspetta\n", up);
	double t0 = now_seconds(), ended = 0, look = -1;
	int shown = -1, back = 0;
	const char *why = "massimo 60 s";
	while (!quit_requested)
	{
		double t = now_seconds();
		if (!ended && t - look >= 0.5)
		{
			look = t;
			if (!boot_scripts_running()) ended = t;
		}
		if (ended && t - ended >= 2) { why = "avvio finito"; break; }
		if (t - t0 >= 60) break;
		int s = (int)(t - t0);
		if (s != shown)
		{
			shown = s;
			menu_begin();
			if (fb_mem)
			{
				int sx = menu_sx(), sy = menu_sy();
				char line[64];
				text_centre(fb_h / 2 - 24 * sy, sx, sy, 1, "La MiSTer sta finendo l'avvio", fb_rgb(255, 200, 80));
				snprintf(line, sizeof(line), "Il video parte da solo  (%d s)", s);
				text_centre(fb_h / 2 - 4 * sy, sx, sy, 1, line, fb_rgb(255, 255, 255));
				text_centre(fb_h - 14 * sy, sx, sy, 1, "B Indietro", fb_rgb(230, 230, 230));
			}
			menu_end();
		}
		int code = wait_key_for(0.1);
		if (code == KEY_ESC || code == KEY_BACKSPACE) { back = 1; why = "B premuto"; break; }
	}
	printf("mediacenter: attesa fine avvio %.1f s (%s)\n", now_seconds() - t0, why);
	return back;
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

	// Super Video CD (MPEG-2) and Video CD (MPEG-1): FFmpeg, so the screen
	// is set up for the CRT as for a DVD (the Video CD player kept the HDMI
	// frame buffer). MC_VCD_OLD=1 keeps the Video CD player for Video CDs.
	if (kind && (strcmp(kind, "Video CD") || !getenv("MC_VCD_OLD")))
	{
		printf("mediacenter: %s, %d track(s)\n", kind, track_last);
		if (reopen_screen()) return -1;
		svcd_kind = strcmp(kind, "Video CD") ? "Super Video CD" : "Video CD";
		if (!boot_wait()) svcd_play();
		svcd_kind = "Super Video CD";
		close(cd_fd);
		cd_fd = -1;
		return 0;
	}
	close(cd_fd);
	cd_fd = -1;

	if (!kind)
	{
		if (dvd_detect(device))
		{
			if (reopen_screen()) return -1;
			if (!boot_wait()) dvd_play(device);
			return 0;
		}
		return browse_data_disc(device);
	}

	if (boot_wait()) return 0;
	// The Video CD player opens the framebuffer and the controls itself and
	// releases them when it ends.
	fb_close();
	if (input_fd >= 0) close(input_fd);
	input_fd = -1;

	char *argv[3] = { (char *)"vcd_player", (char *)device, NULL };
	int argc = 2;
	fb_aspect = 4.0 / 3.0;
	vcd_main(argc, argv);

	fb_mem = fb_map = NULL;
	input_fd = -1;
	src_w = src_h = 0;
	return 0;
}

static int reopen_screen(void)
{
	if (!fb_mem && fb_open_pages(0)) return -1;
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

// A Super Video CD copied to the card as .bin + .cue: played like the disc
// in the drive, reading the sectors from the image.
static int play_cue_image(const char *cue)
{
	FILE *f = fopen(cue, "r");
	if (!f) return -1;
	char line[512], bin[1100] = "";
	int t = 0, last = 0;
	while (fgets(line, sizeof(line), f))
	{
		char name[400], mode[32];
		int n, mm, ss, ff;
		if (sscanf(line, " FILE \"%399[^\"]\"", name) == 1 && !bin[0])
		{
			const char *slash = strrchr(cue, '/');
			if (name[0] == '/' || !slash) snprintf(bin, sizeof(bin), "%s", name);
			else snprintf(bin, sizeof(bin), "%.*s/%s", (int)(slash - cue), cue, name);
		}
		else if (sscanf(line, " TRACK %d %31s", &n, mode) == 2 && n >= 1 && n <= MAX_TRACKS)
		{
			t = n;
			tracks[t].data = strcasecmp(mode, "AUDIO") != 0;
			tracks[t].start = -1;
			if (t > last) last = t;
		}
		else if (t && sscanf(line, " INDEX %d %d:%d:%d", &n, &mm, &ss, &ff) == 4 && n == 1)
			tracks[t].start = (mm * 60 + ss) * 75 + ff;
	}
	fclose(f);
	if (!bin[0] || !last) return -1;
	img_fd = open(bin, O_RDONLY | O_CLOEXEC);
	if (img_fd < 0) { printf("mediacenter: cannot open %s\n", bin); return -1; }
	struct stat st;
	fstat(img_fd, &st);
	track_first = 1;
	track_last = last;
	for (int i = 1; i <= last; i++)
		tracks[i].end = i < last ? tracks[i + 1].start : (int)(st.st_size / RAW_SECTOR);
	tracks[0].start = (int)(st.st_size / RAW_SECTOR); // lead-out

	const char *kind = detect_vcd_kind();
	printf("mediacenter: image %s: %s, %d track(s)\n", bin, kind ? kind : "not a Video CD", last);
	int r = -1;
	if (kind && strcmp(kind, "Video CD"))
	{
		if (!reopen_screen()) { svcd_play(); r = 0; }
	}
	else if (kind) printf("mediacenter: Video CD (MPEG-1) images are not supported yet\n");
	close(img_fd);
	img_fd = -1;
	return r;
}

static int play_file(const char *path, int in_playlist)
{
	if (boot_wait()) return END_BACK;
	if (is_dvd_path(path)) return dvd_play(path);
	if (has_ext(path, (const char *const[]){ "cue", NULL }))
	{
		play_cue_image(path);
		return END_DONE;
	}
	if (media_open(path)) return END_DONE;
	native_retry();
	native_fb = !mc.audio_only && !getenv("MC_ARMSCALE");
	int r = media_play(in_playlist);
	native_fb = 0;
	fb_native(0, 0, 0);
	media_close();
	return r;
}

// ---------------------------------------------------------------------------
// Super Video CD
// ---------------------------------------------------------------------------

// The MPEG-2 tracks of a Super Video CD are Mode 2 Form 2 sectors: FFmpeg
// gets the 2324-byte user data of each sector of the track as one file.
static feed_t svcd_feed;

static int svcd_read(void *opaque, uint8_t *buf, int size)
{
	feed_t *f = (feed_t *)opaque;
	if (f->pos >= f->size || quit_requested) return AVERROR_EOF;

	int first = (int)(f->pos / FORM2_DATA);
	int count = (f->end - f->start) - first;
	if (count > READ_BATCH) count = READ_BATCH;
	int need = (int)((size + f->pos % FORM2_DATA + FORM2_DATA - 1) / FORM2_DATA);
	if (count > need) count = need;
	if (count <= 0) return AVERROR_EOF;

	read_raw(f->start + first, count, f->raw);
	int done = 0;
	for (int i = 0; i < count && done < size && f->pos < f->size; i++)
	{
		sector_block(f->raw + i * RAW_SECTOR, f->block);
		size_t off = f->pos % FORM2_DATA;
		size_t n = FORM2_DATA - off;
		if (n > (size_t)(size - done)) n = size - done;
		if (n > f->size - f->pos) n = f->size - f->pos;
		memcpy(buf + done, f->block + off, n);
		done += (int)n;
		f->pos += n;
	}
	return done ? done : AVERROR_EOF;
}

static int64_t svcd_seek(void *opaque, int64_t offset, int whence)
{
	feed_t *f = (feed_t *)opaque;
	if (whence == AVSEEK_SIZE) return (int64_t)f->size;
	whence &= ~AVSEEK_FORCE;
	if (whence == SEEK_CUR) offset += (int64_t)f->pos;
	else if (whence == SEEK_END) offset += (int64_t)f->size;
	else if (whence != SEEK_SET) return -1;
	if (offset < 0) return -1;
	f->pos = (size_t)offset < f->size ? (size_t)offset : f->size;
	return (int64_t)f->pos;
}

// System clock of the MPEG pack that starts at lba (or within the next few
// sectors), in seconds; -1 when there is none.
static double svcd_pack_time(int lba, int end)
{
	int count = end - lba;
	if (count > 8) count = 8;
	if (count <= 0) return -1;
	read_raw(lba, count, svcd_feed.raw);
	for (int i = 0; i < count; i++)
	{
		int len = 0;
		const uint8_t *p = sector_payload(svcd_feed.raw + i * RAW_SECTOR, &len);
		if (len < 12 || p[0] || p[1] || p[2] != 1 || p[3] != 0xBA) continue;
		uint64_t scr;
		if ((p[4] & 0xC0) == 0x40) // MPEG-2
			scr = ((uint64_t)((p[4] >> 3) & 7) << 30) | ((uint64_t)(p[4] & 3) << 28) | ((uint64_t)p[5] << 20) |
				((uint64_t)(p[6] >> 3) << 15) | ((uint64_t)(p[6] & 3) << 13) | ((uint64_t)p[7] << 5) | (p[8] >> 3);
		else if ((p[4] & 0xF0) == 0x20) // MPEG-1
			scr = ((uint64_t)((p[4] >> 1) & 7) << 30) | ((uint64_t)p[5] << 22) | ((uint64_t)(p[6] >> 1) << 15) |
				((uint64_t)p[7] << 7) | (p[8] >> 1);
		else
			continue;
		return scr / 90000.0;
	}
	return -1;
}

// Chapters of the track from the ENTRIES.SVD points that fall in it: the time
// of each is the clock of the MPEG pack it points to.
static void svcd_track_chapters(int track)
{
	svcd_chapters = 0;
	for (int i = 0; i < num_entries && svcd_chapters < MAX_ENTRIES; i++)
	{
		if (entries[i].track != track) continue;
		double t = svcd_pack_time(entries[i].lba, tracks[track].end);
		if (t < 0) continue;
		t -= mc.start;
		if (t < 0) t = 0;
		if (svcd_chapters && t <= svcd_chapter[svcd_chapters - 1]) continue;
		svcd_chapter[svcd_chapters++] = t;
	}
	if (svcd_chapters == 1) svcd_chapters = 0; // only the track start
}

// Plays sectors start..end of the disc (a track, or menu segments). With a
// track its chapters are known; seek_lba starts at an entry point.
// Returns how playback ended, -1 when there is nothing to play.
static int svcd_play_range(int start, int end, int track, int seek_lba, const char *name, int in_playlist)
{
	feed_t *f = &svcd_feed;
	f->start = start;
	f->end = end;
	f->pos = 0;
	f->size = (size_t)(f->end - f->start) * FORM2_DATA;

	const int io_size = 32 * FORM2_DATA;
	uint8_t *io_buf = (uint8_t *)av_malloc(io_size);
	media_io = io_buf ? avio_alloc_context(io_buf, io_size, 0, f, svcd_read, NULL, svcd_seek) : NULL;
	if (!media_io)
	{
		av_free(io_buf);
		return -1;
	}

	int r = -1;
	if (!media_open(name))
	{
		snprintf(mc.title, sizeof(mc.title), "%s", name);
		if (track) svcd_track_chapters(track);
		double t = seek_lba >= 0 ? svcd_pack_time(seek_lba, end) - mc.start : 0;
		if (t >= 0.5) media_seek(t); // otherwise from the first picture already read
		fprintf(stderr, "mediacenter: %s, sectors %d-%d, %d chapter(s)%s\n", name, start, end, svcd_chapters,
			seek_lba >= 0 ? ", from an entry point" : "");
		native_retry();
		native_fb = !mc.audio_only && !getenv("MC_ARMSCALE");
		r = media_play(in_playlist);
		native_fb = 0;
		fb_native(0, 0, 0);
		media_close();
	}
	else fprintf(stderr, "mediacenter: %s has no MPEG stream\n", name);

	av_freep(&media_io->buffer);
	avio_context_free(&media_io);
	svcd_chapters = 0;
	return r;
}

// ---------------------------------------------------------------------------
// Super Video CD menus (PBC): the lists of PSD.SVD, read by read_pbc() as for
// a Video CD 2.0, played item by item through media_play().
// ---------------------------------------------------------------------------

static struct
{
	const uint8_t *list; // current list
	int item, n;         // play list: item playing, number of items
	int sel;             // menu: current choice
	int loops;           // menu: plays of its picture still to do (0 = always)
	int holding;         // the last picture stays on screen
	double until;        // ... until then (0 = until a key)
	int goto_ofs;        // list chosen with a key
} spbc;

static int spbc_jump(int ofs)
{
	if (ofs == PSD_OFS_DISABLED || !psd_list(ofs)) return 1;
	spbc.goto_ofs = ofs;
	return 2;
}

static int spbc_is_menu(void)
{
	return spbc.list && spbc.list[0] != 0x10;
}

static int spbc_key(int code)
{
	const uint8_t *d = spbc.list;
	if (!d) return 0;
	// B: back to the first list, as a player's stop button does
	if (code == KEY_ESC || code == KEY_STOP || code == KEY_STOPCD) return spbc_jump(pbc_first);

	if (spbc_is_menu())
	{
		int nos = d[2], bsn = d[3];
		switch (code)
		{
		case KEY_UP:
			if (nos) spbc.sel = bsn + (spbc.sel - bsn + nos - 1) % nos;
			return 1;
		case KEY_DOWN:
			if (nos) spbc.sel = bsn + (spbc.sel - bsn + 1) % nos;
			return 1;
		case KEY_ENTER:
			if (spbc.sel - bsn < nos && 20 + 2 * (spbc.sel - bsn) + 2 <= psd_size - (d - psd))
				return spbc_jump(be16(d + 20 + 2 * (spbc.sel - bsn)));
			return 1;
		case KEY_PLAY:
		case KEY_PLAYCD:
		{
			int ofs = be16(d + 12); // default
			if (ofs == PSD_OFS_MULTI_DEF || ofs == PSD_OFS_MULTI_DEF_NO_NUM) ofs = be16(d + 20);
			return spbc_jump(ofs);
		}
		case KEY_RIGHT: return spbc_jump(be16(d + 8));  // next
		case KEY_LEFT:  return spbc_jump(be16(d + 6));  // previous
		case KEY_SPACE: return spbc_jump(be16(d + 10)); // return
		case KEY_PAGEUP:
		case KEY_PAGEDOWN:
		case KEY_FASTFORWARD:
		case KEY_REWIND:
			return 1; // no fast forward / rewind in menus
		}
		return 0;
	}

	// Play list
	switch (code)
	{
	case KEY_SPACE:
		return spbc_jump(be16(d + 8)); // return, usually the menu
	case KEY_PLAY:
	case KEY_PLAYCD:
	case KEY_RIGHT:
		return spbc.holding ? spbc_jump(be16(d + 6)) : 0; // waiting at the end: next
	case KEY_LEFT:
		return spbc.holding ? spbc_jump(be16(d + 4)) : 0; // previous
	}
	return 0;
}

static int spbc_end(void)
{
	const uint8_t *d = spbc.list;
	if (!d) return 0;
	int menu = spbc_is_menu();
	if (!menu && spbc.item < spbc.n - 1) return 0; // next item of the list
	if (menu && !spbc.holding && mc.duration >= 1.0 && spbc.loops != 1)
	{
		if (spbc.loops > 1) spbc.loops--;
		return 1;
	}
	if (!spbc.holding)
	{
		// wait and timeout: seconds, 255 = until a key
		spbc.holding = 1;
		double w = pbc_wait_seconds(menu ? d[16] : d[12]);
		if (menu && be16(d + 14) == PSD_OFS_DISABLED) w = -1;
		spbc.until = w >= 0 ? now_seconds() + w : 0;
	}
	if (spbc.until && now_seconds() >= spbc.until) return 0;
	return 2;
}

static void spbc_osd(osd_info_t *info)
{
	const uint8_t *d = spbc.list;
	info->has_menu = 1;
	if (!spbc_is_menu()) return;
	info->menu = 1;
	info->sel = spbc.sel;
	info->sel_min = d[3];
	info->sel_max = d[3] + d[2] - 1;
}

static int spbc_item(int item)
{
	char name[64];
	if (item >= 2 && item <= 99)
	{
		if (item > track_last || !tracks[item].data) return -1;
		snprintf(name, sizeof(name), "%s - traccia %d", svcd_kind, item - 1);
		return svcd_play_range(tracks[item].start, tracks[item].end, item, -1, name, 1);
	}
	if (item >= 100 && item < 600)
	{
		int e = item - 100;
		if (e >= num_entry_items || !entry_items[e].track) return -1;
		int t = entry_items[e].track;
		snprintf(name, sizeof(name), "%s - traccia %d", svcd_kind, t - 1);
		return svcd_play_range(tracks[t].start, tracks[t].end, t, entry_items[e].lba, name, 1);
	}
	if (item >= 1000 && item < 1000 + seg_count)
	{
		// a play item can span several segments: the following ones are
		// marked as continuations
		int s = item - 1000, n = 1;
		while (s + n < seg_count && (seg_info[s + n] & 0x20)) n++;
		snprintf(name, sizeof(name), "%s - menu", svcd_kind);
		return svcd_play_range(seg_first + s * SEGMENT_SECTORS, seg_first + (s + n) * SEGMENT_SECTORS, 0, -1, name, 1);
	}
	return -1;
}

static int svcd_pbc(void)
{
	int ofs = pbc_first, jumps = 0, r = END_DONE;
	play_key_hook = spbc_key;
	play_end_hook = spbc_end;
	play_osd_hook = spbc_osd;
	while (!quit_requested)
	{
		const uint8_t *d = psd_list(ofs);
		if (!d || (d[0] != 0x10 && d[0] != 0x18 && d[0] != 0x1A)) break; // end list
		if (++jumps > 64)
		{
			fprintf(stderr, "mediacenter: the menus loop without playing anything\n");
			break;
		}
		memset(&spbc, 0, sizeof(spbc));
		spbc.list = d;
		spbc.goto_ofs = -1;
		int next;
		if (d[0] == 0x10)
		{
			spbc.n = d[1];
			fprintf(stderr, "mediacenter: play list (lid %d), %d item(s)\n", be16(d + 2) & 0x7FFF, d[1]);
			r = END_DONE;
			for (spbc.item = 0; spbc.item < spbc.n && !quit_requested; spbc.item++)
			{
				if (14 + spbc.item * 2 + 2 > psd_size - (d - psd)) break;
				r = spbc_item(be16(d + 14 + spbc.item * 2));
				if (r >= 0) jumps = 0;
				if (r == END_QUIT || r == END_BACK || r == END_GOTO) break;
			}
			next = be16(d + 6);
		}
		else
		{
			spbc.sel = d[3];
			spbc.loops = d[17] & 0x7F;
			fprintf(stderr, "mediacenter: menu (lid %d), choices %d-%d\n", be16(d + 4) & 0x7FFF, d[3], d[3] + d[2] - 1);
			play_osd_keep = 1;
			r = spbc_item(be16(d + 18));
			play_osd_keep = 0;
			if (r >= 0) jumps = 0;
			next = be16(d + 14); // timeout
		}
		if (r == END_QUIT || r == END_BACK) break;
		if (r == END_GOTO) ofs = spbc.goto_ofs;
		else if (next != PSD_OFS_DISABLED) ofs = next;
		else break;
	}
	play_key_hook = NULL;
	play_end_hook = NULL;
	play_osd_hook = NULL;
	memset(&spbc, 0, sizeof(spbc));
	printf("mediacenter: end of the %s menus\n", svcd_kind);
	return quit_requested ? END_QUIT : r;
}

// Plays the disc: through its menus when it has them, otherwise the MPEG
// tracks in order, like a playlist.
static int svcd_play(void)
{
	int list[MAX_TRACKS + 1], n = 0;
	for (int t = 2; t <= track_last && t <= MAX_TRACKS; t++)
		if (tracks[t].data) list[n++] = t;
	if (!n)
	{
		fprintf(stderr, "mediacenter: the %s has no MPEG tracks\n", svcd_kind);
		return END_DONE;
	}
	read_entries();
	read_pbc();

	svcd_on = 1;
	int r = END_DONE;
	if (pbc_ok) r = svcd_pbc();
	else
	{
		int i = 0;
		while (i >= 0 && i < n && !quit_requested)
		{
			char name[64];
			snprintf(name, sizeof(name), "%s %d/%d", svcd_kind, i + 1, n);
			r = svcd_play_range(tracks[list[i]].start, tracks[list[i]].end, list[i], -1, name, 1);
			if (r == END_QUIT || r == END_BACK) break;
			if (r == END_PREV) i = i > 0 ? i - 1 : 0;
			else i++;
		}
	}
	svcd_on = 0;
	return quit_requested ? END_QUIT : r;
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

		draw_browser(cwd, labels, dirs, num_items, sel, NULL);
		int top, row_h, page;
		browser_layout(&top, &row_h, &page);
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

// ---------------------------------------------------------------------------
// CRT Adjust (after rmonic79's MiSTer CRT Adjust): size and position of the
// picture on the 15 kHz CRT output, for a CRT whose geometry is off. The menu
// core resamples the picture into the new rectangle; the sync timing does not
// change, so the CRT keeps its lock. HDMI is not affected. Saved in
// mediacenter.ini as MC_CRT_HSIZE, MC_CRT_HPOS, MC_CRT_VSIZE, MC_CRT_VPOS.
//
// The same screen chooses the mode of the 15 kHz output (MC_CRT_MODE) and its
// flicker filter (MC_CRT_FILTER), both done by the menu core, the picture's
// format (MC_CRT_ASPECT, also on HDMI) and pan & scan (MC_CRT_PANSCAN):
// - mode Auto: 576i for a PAL DVD, 480i for an NTSC one and for the lists,
//   or the forced 576i, 288p, 480i or 240p;
// - filter Auto: on for the lists and screens of the player (white text one
//   line tall would flicker on an interlaced output), off for the video,
//   whose subtitles and controls are already smoothed here;
// - format Auto: the video's own aspect (the DVD's 4:3 or 16:9 flag, the
//   file's display aspect); or fixed: Pieno fills the screen (stretched),
//   4:3, 16:9, Anamorfico 2.35:1, Cinema 1.85:1 (also chosen during the
//   film with Start: format_key);
// - pan & scan (CRT only): a picture wider than 4:3 fills the CRT's height
//   and its sides are cut, instead of black bars above and below.
// ---------------------------------------------------------------------------

#define CRT_ITEMS 8
#define CRT_MODE 4
#define CRT_FILTER 5
#define CRT_ASPECT 6
#define CRT_PANSCAN 7
static const struct { const char *key, *label; int min, max; } crt_items[CRT_ITEMS] = {
	{ "MC_CRT_HSIZE", "Larghezza", -16, 15 },
	{ "MC_CRT_HPOS", "Pos. orizzontale", -32, 31 },
	{ "MC_CRT_VSIZE", "Altezza", -16, 15 },
	{ "MC_CRT_VPOS", "Pos. verticale", -16, 15 },
	{ "MC_CRT_MODE", "Modo 15 kHz", 0, 4 },
	{ "MC_CRT_FILTER", "Filtro tremolio", 0, 2 },
	{ "MC_CRT_ASPECT", "Formato", 0, 5 },
	{ "MC_CRT_PANSCAN", "Pan & scan", 0, 1 },
};
static const char *const crt_mode_names[5] = { "Auto", "576i PAL", "288p PAL", "480i NTSC", "240p NTSC" };
static const char *const crt_filter_names[3] = { "Auto", "Spento", "Acceso" };
static const char *const crt_aspect_names[6] = { "Auto", "Pieno", "4:3", "16:9", "Anamorfico 2,35:1", "Cinema 1,85:1" };
static int crt_val[CRT_ITEMS];
static int crt_sent_mode = -1, crt_sent_filter = -1;
static int crt_std = -1; // standard of the video on screen: 1 PAL, 0 NTSC, -1 unknown

// The video's display aspect, or the format fixed in Regola CRT (on the
// screen of sw x sh pixels: Pieno fills it).
static double aspect_used(int sw, int sh)
{
	switch (crt_val[CRT_ASPECT])
	{
	case 1: return sw > 0 && sh > 0 ? (double)sw / sh : video_aspect;
	case 2: return 4.0 / 3.0;
	case 3: return 16.0 / 9.0;
	case 4: return 2.35;        // anamorphic widescreen
	case 5: return 1.85;        // flat cinema
	default: return video_aspect;
	}
}

// The mode sent to the core. A fixed mode chosen in Regola CRT always wins.
// In Auto, while a video is on screen, its frame rate chooses the standard:
// 576i 50 Hz for 25/50 fps, 480i 60 Hz for 23.976/29.97/59.94 fps, always
// interlaced: a 1080p drawn with half its lines (1920x540) keeps all 540 of
// them on the 576 or 480 lines, instead of losing a third in 288p/240p.
// The lists keep Auto (0): the core chooses from their lines and menu_pal.
static int crt_mode_now(void)
{
	if (crt_val[CRT_MODE]) return crt_val[CRT_MODE];
	if (!native_w || fb_menu || crt_std < 0) return 0;
	return crt_std ? 1 : 3;
}

static int crt_write(const char *cmd)
{
	int fd = open("/dev/MiSTer_cmd", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return -1;
	int n = (int)strlen(cmd);
	int ok = write(fd, cmd, n) == n;
	close(fd);
	return ok ? 0 : -1;
}

// The filter actually used: Auto turns it on outside the video pictures.
static int crt_filter_on(void)
{
	if (crt_val[CRT_FILTER]) return crt_val[CRT_FILTER] == 2;
	// the lists: on when interlaced; the video: off
	return !native_w || (fb_menu && fb_h >= 400);
}

// The mode the menu core chooses for the current frame buffer, with the same
// rules (menu.sv), for the log.
static const char *crt_expected(char *buf, size_t len)
{
	int m = crt_mode_now();
	int h = fb_h;
	const char *mode;
	if (m) mode = crt_mode_names[m];
	else
	{
		int pal = h == 576 || h == 288 ? 1 : h == 480 || h == 240 ? 0 : -1;
		int lines = pal == 1 ? 288 : 240;
		int scaled = fb_w > 720 || h > lines;
		if (!scaled) mode = pal == 1 ? "288p PAL (1:1)" : pal == 0 ? "240p NTSC (1:1)" : "240p/288p secondo menu_pal (1:1)";
		else if (h >= 400) mode = pal == 1 ? "576i PAL" : pal == 0 ? "480i NTSC" : "480i/576i secondo menu_pal";
		else mode = pal == 1 ? "288p PAL" : pal == 0 ? "240p NTSC" : "240p/288p secondo menu_pal";
	}
	snprintf(buf, len, "%s, filtro %s", mode, crt_filter_on() ? "acceso" : "spento");
	return buf;
}

// Sends the settings to the Main; the mode and the filter only when they
// change (the filter follows the frame buffer in Auto).
static void crt_send(void)
{
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "fb_cmd3 crt %d %d %d %d\n", crt_val[0], crt_val[1], crt_val[2], crt_val[3]);
	if (crt_write(cmd)) fprintf(stderr, "mediacenter: CRT adjust not sent\n");
	// how the picture fits the CRT's 4:3 screen: bars, filled (Pieno), pan & scan
	snprintf(cmd, sizeof(cmd), "fb_cmd3 crtfit %d\n", crt_val[CRT_ASPECT] == 1 ? 1 : crt_val[CRT_PANSCAN] ? 2 : 0);
	crt_write(cmd);
	int flt = crt_filter_on();
	int mode = crt_mode_now();
	if (mode != crt_sent_mode || flt != crt_sent_filter)
	{
		snprintf(cmd, sizeof(cmd), "fb_cmd3 crtmode %d %d\n", mode, flt);
		crt_write(cmd);
		if (mode != crt_sent_mode) crt_mode_sent_t = now_seconds();
		crt_sent_mode = mode;
		crt_sent_filter = flt;
	}
}

// The mode measured by the Main on the core's 15 kHz output; "" until known.
static const char *crt_measured(char *buf, size_t len)
{
	buf[0] = 0;
	FILE *f = fopen("/tmp/mediacenter_crt", "r");
	if (!f) return buf;
	unsigned w = 0, h = 0;
	int il = 0;
	double hz = 0, vz = 0;
	if (fscanf(f, "%u %u %d %lf %lf", &w, &h, &il, &hz, &vz) == 5 && h)
		snprintf(buf, len, "%ux%u%s %s, %.2f kHz, %.2f Hz", w, h, il ? "i" : "p",
			il ? "interlacciato" : "progressivo", hz, vz);
	fclose(f);
	return buf;
}

// Called when the frame buffer changes: Auto filter, and the log line.
static void crt_fb_changed(void)
{
	char e[96], m[96];
	crt_send();
	fprintf(stderr, "mediacenter: CRT 15 kHz atteso: %dx%d -> %s\n", fb_w, fb_h, crt_expected(e, sizeof(e)));
	if (crt_measured(m, sizeof(m))[0]) fprintf(stderr, "mediacenter: CRT 15 kHz misurato dal Main (ultimo): %s\n", m);
}

// The standard of the video being drawn, from its frame rate: 25 or 50 fps
// PAL, 23.976/24, 29.97/30 or 59.94/60 fps NTSC; otherwise from its lines
// (576/288 PAL, 480/240 NTSC); otherwise unknown (Auto as before).
// The core gets it with the frame buffer, when crt_fb_changed sends the mode.
static void crt_video_std(const AVFrame *f)
{
	double fps = 0;
	if (mc.vdec && mc.vdec->framerate.num > 0 && mc.vdec->framerate.den > 0) fps = av_q2d(mc.vdec->framerate);
	if (!(fps > 0) && mc.fmt && mc.vstream >= 0 && mc.vstream < (int)mc.fmt->nb_streams)
		fps = av_q2d(mc.fmt->streams[mc.vstream]->avg_frame_rate);
	int std = -1;
	if (fps > 0)
	{
		if (fabs(fps - 25) < 0.6 || fabs(fps - 50) < 1.0) std = 1;
		else if ((fps > 23.0 && fps < 24.6) || (fps > 29.0 && fps < 30.6) || (fps > 59.0 && fps < 60.6)) std = 0;
	}
	if (std < 0)
	{
		int h = f->height;
		std = h == 576 || h == 288 ? 1 : h == 480 || h == 240 ? 0 : -1;
	}
	if (std != crt_std)
	{
		fprintf(stderr, "mediacenter: CRT standard dal video: %.3f fps, %d righe -> %s\n", fps, f->height,
			std == 1 ? "PAL 50 Hz" : std == 0 ? "NTSC 60 Hz" : "come Auto");
		crt_std = std;
		if (native_w && !fb_menu) crt_send();
	}
}

// Reads the settings (mediacenter.ini, already in the environment) and sends
// them to the Main.
static void crt_load(void)
{
	for (int i = 0; i < CRT_ITEMS; i++)
	{
		const char *e = getenv(crt_items[i].key);
		int v = e ? atoi(e) : 0;
		crt_val[i] = v < crt_items[i].min ? crt_items[i].min : v > crt_items[i].max ? crt_items[i].max : v;
	}
	crt_send();
}

// Writes the values into mediacenter.ini, keeping its other lines.
static int crt_save(void)
{
	const char *path = "/media/fat/linux/mediacenter.ini";
	char *old = NULL;
	size_t len = 0;
	FILE *f = fopen(path, "r");
	if (f)
	{
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz > 0 && sz < 65536 && (old = (char *)malloc(sz + 1)))
		{
			len = fread(old, 1, sz, f);
			old[len] = 0;
		}
		fclose(f);
	}
	char tmp[64];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f) { free(old); return -1; }
	for (char *line = old; line && *line;)
	{
		char *next = strchr(line, '\n');
		size_t n = next ? (size_t)(next - line + 1) : strlen(line);
		const char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (strncmp(p, "MC_CRT_", 7)) fwrite(line, 1, n, f);
		line += n;
	}
	if (len && old[len - 1] != '\n') fputc('\n', f);
	for (int i = 0; i < CRT_ITEMS; i++)
	{
		char v[16];
		snprintf(v, sizeof(v), "%d", crt_val[i]);
		if (crt_val[i]) fprintf(f, "%s=%d\n", crt_items[i].key, crt_val[i]);
		setenv(crt_items[i].key, v, 1);
	}
	free(old);
	int err = fclose(f);
	if (!err) err = rename(tmp, path);
	sync();
	return err;
}

// The format chosen during the film: Start opens a box in place of the
// button legend, Left/Right (or Up/Down) change the format at once, on the
// CRT and over HDMI; A, Start or B close it, and so does a pause of 6 s.
// The choice is saved like Regola CRT's (MC_CRT_ASPECT, MC_CRT_PANSCAN).
static const struct { const char *name; int aspect, pan; } fmt_list[] = {
	{ "Auto", 0, 0 },
	{ "4:3", 2, 0 },
	{ "16:9", 3, 0 },
	{ "Cinema 1,85:1", 5, 0 },
	{ "Anamorfico 2,35:1", 4, 0 },
	{ "Pieno", 1, 0 },
	{ "Pan & scan", 0, 1 },
};
#define FMT_N ((int)(sizeof(fmt_list) / sizeof(fmt_list[0])))
static int fmt_open, fmt_changed;
static double fmt_until;

static int fmt_index(void)
{
	if (crt_val[CRT_PANSCAN]) return FMT_N - 1;
	for (int i = 0; i < FMT_N; i++)
		if (!fmt_list[i].pan && fmt_list[i].aspect == crt_val[CRT_ASPECT]) return i;
	return 0;
}

static void fmt_close(void)
{
	if (!fmt_open) return;
	fmt_open = 0;
	if (!fmt_changed) return;
	fmt_changed = 0;
	int err = crt_save();
	fprintf(stderr, "mediacenter: formato scelto durante il film: %s%s\n", fmt_list[fmt_index()].name,
		err ? " (non salvato nel file ini)" : "");
}

// A key while a film plays; 1 when the box took it. menu: a disc menu or a
// list is on screen (Start belongs to it).
static int format_key(int code, double t, int state, int menu)
{
	if (!fmt_open)
	{
		if ((code == KEY_PLAY || code == KEY_PLAYCD) && state == ST_PLAY && !menu)
		{
			fmt_open = 1;
			fmt_until = t + 6.0;
			return 1;
		}
		return 0;
	}
	int d = 0;
	switch (code)
	{
	case KEY_LEFT: case KEY_UP: d = FMT_N - 1; break;
	case KEY_RIGHT: case KEY_DOWN: d = 1; break;
	case KEY_ENTER: case KEY_PLAY: case KEY_PLAYCD: case KEY_ESC: case KEY_SPACE:
		fmt_close();
		return 1;
	default:
		// any other key closes the box and does its own job
		fmt_close();
		return 0;
	}
	int i = (fmt_index() + d) % FMT_N;
	crt_val[CRT_ASPECT] = fmt_list[i].aspect;
	crt_val[CRT_PANSCAN] = fmt_list[i].pan;
	crt_send();
	fmt_changed = 1;
	fmt_until = t + 6.0;
	return 1;
}

// Closes the box after its time; 1 when it did (the controls to redraw).
static int format_tick(double t)
{
	if (!fmt_open || t < fmt_until) return 0;
	fmt_close();
	return 1;
}

static const char *format_label(void)
{
	return fmt_open ? fmt_list[fmt_index()].name : NULL;
}

// A test picture for the adjustment: border, grid, centre cross and values.
// The lists' frame buffer: 720 pixels and the lines of the 15 kHz mode
// chosen in Regola CRT. Auto: 576 lines (576i) with menu_pal=1 in
// MiSTer.ini, otherwise 480 (480i), as the menu core chooses for the lists.
static int ini_menu_pal(void)
{
	static int v = -1;
	if (v >= 0) return v;
	v = 0;
	FILE *f = fopen("/media/fat/MiSTer.ini", "r");
	if (!f) return v;
	char l[256];
	while (fgets(l, sizeof(l), f))
	{
		char *p = l;
		while (*p == ' ' || *p == '\t') p++;
		if (strncasecmp(p, "menu_pal", 8)) continue;
		p += 8;
		while (*p == ' ' || *p == '\t') p++;
		if (*p++ != '=') continue;
		v = atoi(p) != 0;
	}
	fclose(f);
	return v;
}

static void menu_fb(void)
{
	if (getenv("MC_MENU_HD") && atoi(getenv("MC_MENU_HD"))) return; // the lists at 1280x720, as up to version 2.1
	static const int lines[5] = { 0, 576, 288, 480, 240 };
	int h = lines[crt_val[CRT_MODE]];
	if (!h) h = ini_menu_pal() ? 576 : 480;
	if (fb_menu && native_w == 720 && native_h == h) return;
	menu_asking = 1;
	fb_native(720, h, 4.0 / 3.0);
	menu_asking = 0;
}

// The measured mode in short, for the screen.
static const char *crt_measured_short(char *buf, size_t len)
{
	buf[0] = 0;
	FILE *f = fopen("/tmp/mediacenter_crt", "r");
	if (!f) return buf;
	unsigned w = 0, h = 0;
	int il = 0;
	double hz = 0, vz = 0;
	if (fscanf(f, "%u %u %d %lf %lf", &w, &h, &il, &hz, &vz) == 5 && h)
		snprintf(buf, len, "%ux%u%s %.2f kHz %.1f Hz", w, h, il ? "i" : "p", hz, vz);
	fclose(f);
	return buf;
}

static void crt_draw(int sel, const char *message)
{
	menu_begin();
	int sx = menu_sx(), sy = menu_sy(), gw = GLYPH_W * sx;
	uint32_t white = fb_rgb(255, 255, 255), grey = fb_rgb(160, 160, 160), accent = fb_rgb(40, 90, 190);
	uint32_t red = fb_rgb(220, 40, 40), green = fb_rgb(40, 200, 40);

	// the test pattern: grid, centre cross, red border on the edge of the
	// lists' part of the screen (the middle 640 pixels of 720: the same
	// place as when the lists were 640 wide, so the values stay valid)
	int ox = menu_ox(), mw = menu_w();
	for (int i = 1; i < 8; i++)
	{
		fb_fill_rect(ox + mw * i / 8, 0, sx, fb_h, grey);
		fb_fill_rect(ox, fb_h * i / 8, mw, sy, grey);
	}
	fb_fill_rect(fb_w / 2 - sx, 0, 2 * sx, fb_h, green);
	fb_fill_rect(ox, fb_h / 2 - sy, mw, 2 * sy, green);
	fb_fill_rect(ox, 0, mw, 2 * sy, red);
	fb_fill_rect(ox, fb_h - 2 * sy, mw, 2 * sy, red);
	fb_fill_rect(ox, 0, 2 * sx, fb_h, red);
	fb_fill_rect(ox + mw - 2 * sx, 0, 2 * sx, fb_h, red);

	int row_h = 11 * sy;
	int bw = 38 * gw, bh = 29 * row_h / 2 + 8 * sy;
	if (bw > mw - 8 * sx) bw = mw - 8 * sx;
	int bx = (fb_w - bw) / 2, by = (fb_h - bh) / 2;
	fb_fill_rect(bx, by, bw, bh, fb_rgb(0, 0, 0));
	int y = by + 4 * sy;
	text_centre(y, sx, sy, 1, "Regola CRT", white);
	y += row_h;
	text_centre(y, sx, sy, 0, "Il bordo rosso deve toccare i lati", grey);
	y += row_h * 3 / 2;
	for (int i = 0; i < CRT_ITEMS; i++, y += row_h)
	{
		char line[64];
		if (i == CRT_MODE) snprintf(line, sizeof(line), "%-17s %s", crt_items[i].label, crt_mode_names[crt_val[i]]);
		else if (i == CRT_FILTER) snprintf(line, sizeof(line), "%-17s %s", crt_items[i].label, crt_filter_names[crt_val[i]]);
		else if (i == CRT_ASPECT) snprintf(line, sizeof(line), "%-17s %s", crt_items[i].label, crt_aspect_names[crt_val[i]]);
		else if (i == CRT_PANSCAN) snprintf(line, sizeof(line), "%-17s %s", crt_items[i].label, crt_val[i] ? "Acceso" : "Spento");
		else snprintf(line, sizeof(line), "%-17s %+d", crt_items[i].label, crt_val[i]);
		if (i == sel) fb_fill_rect(bx + sx, y - 2 * sy, bw - 2 * sx, row_h, accent);
		fb_text_xy(bx + gw, y, sx, sy, 1, line, white);
	}
	y += row_h / 2;
	char m[64], line[96];
	snprintf(line, sizeof(line), "Misurata: %s", crt_measured_short(m, sizeof(m))[0] ? m : "non ancora");
	text_centre(y, sx, sy, 0, line, grey);
	y += row_h * 3 / 2;
	if (message) text_centre(y, sx, sy, 1, message, fb_rgb(255, 200, 80));
	else text_centre(y, sx, sy, 1, "Su/Giu Scegli  \x10\x11 Cambia", fb_rgb(230, 230, 230));
	y += row_h;
	text_centre(y, sx, sy, 1, "A Salva  B Annulla  Y Azzera", fb_rgb(230, 230, 230));
	menu_end();
}

// Waits for a key up to secs seconds; 0 also when the time runs out.
static int wait_key_for(double secs)
{
	double end = now_seconds() + secs;
	while (!quit_requested && now_seconds() < end)
	{
		int code = input_key(now_seconds());
		if (code) return code;
		usleep(20000);
	}
	return 0;
}

static void crt_adjust_screen(void)
{
	int saved[CRT_ITEMS];
	memcpy(saved, crt_val, sizeof(saved));
	int sel = 0;
	const char *message = NULL;
	char shown[96] = "", m[96];
	crt_draw(sel, message);
	while (!quit_requested)
	{
		// the measured mode is shown again when the Main reports a change
		int code = wait_key_for(0.5);
		if (!code)
		{
			if (strcmp(crt_measured(m, sizeof(m)), shown))
			{
				snprintf(shown, sizeof(shown), "%s", m);
				crt_draw(sel, message);
			}
			continue;
		}
		message = NULL;
		int d = 0;
		switch (code)
		{
		case KEY_UP: sel = (sel + CRT_ITEMS - 1) % CRT_ITEMS; crt_draw(sel, message); continue;
		case KEY_DOWN: sel = (sel + 1) % CRT_ITEMS; crt_draw(sel, message); continue;
		case KEY_LEFT: d = -1; break;
		case KEY_RIGHT: d = 1; break;
		case KEY_PAGEUP: case KEY_REWIND: d = -4; break;
		case KEY_PAGEDOWN: case KEY_FASTFORWARD: d = 4; break;
		case KEY_SPACE: memset(crt_val, 0, sizeof(crt_val)); crt_send(); crt_draw(sel, message); continue;
		case KEY_ENTER: case KEY_PLAY:
			if (crt_save())
			{
				message = "Impossibile salvare il file ini";
				crt_draw(sel, message);
				continue;
			}
			fprintf(stderr, "mediacenter: CRT salvato: %d %d %d %d, modo %s, filtro %s, formato %s, pan & scan %s\n", crt_val[0],
				crt_val[1], crt_val[2], crt_val[3], crt_mode_names[crt_val[CRT_MODE]], crt_filter_names[crt_val[CRT_FILTER]],
				crt_aspect_names[crt_val[CRT_ASPECT]], crt_val[CRT_PANSCAN] ? "acceso" : "spento");
			return;
		case KEY_ESC: case KEY_TAB: case KEY_STOP: case KEY_STOPCD:
			memcpy(crt_val, saved, sizeof(saved));
			crt_send();
			return;
		default: continue;
		}
		int v = crt_val[sel] + d;
		if (v < crt_items[sel].min) v = crt_items[sel].min;
		if (v > crt_items[sel].max) v = crt_items[sel].max;
		crt_val[sel] = v;
		crt_send();
		crt_draw(sel, message);
	}
}

#include "splash.h"

#ifndef MC_BUILD
#define MC_BUILD "locale"
#endif

// The MMC picture for 3 s when the player starts from the Scripts menu (a
// disc started by the Main goes straight to the film). It is drawn in the
// lists' frame buffer, shown as 4:3 on the CRT and over HDMI. Any key skips
// it; MC_SPLASH=0 in mediacenter.ini turns it off.
static void show_splash(void)
{
	const char *e = getenv("MC_SPLASH");
	if (e && !atoi(e)) return;
	uint8_t *pic = (uint8_t *)malloc(SPLASH_W * SPLASH_H);
	if (!pic) return;
	size_t n = 0;
	for (size_t i = 0; i + 1 < sizeof(splash_rle) && n < SPLASH_W * SPLASH_H; i += 2)
		for (int k = 0; k < splash_rle[i] && n < SPLASH_W * SPLASH_H; k++) pic[n++] = splash_rle[i + 1];
	uint32_t pal[256];
	for (int i = 0; i < 256; i++) pal[i] = fb_rgb(splash_pal[3 * i], splash_pal[3 * i + 1], splash_pal[3 * i + 2]);

	menu_begin();
	if (fb_mem)
	{
		// the lists' frame buffer is shown as 4:3: the picture fills it;
		// another one (MC_MENU_HD) keeps square pixels, black at the sides
		int dw = fb_w, dh = fb_h;
		if (!fb_menu)
		{
			dw = fb_h * 4 / 3;
			if (dw > fb_w) { dw = fb_w; dh = fb_w * 3 / 4; }
		}
		int x0 = (fb_w - dw) / 2, y0 = (fb_h - dh) / 2;
		for (int y = 0; y < dh; y++)
		{
			const uint8_t *src = pic + (size_t)(y * SPLASH_H / dh) * SPLASH_W;
			uint32_t *row = (uint32_t *)(fb_mem + (size_t)(y0 + y) * fb_stride) + x0;
			for (int x = 0; x < dw; x++) row[x] = pal[src[x * SPLASH_W / dw]];
		}
		// the build number in the black band at the bottom, right of the
		// centre, big enough to read on the CRT (24 lines out of 480)
		char b[32];
		snprintf(b, sizeof(b), "Build %s", MC_BUILD);
		int s = dh / 160 > 2 ? dh / 160 : 2;
		int tw = (int)strlen(b) * GLYPH_W * s;
		fb_text(x0 + dw * 560 / 640 - tw, y0 + dh * 436 / 480, s, b, fb_rgb(200, 200, 200));
	}
	menu_end();
	free(pic);
	wait_key_for(3.0);
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

		if (!strcmp(sources[sel].path, "crt:"))
		{
			crt_adjust_screen();
			continue;
		}
		if (!sources[sel].path[0])
		{
			if (play_disc_in_drive(NULL))
			{
				draw_browser("Scegli la sorgente", labels, NULL, num_sources, sel, NULL);
				menu_notice("Nessun disco leggibile");
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
	double t_decode = 0, t_convert = 0, t_copy = 0, t_yuv = 0;
	int yuv_frames = 0;
	uint8_t *rgb = NULL, *fb_copy = NULL;
	size_t rgb_size = 0;
	// The first 60 s of media are enough to judge; Ctrl+C stops earlier.
	for (int more = 1; more && !quit_requested;)
	{
		double media = !isnan(mc.last_vpts) ? mc.last_vpts : mc.audio_end;
		if (media >= 60.0) break;
		double a = now_seconds();
		more = decode_step();
		double b = now_seconds();
		t_decode += b - a;
		// the colour conversion and the copy to the screen, which the player
		// does for every picture it shows (every other line in light mode)
		for (int i = 0; i < mc.vq_count; i++)
		{
			AVFrame *f = mc.vq[i].frame;
			int h = f->height, linesize[AV_NUM_DATA_POINTERS];
			memcpy(linesize, f->linesize, sizeof(linesize));
			if (half_lines && !(f->flags & AV_FRAME_FLAG_INTERLACED))
			{
				h /= 2;
				for (int k = 0; k < AV_NUM_DATA_POINTERS; k++) linesize[k] *= 2;
			}
			int rs = (f->width * 4 + 63) & ~63;
			if (!rgb || (size_t)rs * h > rgb_size)
			{
				av_free(rgb);
				av_free(fb_copy);
				rgb_size = (size_t)rs * h;
				rgb = (uint8_t *)av_malloc(rgb_size);
				fb_copy = (uint8_t *)av_malloc(rgb_size);
			}
			a = now_seconds();
			mc.sws = sws_getCachedContext(mc.sws, f->width, h, (enum AVPixelFormat)f->format,
				f->width, h, AV_PIX_FMT_BGRA, SWS_POINT, NULL, NULL, NULL);
			uint8_t *dst[4] = { rgb, NULL, NULL, NULL };
			int stride[4] = { rs, 0, 0, 0 };
			if (mc.sws && rgb) sws_scale(mc.sws, (const uint8_t *const *)f->data, linesize, 0, h, dst, stride);
			b = now_seconds();
			t_convert += b - a;
			if (fb_copy) memcpy(fb_copy, rgb, (size_t)rs * h);
			t_copy += now_seconds() - b;
			// what the player does instead with the FPGA converting (YUV)
			if (f->format == AV_PIX_FMT_YUV420P && fb_copy)
			{
				int half = h != f->height;
				b = now_seconds();
				for (int y = 0; y < h; y++)
				{
					int ly = half ? 2 * y : y, cy = half ? y : y / 2;
					pack_yuyv(rgb, f->data[0] + (size_t)ly * f->linesize[0], f->data[1] + (size_t)cy * f->linesize[1],
						f->data[2] + (size_t)cy * f->linesize[2], f->width & ~1);
					memcpy(fb_copy + (size_t)y * f->width * 2, rgb, (size_t)f->width * 2);
				}
				t_yuv += now_seconds() - b;
				yuv_frames++;
			}
		}
		frames += mc.vq_count;
		vq_clear();
		mc.pend_len = 0;
	}
	av_free(rgb);
	av_free(fb_copy);
	double secs = now_seconds() - t0;
	double media = !isnan(mc.last_vpts) ? mc.last_vpts : mc.audio_end;
	printf("mediacenter: decoded %d pictures, %.1f s of media in %.1f s: %.0f%% of real time%s\n",
		frames, media, secs, secs > 0 ? 100.0 * media / secs : 0,
		secs > 0 && media / secs >= 1.0 ? " (plays)" : " (too slow)");
	if (frames)
	{
		double fps = mc.vstream >= 0 ? av_q2d(mc.fmt->streams[mc.vstream]->avg_frame_rate) : 0;
		printf("mediacenter: per picture: decoding (with audio) %.1f ms, colour conversion %.1f ms, copy %.1f ms",
			1000 * t_decode / frames, 1000 * t_convert / frames, 1000 * t_copy / frames);
		if (fps > 0) printf(", time available %.1f ms", 1000 / fps);
		printf("\nmediacenter: colour conversion and copy are %.0f%% of the work\n",
			100 * (t_convert + t_copy) / (t_decode + t_convert + t_copy));
		if (yuv_frames)
			printf("mediacenter: with the FPGA converting (YUV): copy %.1f ms per picture instead of %.1f ms\n",
				1000 * t_yuv / yuv_frames, 1000 * (t_convert + t_copy) / frames);
	}
	media_close();
	return 0;
}

// mediacenter --bench-dec <file> [seconds] and --bench-parse <file> [seconds]
// (FPGA accelerator, phase 0): decode only, with nothing drawn, and time it
// every 10 s of media. --bench-parse uses the MC_PARSEONLY change in our
// FFmpeg: the stream is read (CABAC, macroblocks, vectors, coefficients) but
// no pixel is rebuilt, which is what the ARM would still do if the FPGA
// rebuilt the pixels. Same decoder setup as playback (light mode, 2 threads).
static int bench_windows(const char *path, double secs, int parse)
{
	setenv("MC_NOZC", "1", 1); // nothing is drawn: leave the core alone
	if (parse) setenv("MC_PARSEONLY", "1", 1);
	if (media_open(path)) return 1;
	if (mc.vstream < 0 || !mc.vdec) { fprintf(stderr, "mediacenter: no video\n"); media_close(); return 1; }
	double fps = av_q2d(mc.fmt->streams[mc.vstream]->avg_frame_rate);
	double budget = fps > 0 ? 1000 / fps : 0;
	printf("mediacenter: bench %s, %s, %s\n", parse ? "SOLO LETTURA DEL FLUSSO (MC_PARSEONLY)" : "decodifica normale",
		avcodec_get_name(mc.vdec->codec_id), path);
	if (budget > 0) printf("mediacenter: tempo disponibile per immagine %.1f ms\n", budget);
	int frames = 0, win_frames = 0, win = 0, over = 0, wins = 0;
	double t0 = now_seconds(), win_t = t0, worst = 0;
	int worst_win = 0;
	for (int more = 1; more && !quit_requested;)
	{
		more = decode_step();
		frames += mc.vq_count;
		win_frames += mc.vq_count;
		vq_clear();
		mc.pend_len = 0;
		double media = !isnan(mc.last_vpts) ? mc.last_vpts : 0;
		int end = !more || quit_requested || (secs > 0 && media >= secs);
		if ((media >= (win + 1) * 10.0 || end) && win_frames)
		{
			double t = now_seconds(), ms = 1000 * (t - win_t) / win_frames;
			int a = win * 10, b = (int)media;
			if (b < a) b = a;
			printf("mediacenter: %d:%02d-%d:%02d  %4d immagini  %5.1f ms per immagine%s\n",
				a / 60, a % 60, b / 60, b % 60, win_frames, ms, budget > 0 && ms > budget ? "  oltre" : "");
			if (ms > worst) { worst = ms; worst_win = a; }
			if (budget > 0 && ms > budget) over++;
			wins++;
			win_t = t;
			win_frames = 0;
			while (media >= (win + 1) * 10.0) win++;
		}
		if (end) break;
	}
	double total = now_seconds() - t0;
	double media = !isnan(mc.last_vpts) ? mc.last_vpts : 0;
	if (frames)
	{
		printf("mediacenter: RISULTATO %s: %d immagini, %.0f s di video in %.0f s, media %.1f ms per immagine (%.0f%% del tempo reale)\n",
			parse ? "solo lettura" : "normale", frames, media, total, 1000 * total / frames, total > 0 ? 100 * media / total : 0);
		printf("mediacenter: finestra peggiore %d:%02d con %.1f ms per immagine; finestre oltre il tempo disponibile: %d su %d\n",
			worst_win / 60, worst_win % 60, worst, over, wins);
	}
	media_close();
	return 0;
}

#include "zctest.c"

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
						uint8_t rgb[3] = { 0, 0, 0 };
						if (fb_yuv)
						{
							// as the menu core converts it
							const uint8_t *q = fb_mem + (size_t)y * fb_stride + (x & ~1) * 2;
							int cy = (q[(x & 1) * 2] - 16) * 298, cu = q[1] - 128, cv = q[3] - 128;
							int c[3] = { cy + 409 * cv, cy - 100 * cu - 208 * cv, cy + 516 * cu };
							for (int k = 0; k < 3; k++)
							{
								int t = (c[k] + 128) >> 8;
								rgb[k] = t < 0 ? 0 : t > 255 ? 255 : t;
							}
						}
						else
						{
							rgb[0] = px[fb_bgr ? 2 : 0];
							rgb[1] = px[1];
							rgb[2] = px[fb_bgr ? 0 : 2];
						}
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
	const char *size = getenv("MC_TESTFB"); // another screen size, "1280x720"
	if (size) sscanf(size, "%dx%d", &fb_w, &fb_h);
	fb_stride = fb_w * 4;
	fb_len = (size_t)fb_stride * fb_h;
	fb_bgr = 1;
	fb_mem = (uint8_t *)calloc(1, fb_len);
	fb_fake = 1;
	test_start = now_seconds();
	input_hook = test_input;
	return fb_mem ? 0 : -1;
}

// Settings for the player, read at start as environment variables (a
// variable already set wins): /media/fat/linux/mediacenter.ini with lines
// like MC_AV_OFFSET=0.05; lines starting with # or ; are comments.
static void load_settings(void)
{
	FILE *f = fopen("/media/fat/linux/mediacenter.ini", "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f))
	{
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == ';' || *p == '[') continue;
		char *eq = strchr(p, '=');
		if (!eq) continue;
		*eq = 0;
		char *v = eq + 1, *end = v + strlen(v);
		while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
		for (char *k = eq; k > p && k[-1] == ' '; ) *--k = 0;
		if (*p) setenv(p, v, 0);
	}
	fclose(f);
}

// Which Main runs and what it did with the menu core, for the log: after a
// game another Main (Physical Disc's, the official one) may be in charge, and
// only MiSTer MediaCenter's Main loads the menu core with the CRT output.
static void log_main_info(void)
{
	DIR *d = opendir("/proc");
	if (d)
	{
		struct dirent *e;
		while ((e = readdir(d)))
		{
			if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
			char link[64], exe[256];
			snprintf(link, sizeof(link), "/proc/%s/exe", e->d_name);
			ssize_t n = readlink(link, exe, sizeof(exe) - 1);
			if (n <= 0) continue;
			exe[n] = 0;
			if (!strncmp(exe, "/media/fat/MiSTer", 17)) fprintf(stderr, "mediacenter: Main in uso: %s\n", exe);
		}
		closedir(d);
	}
	FILE *f = fopen("/tmp/mediacenter_main.log", "r");
	if (f)
	{
		char line[256], last[4][256] = { "", "", "", "" };
		int n = 0;
		while (fgets(line, sizeof(line), f)) snprintf(last[n++ % 4], sizeof(last[0]), "%s", line);
		fclose(f);
		for (int i = n > 4 ? n - 4 : 0; i < n; i++) fprintf(stderr, "mediacenter: Main: %s", last[i % 4]);
	}
	else fprintf(stderr, "mediacenter: Main: nessun registro del menu (Main non di MiSTer MediaCenter?)\n");
}

int main(int argc, char *argv[])
{
	// Both ARM cores. Started from the Scripts menu the player inherited the
	// Main's own setting, the second core only, which the Main itself keeps
	// busy: a DVD then dropped about one picture in five. Started for a disc
	// the Main already gives it both.
	{
		cpu_set_t cpus;
		CPU_ZERO(&cpus);
		long n = sysconf(_SC_NPROCESSORS_CONF);
		for (long i = 0; i < n && i < CPU_SETSIZE; i++) CPU_SET(i, &cpus);
		if (n > 0 && sched_setaffinity(0, sizeof(cpus), &cpus))
			fprintf(stderr, "mediacenter: CPU cores not set (%s)\n", strerror(errno));
		else if (n > 0) fprintf(stderr, "mediacenter: %ld CPU cores in use\n", n);
	}
	load_settings();
	crt_load();
	fb_present_yuv = yuv_draw;
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
	av_log_set_level(AV_LOG_ERROR);
	mc.vstream = mc.astream = -1;

	const char *arg = argc > 1 ? argv[1] : NULL;

	// Only --bench-parse may switch off the picture rebuilding in FFmpeg.
	unsetenv("MC_PARSEONLY");
	// Decoding speed test: decodes the whole file without output.
	if (arg && !strcmp(arg, "--bench") && argc > 2) return bench(argv[2]);
	// FPGA accelerator phase 0: decode-only timing, normal or stream-read only.
	if (arg && (!strcmp(arg, "--bench-dec") || !strcmp(arg, "--bench-parse")) && argc > 2)
		return bench_windows(argv[2], argc > 3 ? atof(argv[3]) : 0, !strcmp(arg, "--bench-parse"));
	// Zero-copy feasibility test (zctest.c).
	if (arg && !strcmp(arg, "--zctest") && argc > 2) return zctest(argv[2]);

	// The console back in text mode (mediacenter.sh, after a crash).
	if (arg && !strcmp(arg, "--console-text"))
	{
		int fd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (fd >= 0) { ioctl(fd, KDSETMODE, KD_TEXT); close(fd); }
		return 0;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_crash;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	console_take();

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
		// cdimage:<raw 2352-byte image>:<lba of track 2>[:<lba of track 3>...]
		// plays the image as the Super Video CD in the drive
		if (arg && !strncmp(arg, "cdimage:", 8))
		{
			char spec[1024];
			snprintf(spec, sizeof(spec), "%s", arg + 8);
			char *save = NULL, *file = strtok_r(spec, ":", &save);
			img_fd = file ? open(file, O_RDONLY | O_CLOEXEC) : -1;
			if (img_fd < 0) return 1;
			struct stat st;
			fstat(img_fd, &st);
			track_first = 1;
			track_last = 1;
			tracks[1].start = 0;
			tracks[1].data = 1;
			for (char *t; (t = strtok_r(NULL, ":", &save)) && track_last < MAX_TRACKS;)
			{
				tracks[++track_last].start = atoi(t);
				tracks[track_last].data = 1;
			}
			for (int t = 1; t <= track_last; t++)
				tracks[t].end = t < track_last ? tracks[t + 1].start : (int)(st.st_size / RAW_SECTOR);
			printf("test: %s\n", detect_vcd_kind() ? detect_vcd_kind() : "not a Video CD");
			int r = svcd_play();
			printf("test: ended with %d\n", r);
			return 0;
		}
		// "browser": the source list and its screens
		int r;
		if (arg && !strcmp(arg, "browser")) { show_splash(); r = run_browser(); }
		else r = play_file(arg, 0);
		printf("test: ended with %d\n", r);
		return 0;
	}

	if (fb_open_pages(0))
	{
		fprintf(stderr, "mediacenter: 32-bit /dev/fb0 not available\n");
		return 1;
	}
	osd_layout();
	input_open();
	log_main_info();
	crt_fb_changed();

	int rc = 0;
	if (arg) rc = play_file(arg, 0) == END_DONE ? 0 : 0;
	else
	{
		show_splash(); // started from the Scripts menu: no disc to play
		rc = run_browser();
	}

	fb_native(0, 0, 0); // the normal frame buffer back for the Main
	fb_clear();
	fb_close();
	if (input_fd >= 0) close(input_fd);
	return rc;
}
