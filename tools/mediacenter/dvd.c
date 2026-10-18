// DVD-Video player of mediacenter, included by mediacenter.c.
//
// libdvdnav runs the disc's navigation (menus, titles, chapters) and hands
// over the MPEG program stream one 2048-byte block at a time. The blocks are
// split into video, audio and subpicture packets here and decoded with the
// same FFmpeg decoders, output, clock and on-screen controls as the files.
//
// The disc is read and decoded by a reader thread on one core while the
// player loop shows the pictures and handles the controls on the other, so a
// slow read from the drive does not stop the picture.
//
// Encrypted (CSS) discs need libdvdcss: it is loaded from DVDCSS_PATH, where
// MiSTer_DVD's install_dvdcss puts it, before libdvdread looks for it.

#include <dlfcn.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes" // gcc_struct in libdvdread's headers
#include <dvdnav/dvdnav.h>
#pragma GCC diagnostic pop

#define DVDCSS_PATH "/media/fat/dvdcss/libdvdcss.so.2"
#define DVD_LANG "it"
#define DVD_MAX_SUBS 8
#define DVD_FF_SPEED 10.0
#define DVD_VQ 30          // pictures decoded ahead (1.2 s at 25 fps)
#define DVD_AHEAD 1.0      // seconds of audio and video decoded ahead

typedef struct
{
	AVSubtitle sub;
	double start, end; // end is INFINITY until the next subpicture
} dvd_sub_t;

typedef struct
{
	dvdnav_t *nav;
	uint8_t block[DVD_VIDEO_LB_LEN];

	AVCodecParserContext *vpar, *apar, *spar;
	AVCodecContext *sdec;
	int want_audio_id; // substream id to play (dvd_audio_id)
	int audio_id;     // substream id being decoded (0x80.., 0x88.., 0xA0.., 0xC0..)
	int audio_codec;  // AVCodecID of adec
	AVChannelLayout swr_layout;
	int swr_rate, swr_fmt;

	// Streams. libdvdnav's dvdnav_get_audio_logical_stream() and
	// dvdnav_get_spu_logical_stream() take a logical (menu) stream number and
	// return the physical (MPEG) one, despite their names.
	int user_audio;      // logical stream chosen by the user, -1 follows the disc
	int user_spu;        // -2 follows the disc, -1 off, else the logical stream
	int vm_audio_phys;   // physical audio stream chosen by the disc
	int vm_audio_logical;
	int vm_spu_phys;     // physical subpicture stream chosen by the disc (0x80: forced only)
	int spu_phys, spu_forced_only;

	uint32_t clut[16]; // 0x00YYCrCb from the disc
	uint32_t clut_rgb[16];
	int clut_valid;

	dvd_sub_t subs[DVD_MAX_SUBS];
	int num_subs;

	// Menu buttons
	int hl_on;
	dvdnav_highlight_area_t hl;

	// Program stream time (90 kHz) to playing position: pos = pts + offset,
	// applied in dvd_demux.
	int64_t offset;
	int64_t vobu_end;  // end of the last VOBU on the playing timeline
	int timeline_set;
	int hop;           // the next VOBU starts at the current position

	int still;         // 0 none, 1..254 seconds, 255 until a button is pressed
	double still_until;
	int wait;          // DVDNAV_WAIT: let the queued output play out
	int stopped;       // DVDNAV_STOP
	int halt;          // stopped by the user: read nothing

	// Reader thread and the requests the player loop leaves for it
	pthread_t thread;
	int thread_on;
	volatile int thread_run;
	int req_flush;     // drop everything queued (a jump made by the player)
	int req_seek;      // fast forward / rewind: jump to seek_to and decode one picture
	double seek_to;
	int seeking;       // a seek was made: hold after its first picture
	int ff_hold;       // the picture after a seek is decoded
	int scanning;      // fast forward / rewind on: seeks hold after a picture
	int req_audio;     // the user changed the audio stream

	AVFrame *last;     // the picture on screen, redrawn when the overlay changes
	int overlay_dirty;
	int title, chapters;
	double duration;
	char audio_label[64];
} dvd_t;

static dvd_t dvd;

// Guards what both threads use: the picture and audio queues, the
// subpictures, the button highlight, the clock and the flags above.
static pthread_mutex_t dvd_lock = PTHREAD_MUTEX_INITIALIZER;
#define DVD_LOCK() pthread_mutex_lock(&dvd_lock)
#define DVD_UNLOCK() pthread_mutex_unlock(&dvd_lock)

static void ycrcb_to_rgb(uint32_t v, int *r, int *g, int *b)
{
	int y = (v >> 16) & 0xff, cr = (v >> 8) & 0xff, cb = v & 0xff;
	double yy = 1.164 * (y - 16);
	int rr = (int)(yy + 1.596 * (cr - 128));
	int gg = (int)(yy - 0.813 * (cr - 128) - 0.391 * (cb - 128));
	int bb = (int)(yy + 2.018 * (cb - 128));
	*r = rr < 0 ? 0 : rr > 255 ? 255 : rr;
	*g = gg < 0 ? 0 : gg > 255 ? 255 : gg;
	*b = bb < 0 ? 0 : bb > 255 ? 255 : bb;
}

// Timestamps get the timeline offset as they are read, so that pictures
// still inside the decoder keep the timeline of their own cell.
static double dvd_pos(int64_t pts90)
{
	return pts90 / 90000.0;
}

// ---------------------------------------------------------------------------
// Decoders
// ---------------------------------------------------------------------------

static AVCodecContext *dvd_open_codec(enum AVCodecID id, AVDictionary **opts)
{
	const AVCodec *codec = avcodec_find_decoder(id);
	if (!codec) return NULL;
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) return NULL;
	ctx->pkt_timebase = (AVRational){ 1, 90000 };
	if (id == AV_CODEC_ID_MPEG2VIDEO)
	{
		ctx->thread_count = 2;
		ctx->thread_type = FF_THREAD_SLICE; // no frame-thread delay before stills
	}
	if (avcodec_open2(ctx, codec, opts) < 0) avcodec_free_context(&ctx);
	return ctx;
}

// The subpicture decoder takes the disc's colour table as an option, so it is
// opened again when the table changes.
static void dvd_open_spu_decoder(void)
{
	if (dvd.sdec) avcodec_free_context(&dvd.sdec);
	AVDictionary *opts = NULL;
	if (dvd.clut_valid)
	{
		char pal[16 * 8], *p = pal;
		for (int i = 0; i < 16; i++)
		{
			uint32_t c = dvd.clut_rgb[i];
			p += sprintf(p, "%s%06x", i ? "," : "", c & 0xffffff);
		}
		av_dict_set(&opts, "palette", pal, 0);
	}
	dvd.sdec = dvd_open_codec(AV_CODEC_ID_DVD_SUBTITLE, &opts);
	av_dict_free(&opts);
}

// Called with dvd_lock held.
static void dvd_clear_subs(void)
{
	for (int i = 0; i < dvd.num_subs; i++) avsubtitle_free(&dvd.subs[i].sub);
	dvd.num_subs = 0;
	dvd.overlay_dirty = 1;
}

static void dvd_set_audio_codec(int id)
{
	enum AVCodecID codec;
	if (id >= 0x80 && id <= 0x87) codec = AV_CODEC_ID_AC3;
	else if (id >= 0x88 && id <= 0x8f) codec = AV_CODEC_ID_DTS;
	else if (id >= 0xa0 && id <= 0xa7) codec = AV_CODEC_ID_PCM_DVD;
	else codec = AV_CODEC_ID_MP2;

	int same = mc.adec && dvd.audio_codec == (int)codec;
	dvd.audio_id = id;
	if (same)
	{
		// another stream of the same format: start clean
		printf("mediacenter: DVD audio stream 0x%02x %s\n", id, avcodec_get_name(codec));
		avcodec_flush_buffers(mc.adec);
		if (dvd.apar) { av_parser_close(dvd.apar); dvd.apar = av_parser_init(codec); }
		return;
	}
	if (mc.adec) avcodec_free_context(&mc.adec);
	if (dvd.apar) av_parser_close(dvd.apar);
	dvd.apar = NULL;
	swr_free(&mc.swr);
	mc.adec = dvd_open_codec(codec, NULL);
	dvd.audio_codec = codec;
	if (codec != AV_CODEC_ID_PCM_DVD) dvd.apar = av_parser_init(codec);
	printf("mediacenter: DVD audio stream 0x%02x %s\n", id, avcodec_get_name(codec));
}

static void dvd_audio_frame(AVFrame *f)
{
	// The output converter follows the decoded format, which can change
	// between titles.
	if (!mc.swr || f->sample_rate != dvd.swr_rate || f->format != dvd.swr_fmt ||
		av_channel_layout_compare(&f->ch_layout, &dvd.swr_layout))
	{
		swr_free(&mc.swr);
		AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
		if (swr_alloc_set_opts2(&mc.swr, &out, AV_SAMPLE_FMT_S16, MC_AUDIO_RATE,
			&f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0 || swr_init(mc.swr) < 0)
		{
			swr_free(&mc.swr);
			return;
		}
		av_channel_layout_uninit(&dvd.swr_layout);
		av_channel_layout_copy(&dvd.swr_layout, &f->ch_layout);
		dvd.swr_rate = f->sample_rate;
		dvd.swr_fmt = f->format;
		printf("mediacenter: DVD sound %d Hz, %d channels\n", f->sample_rate, f->ch_layout.nb_channels);
	}
	double pts = f->best_effort_timestamp == AV_NOPTS_VALUE ? NAN : dvd_pos(f->best_effort_timestamp);
	DVD_LOCK();
	// keep continuity across small timestamp jitter
	if (!isnan(pts) && mc.pend_len && fabs(pts - mc.audio_end) < 0.08) pts = NAN;
	push_audio_at(f, pts);
	DVD_UNLOCK();
}

static void dvd_video_frame(AVFrame *f)
{
	double pts = f->best_effort_timestamp == AV_NOPTS_VALUE ? NAN : dvd_pos(f->best_effort_timestamp);
	AVRational fr = mc.vdec->framerate;
	double step = (fr.num > 0 && fr.den > 0) ? 1.0 / av_q2d(fr) : 0.04;
	DVD_LOCK();
	queue_video_at(f, pts, step);
	if (dvd.seeking) { dvd.seeking = 0; dvd.ff_hold = dvd.scanning; }
	DVD_UNLOCK();
}

static void dvd_receive(AVCodecContext *ctx, int video)
{
	while (avcodec_receive_frame(ctx, mc.frame) == 0)
	{
		if (video) dvd_video_frame(mc.frame);
		else dvd_audio_frame(mc.frame);
		av_frame_unref(mc.frame);
	}
}

static void dvd_send(AVCodecContext *ctx, const uint8_t *data, int size, int64_t pts, int video)
{
	AVPacket *pkt = mc.pkt;
	av_packet_unref(pkt);
	pkt->data = (uint8_t *)data;
	pkt->size = size;
	pkt->pts = pts;
	pkt->dts = AV_NOPTS_VALUE;
	if (avcodec_send_packet(ctx, data ? pkt : NULL) == AVERROR(EAGAIN))
	{
		dvd_receive(ctx, video);
		avcodec_send_packet(ctx, data ? pkt : NULL);
	}
	dvd_receive(ctx, video);
	pkt->data = NULL;
	pkt->size = 0;
}

static void dvd_spu_packet(const uint8_t *data, int size, int64_t pts)
{
	if (!dvd.sdec) return;
	AVPacket *pkt = mc.pkt;
	av_packet_unref(pkt);
	pkt->data = (uint8_t *)data;
	pkt->size = size;
	pkt->pts = pts;
	AVSubtitle sub;
	int got = 0;
	if (avcodec_decode_subtitle2(dvd.sdec, &sub, &got, pkt) < 0 || !got)
	{
		pkt->data = NULL;
		pkt->size = 0;
		return;
	}
	pkt->data = NULL;
	pkt->size = 0;

	DVD_LOCK();
	double base = pts == AV_NOPTS_VALUE ? mc.clock : dvd_pos(pts);
	if (dvd.num_subs == DVD_MAX_SUBS)
	{
		avsubtitle_free(&dvd.subs[0].sub);
		memmove(dvd.subs, dvd.subs + 1, sizeof(dvd.subs[0]) * (DVD_MAX_SUBS - 1));
		dvd.num_subs--;
	}
	dvd_sub_t *s = &dvd.subs[dvd.num_subs++];
	s->sub = sub;
	s->start = base + sub.start_display_time / 1000.0;
	dvd.overlay_dirty = 1;
	s->end = (sub.end_display_time > sub.start_display_time && sub.end_display_time < 0x7fffffff)
		? base + sub.end_display_time / 1000.0 : INFINITY;
	DVD_UNLOCK();
}

// Feeds a parser and the decoder behind it.
static void dvd_parse(AVCodecParserContext *par, AVCodecContext *ctx, const uint8_t *data, int size, int64_t pts, int kind)
{
	while (size > 0 || !data)
	{
		uint8_t *out = NULL;
		int out_size = 0;
		int used = av_parser_parse2(par, ctx, &out, &out_size, data, data ? size : 0, pts, AV_NOPTS_VALUE, 0);
		pts = AV_NOPTS_VALUE;
		if (out_size)
		{
			if (kind == 2) dvd_spu_packet(out, out_size, par->pts);
			else dvd_send(ctx, out, out_size, par->pts, kind == 0);
		}
		if (!data) break;
		data += used;
		size -= used;
		if (!used && !out_size) break;
	}
}

// Decodes everything still inside the video decoder (before a still picture),
// then makes it ready for more.
static void dvd_drain_video(void)
{
	if (!mc.vdec) return;
	if (dvd.vpar) dvd_parse(dvd.vpar, mc.vdec, NULL, 0, AV_NOPTS_VALUE, 0);
	dvd_send(mc.vdec, NULL, 0, AV_NOPTS_VALUE, 1);
	avcodec_flush_buffers(mc.vdec);
	av_parser_close(dvd.vpar);
	dvd.vpar = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
}

// Drops everything queued or buffered (a jump to another place on the disc).
// Reader thread only: it owns the decoders.
static void dvd_flush(void)
{
	if (mc.vdec) avcodec_flush_buffers(mc.vdec);
	if (mc.adec) avcodec_flush_buffers(mc.adec);
	if (dvd.vpar) av_parser_close(dvd.vpar);
	dvd.vpar = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
	if (dvd.apar) { av_parser_close(dvd.apar); dvd.apar = av_parser_init(dvd.audio_codec); }
	if (dvd.spar) av_parser_close(dvd.spar);
	dvd.spar = av_parser_init(AV_CODEC_ID_DVD_SUBTITLE);
	DVD_LOCK();
	vq_clear();
	mc.pend_len = 0;
	mc.audio_end = mc.clock;
	mc.last_vpts = NAN;
	dvd_clear_subs();
	DVD_UNLOCK();
	audio_flush();
}

// ---------------------------------------------------------------------------
// Program stream
// ---------------------------------------------------------------------------

// The logical audio stream playing: the user's choice, else the disc's.
static int dvd_audio_logical(void)
{
	if (dvd.user_audio >= 0) return dvd.user_audio;
	// the disc's register can hold a stream that does not exist (the VM then
	// plays the first one): find the logical stream from the physical one
	for (int l = 0; l < 8; l++)
		if (dvdnav_get_audio_logical_stream(dvd.nav, l) == dvd.vm_audio_phys) return l;
	return dvd.vm_audio_logical;
}

// Its substream id in the program stream, from its format.
static int dvd_audio_id(void)
{
	int logical = dvd_audio_logical();
	int phys = dvd.user_audio >= 0 ? dvdnav_get_audio_logical_stream(dvd.nav, dvd.user_audio) : dvd.vm_audio_phys;
	if (phys < 0) phys = dvd.vm_audio_phys;
	if (phys < 0) return -1;
	switch (logical >= 0 ? dvdnav_audio_stream_format(dvd.nav, logical) : DVD_AUDIO_FORMAT_AC3)
	{
	case DVD_AUDIO_FORMAT_MPEG:
	case DVD_AUDIO_FORMAT_MPEG2_EXT: return 0xc0 + (phys & 7);
	case DVD_AUDIO_FORMAT_LPCM: return 0xa0 + (phys & 7);
	case DVD_AUDIO_FORMAT_DTS: return 0x88 + (phys & 7);
	default: return 0x80 + (phys & 7);
	}
}

// Number of logical audio streams of the title (0 outside a title).
static int dvd_count_audio(void)
{
	if (!dvdnav_is_domain_vts(dvd.nav)) return 0;
	int n = 0;
	for (int l = 0; l < 8; l++)
		if (dvdnav_get_audio_logical_stream(dvd.nav, l) >= 0) n = l + 1;
	return n;
}

static int dvd_count_spu(void)
{
	if (!dvdnav_is_domain_vts(dvd.nav)) return 0;
	int n = 0;
	for (int l = 0; l < 32; l++)
		if (dvdnav_get_spu_logical_stream(dvd.nav, l) >= 0) n = l + 1;
	return n;
}

// The logical subpicture stream shown as subtitles, -1 for none.
static int dvd_spu_logical(void)
{
	if (dvd.spu_phys < 0 || dvd.spu_forced_only) return -1;
	if (dvd.user_spu >= 0) return dvd.user_spu;
	for (int l = 0; l < 32; l++)
		if (dvdnav_get_spu_logical_stream(dvd.nav, l) == dvd.spu_phys) return l;
	return -1;
}

// Picks the subpicture stream to decode. Called with dvd_lock held.
static void dvd_update_spu(void)
{
	int phys = dvd.vm_spu_phys;
	int forced = phys >= 0 && (phys & 0x80);
	if (dvdnav_is_domain_vts(dvd.nav) && dvd.user_spu != -2)
	{
		forced = 0;
		phys = dvd.user_spu >= 0 ? dvdnav_get_spu_logical_stream(dvd.nav, dvd.user_spu) : -1;
		// subtitles off: forced subtitles (translated signs) still show
		if (phys < 0 && dvd.vm_spu_phys >= 0) { phys = dvd.vm_spu_phys; forced = 1; }
	}
	phys = phys < 0 ? -1 : (phys & 0x1f);
	if (phys != dvd.spu_phys) dvd_clear_subs();
	dvd.spu_phys = phys;
	dvd.spu_forced_only = forced;
	dvd.overlay_dirty = 1;
}

static int64_t pes_pts(const uint8_t *p)
{
	return ((int64_t)(p[0] & 0x0e) << 29) | (p[1] << 22) | ((p[2] & 0xfe) << 14) | (p[3] << 7) | (p[4] >> 1);
}

static void dvd_demux(const uint8_t *buf, int len)
{
	int i = 0;
	if (len >= 14 && !memcmp(buf, "\x00\x00\x01\xba", 4))
	{
		i = (buf[4] & 0xc0) == 0x40 ? 14 + (buf[13] & 7) : 12; // MPEG-2 or MPEG-1 pack header
	}

	while (i + 6 <= len)
	{
		if (buf[i] || buf[i + 1] || buf[i + 2] != 1) break;
		int id = buf[i + 3];
		int plen = (buf[i + 4] << 8) | buf[i + 5];
		const uint8_t *p = buf + i + 6;
		int end = i + 6 + plen;
		if (end > len) end = len;
		i += 6 + plen;
		if (id == 0xbb || id == 0xbe || id == 0xbf || id < 0xbd) continue; // system header, padding, nav

		// PES header (MPEG-2)
		int remain = (int)(buf + end - p);
		if (remain < 3) continue;
		int64_t pts = AV_NOPTS_VALUE;
		int hlen = p[2];
		if ((p[1] & 0x80) && hlen >= 5 && remain >= 8) pts = pes_pts(p + 3) + dvd.offset;
		p += 3 + hlen;
		remain -= 3 + hlen;
		if (remain <= 0) continue;

		if (id >= 0xe0 && id <= 0xef)
		{
			if (mc.vdec && dvd.vpar) dvd_parse(dvd.vpar, mc.vdec, p, remain, pts, 0);
			continue;
		}

		int sub = id;
		if (id == 0xbd)
		{
			sub = p[0];
			if (sub >= 0x20 && sub <= 0x3f)
			{
				if ((sub & 0x1f) == dvd.spu_phys && dvd.sdec && dvd.spar)
					dvd_parse(dvd.spar, dvd.sdec, p + 1, remain - 1, pts, 2);
				continue;
			}
			if (remain < 4) continue;
			// AC-3, DTS, LPCM: substream id, frame count, first access unit
			p += 4;
			remain -= 4;
		}
		else if (id < 0xc0 || id > 0xc7) continue;

		if (sub != dvd.want_audio_id) continue;
		if (sub != dvd.audio_id || !mc.adec) dvd_set_audio_codec(sub);
		if (!mc.adec) continue;
		if (dvd.apar) dvd_parse(dvd.apar, mc.adec, p, remain, pts, 1);
		else dvd_send(mc.adec, p, remain, pts, 0);
	}
}

// ---------------------------------------------------------------------------
// Navigation events
// ---------------------------------------------------------------------------

static void dvd_update_highlight(void)
{
	pci_t *pci = dvdnav_get_current_nav_pci(dvd.nav);
	int32_t button = 0;
	int on = 0;
	if (pci && pci->hli.hl_gi.btn_ns && dvdnav_get_current_highlight(dvd.nav, &button) == DVDNAV_STATUS_OK && button > 0)
	{
		dvdnav_highlight_area_t hl;
		if (dvdnav_get_highlight_area(pci, button, 0, &hl) == DVDNAV_STATUS_OK)
		{
			on = 1;
			DVD_LOCK();
			if (memcmp(&hl, &dvd.hl, sizeof(hl))) dvd.overlay_dirty = 1;
			dvd.hl = hl;
			DVD_UNLOCK();
		}
	}
	if (on != dvd.hl_on) dvd.overlay_dirty = 1;
	dvd.hl_on = on;
}

static void dvd_title_info(void)
{
	int32_t title = 0, part = 0;
	if (dvdnav_current_title_info(dvd.nav, &title, &part) != DVDNAV_STATUS_OK || title <= 0)
	{
		dvd.title = 0;
		dvd.chapters = 0;
		dvd.duration = 0;
		return;
	}
	if (title == dvd.title) return;
	if (dvd.title > 0)
	{
		// another title has its own streams: follow the disc again
		dvd.user_audio = -1;
		if (dvd.user_spu >= 0) dvd.user_spu = -2;
	}
	dvd.title = title;
	int32_t parts = 0;
	dvdnav_get_number_of_parts(dvd.nav, title, &parts);
	dvd.chapters = parts;
	uint64_t *times = NULL, duration = 0;
	if (dvdnav_describe_title_chapters(dvd.nav, title, &times, &duration)) dvd.duration = duration / 90000.0;
	free(times);
}

// Handles one navigation step. Returns 0 when the disc stopped.
static int dvd_step(void)
{
	int event = 0, len = 0;
	uint8_t *buf = dvd.block;
	if (dvdnav_get_next_block(dvd.nav, buf, &event, &len) != DVDNAV_STATUS_OK)
	{
		fprintf(stderr, "mediacenter: DVD read error: %s\n", dvdnav_err_to_string(dvd.nav));
		dvd.stopped = 1;
		return 0;
	}

	static int debug = -1;
	if (debug < 0) debug = getenv("MC_DEBUG") != NULL;
	if (debug && event != DVDNAV_BLOCK_OK && event != DVDNAV_NAV_PACKET)
		fprintf(stderr, "dvd: %.2f event %d still %d wait %d vq %d pend %d\n", mc.clock, event, dvd.still, dvd.wait, mc.vq_count, mc.pend_len);

	switch (event)
	{
	case DVDNAV_BLOCK_OK:
		dvd_demux(buf, len);
		break;

	case DVDNAV_NAV_PACKET:
	{
		pci_t *pci = dvdnav_get_current_nav_pci(dvd.nav);
		int64_t s = pci->pci_gi.vobu_s_ptm, e = pci->pci_gi.vobu_e_ptm;
		DVD_LOCK();
		int64_t now = (int64_t)(mc.clock * 90000);
		DVD_UNLOCK();
		if (!dvd.timeline_set || dvd.hop)
		{
			// a jump: what follows plays from now
			dvd.offset = now + 9000 - s;
			dvd.hop = 0;
			dvd.timeline_set = 1;
		}
		else if (llabs(s + dvd.offset - dvd.vobu_end) > 3000)
		{
			// a new cell with its own timestamps: continue from the previous one
			dvd.offset = dvd.vobu_end - s;
			if (dvd.offset + s < now) dvd.offset = now - s;
		}
		dvd.vobu_end = e + dvd.offset;
		dvd.want_audio_id = dvd_audio_id();
		dvd_update_highlight();
		break;
	}

	case DVDNAV_STILL_FRAME:
	{
		dvdnav_still_event_t *ev = (dvdnav_still_event_t *)buf;
		if (!dvd.still)
		{
			dvd_drain_video();
			DVD_LOCK();
			dvd.still = ev->length ? ev->length : 1;
			dvd.still_until = NAN; // starts when the last picture is shown
			DVD_UNLOCK();
		}
		return 1;
	}

	case DVDNAV_WAIT:
		DVD_LOCK();
		dvd.wait = 1;
		DVD_UNLOCK();
		return 1;

	case DVDNAV_SPU_CLUT_CHANGE:
		DVD_LOCK();
		memcpy(dvd.clut, buf, sizeof(dvd.clut));
		for (int i = 0; i < 16; i++)
		{
			int r, g, b;
			ycrcb_to_rgb(dvd.clut[i], &r, &g, &b);
			dvd.clut_rgb[i] = (uint32_t)(r << 16 | g << 8 | b);
		}
		dvd.clut_valid = 1;
		DVD_UNLOCK();
		dvd_open_spu_decoder();
		break;

	case DVDNAV_SPU_STREAM_CHANGE:
	{
		dvdnav_spu_stream_change_event_t *ev = (dvdnav_spu_stream_change_event_t *)buf;
		DVD_LOCK();
		dvd.vm_spu_phys = ev->physical_wide;
		dvd_update_spu();
		DVD_UNLOCK();
		break;
	}

	case DVDNAV_AUDIO_STREAM_CHANGE:
	{
		dvdnav_audio_stream_change_event_t *ev = (dvdnav_audio_stream_change_event_t *)buf;
		dvd.vm_audio_phys = ev->physical;
		dvd.vm_audio_logical = ev->logical;
		dvd.want_audio_id = dvd_audio_id();
		break;
	}

	case DVDNAV_HIGHLIGHT:
		dvd_update_highlight();
		break;

	case DVDNAV_VTS_CHANGE:
	{
		int aspect = dvdnav_get_video_aspect(dvd.nav);
		fb_aspect = aspect == 3 ? 16.0 / 9.0 : 4.0 / 3.0;
		dvd.title = -1;
		break;
	}

	case DVDNAV_CELL_CHANGE:
		dvd_title_info();
		DVD_LOCK();
		dvd_update_spu();
		DVD_UNLOCK();
		break;

	case DVDNAV_HOP_CHANNEL:
		dvd_flush();
		dvd.hop = 1;
		break;

	case DVDNAV_STOP:
		DVD_LOCK();
		dvd.stopped = 1;
		DVD_UNLOCK();
		return 0;

	default:
		break;
	}
	return 1;
}

// ---------------------------------------------------------------------------
// Picture
// ---------------------------------------------------------------------------

static void blend(uint8_t *px, int r, int g, int b, int a)
{
	// frame_rgb is BGRA or RGBA like the framebuffer
	int c0 = fb_bgr ? b : r, c2 = fb_bgr ? r : b;
	px[0] = (uint8_t)((px[0] * (255 - a) + c0 * a) / 255);
	px[1] = (uint8_t)((px[1] * (255 - a) + g * a) / 255);
	px[2] = (uint8_t)((px[2] * (255 - a) + c2 * a) / 255);
}

static const dvd_sub_t *dvd_current_sub(void)
{
	const dvd_sub_t *cur = NULL;
	for (int i = 0; i < dvd.num_subs; i++)
	{
		if (dvd.subs[i].start <= mc.clock + 0.02 || dvd.still) cur = &dvd.subs[i];
	}
	if (cur && cur->end <= mc.clock && !dvd.still) cur = NULL;
	if (cur && dvd.spu_forced_only && cur->sub.num_rects && !(cur->sub.rects[0]->flags & AV_SUBTITLE_FLAG_FORCED)) cur = NULL;
	return cur;
}

// Drops the subpictures replaced by a newer one already showing.
// Called with dvd_lock held.
static void dvd_expire_subs(void)
{
	int keep = 0;
	for (int i = 1; i < dvd.num_subs; i++)
		if (dvd.subs[i].start <= mc.clock) keep = i;
	if (!dvd.still && keep)
	{
		for (int i = 0; i < keep; i++) avsubtitle_free(&dvd.subs[i].sub);
		memmove(dvd.subs, dvd.subs + keep, sizeof(dvd.subs[0]) * (dvd.num_subs - keep));
		dvd.num_subs -= keep;
	}
}

// Draws the subpicture (subtitles, menu graphics) and the selected button.
// On a progressive picture (or one whose fields are kept) each line of the
// subpicture is blended with the one above, as the deinterlacing does for an
// interlaced picture: the white letters then have no edge one line tall,
// which on an interlaced CRT output is in one field only and flickers.
static void dvd_overlay_locked(void)
{
	const dvd_sub_t *cur = dvd_current_sub();
	if (!cur) return;
	static uint16_t *buf;  // premultiplied r, g, b and alpha of a rectangle
	static size_t buf_n;
	for (unsigned n = 0; n < cur->sub.num_rects; n++)
	{
		AVSubtitleRect *r = cur->sub.rects[n];
		if (r->type != SUBTITLE_BITMAP || !r->data[0] || r->w <= 0 || r->h <= 0) continue;
		const uint32_t *pal = (const uint32_t *)r->data[1];
		int rows = r->h + 1; // one more line below for the blend
		size_t need = (size_t)r->w * rows * 4;
		if (need > buf_n)
		{
			free(buf);
			buf = (uint16_t *)malloc(need * sizeof(uint16_t));
			buf_n = buf ? need : 0;
			if (!buf) return;
		}
		memset(buf, 0, need * sizeof(uint16_t));
		for (int y = 0; y < r->h; y++)
		{
			int sy = r->y + y;
			const uint8_t *row = r->data[0] + (size_t)y * r->linesize[0];
			int in_row = dvd.hl_on && sy >= (int)dvd.hl.sy && sy <= (int)dvd.hl.ey;
			uint16_t *o = buf + (size_t)y * r->w * 4;
			for (int x = 0; x < r->w; x++, o += 4)
			{
				int sx = r->x + x;
				int code = row[x] & 3;
				int cr, cg, cb, a;
				if (in_row && sx >= (int)dvd.hl.sx && sx <= (int)dvd.hl.ex)
				{
					// button colours: 4 colour indices and 4 contrasts
					int ci = (dvd.hl.palette >> (16 + 4 * code)) & 0xf;
					a = ((dvd.hl.palette >> (4 * code)) & 0xf) * 17;
					uint32_t c = dvd.clut_rgb[ci];
					cr = (c >> 16) & 0xff; cg = (c >> 8) & 0xff; cb = c & 0xff;
				}
				else
				{
					uint32_t c = pal ? pal[row[x]] : 0;
					a = (c >> 24) & 0xff;
					cr = (c >> 16) & 0xff; cg = (c >> 8) & 0xff; cb = c & 0xff;
				}
				o[0] = (uint16_t)(cr * a); o[1] = (uint16_t)(cg * a); o[2] = (uint16_t)(cb * a); o[3] = (uint16_t)a;
			}
		}
		int out_rows = r->h;
		// while the controls are on screen, a subtitle under them is moved
		// up above them, so the language just chosen can be seen at once
		int dy = 0;
		if (ovl_shown && ovl_a && !dvd.hl_on && src_h == fb_h && r->y + r->h + 1 > ovl_y && r->h < ovl_y)
			dy = ovl_y - (r->y + r->h + 1);
		if (dy < -r->y) dy = -r->y;
		if (overlay_soften)
		{
			// from the bottom up: line y becomes the mean of lines y - 1 and y
			for (int y = rows - 1; y >= 0; y--)
			{
				uint16_t *o = buf + (size_t)y * r->w * 4;
				const uint16_t *up = y ? o - (size_t)r->w * 4 : NULL;
				for (int i = 0; i < r->w * 4; i++) o[i] = (uint16_t)((o[i] + (up ? up[i] : 0)) / 2);
			}
			out_rows = rows;
		}
		for (int y = 0; y < out_rows; y++)
		{
			int sy = r->y + y + dy;
			if (sy < 0 || sy >= src_h) continue;
			uint8_t *dst = frame_rgb + (size_t)sy * src_w * 4;
			const uint16_t *o = buf + (size_t)y * r->w * 4;
			for (int x = 0; x < r->w; x++, o += 4)
			{
				int sx = r->x + x;
				int a = o[3];
				if (!a || sx < 0 || sx >= src_w) continue;
				blend(dst + sx * 4, o[0] / a, o[1] / a, o[2] / a, a);
			}
		}
	}
}

static void dvd_overlay(void)
{
	DVD_LOCK();
	dvd_overlay_locked();
	DVD_UNLOCK();
}

static void dvd_show(AVFrame *f)
{
	if (f != dvd.last)
	{
		av_frame_free(&dvd.last);
		dvd.last = f;
	}
	frame_overlay = dvd_overlay;
	draw_video_frame(f);
	frame_overlay = NULL;
	dvd.overlay_dirty = 0;
}

// ---------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------

static legend_t legend_dvd_play[] =
{
	{ "SELECT", "Pausa" },
	{ "B", "Stop" },
	{ "\x10 \x11", "Capitolo" },
	{ "L R", "\x10\x10 \x11\x11" },
	{ "\x12 \x13", "Audio" },
	{ "A", "Sottotitoli" },
	{ "Y", "Menu" },
	{ "X", "Esci" },
};

static legend_t legend_dvd_menu[] =
{
	{ "\x12 \x13 \x10 \x11", "Scelta" },
	{ "A", "Conferma" },
	{ "Y", "Torna al film" },
	{ "B", "Stop" },
	{ "X", "Esci" },
};

static const char *lang_name(uint16_t code, char *buf)
{
	if (code == 0xffff || !code) return "";
	buf[0] = ' ';
	buf[1] = (char)(code >> 8);
	buf[2] = (char)(code & 0xff);
	buf[3] = 0;
	return buf;
}

static void dvd_make_labels(void)
{
	static const char *const fmt[] = { "AC3", "?", "MPEG", "MPEG", "LPCM", "?", "DTS", "SDDS" };
	char lang[4];
	char a[32] = "", s[32];
	int na = dvd_count_audio();
	int logical = dvd_audio_logical();
	if (na > 0 && logical >= 0)
	{
		uint16_t f = dvdnav_audio_stream_format(dvd.nav, logical);
		snprintf(a, sizeof(a), "Audio %d/%d%s %s", logical + 1, na,
			lang_name(dvdnav_audio_stream_to_lang(dvd.nav, logical), lang), f < 8 ? fmt[f] : "");
	}
	int ns = dvd_count_spu();
	if (!ns) s[0] = 0;
	else
	{
		int l = dvd_spu_logical();
		if (l < 0) snprintf(s, sizeof(s), "  Sott. no");
		else snprintf(s, sizeof(s), "  Sott. %d/%d%s", l + 1, ns, lang_name(dvdnav_spu_stream_to_lang(dvd.nav, l), lang));
	}
	snprintf(dvd.audio_label, sizeof(dvd.audio_label), "%s%s", a, s);
}

static void dvd_stop_thread(void)
{
	if (!dvd.thread_on) return;
	dvd.thread_run = 0;
	pthread_join(dvd.thread, NULL);
	dvd.thread_on = 0;
}

static void dvd_close(void)
{
	dvd_stop_thread();
	DVD_LOCK();
	dvd_clear_subs();
	DVD_UNLOCK();
	av_frame_free(&dvd.last);
	if (dvd.vpar) av_parser_close(dvd.vpar);
	if (dvd.apar) av_parser_close(dvd.apar);
	if (dvd.spar) av_parser_close(dvd.spar);
	if (dvd.sdec) avcodec_free_context(&dvd.sdec);
	av_channel_layout_uninit(&dvd.swr_layout);
	if (dvd.nav) dvdnav_close(dvd.nav);
	memset(&dvd, 0, sizeof(dvd));
	media_close();
}

static int dvd_open(const char *device)
{
	media_close();
	memset(&dvd, 0, sizeof(dvd));

	// Loaded globally so that libdvdread's own dlopen("libdvdcss.so.2") finds it.
	if (!dlopen(DVDCSS_PATH, RTLD_NOW | RTLD_GLOBAL))
		fprintf(stderr, "mediacenter: %s not loaded (%s): only unencrypted DVDs will play\n", DVDCSS_PATH, dlerror());

	if (dvdnav_open(&dvd.nav, device) != DVDNAV_STATUS_OK)
	{
		fprintf(stderr, "mediacenter: %s is not a DVD-Video\n", device);
		dvd.nav = NULL;
		return -1;
	}
	dvdnav_set_readahead_flag(dvd.nav, 1);
	dvdnav_menu_language_select(dvd.nav, (char *)DVD_LANG);
	dvdnav_audio_language_select(dvd.nav, (char *)DVD_LANG);
	dvdnav_spu_language_select(dvd.nav, (char *)DVD_LANG);
	dvdnav_set_PGC_positioning_flag(dvd.nav, 1);

	mc.vdec = dvd_open_codec(AV_CODEC_ID_MPEG2VIDEO, NULL);
	dvd.vpar = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
	dvd.spar = av_parser_init(AV_CODEC_ID_DVD_SUBTITLE);
	dvd_open_spu_decoder();
	mc.pkt = av_packet_alloc();
	mc.frame = av_frame_alloc();
	mc.pend_cap = MC_AUDIO_RATE * MC_PENDING_SECONDS;
	mc.pend = (int16_t *)malloc((size_t)mc.pend_cap * 4);
	if (!mc.vdec || !dvd.vpar || !mc.pkt || !mc.frame || !mc.pend)
	{
		dvd_close();
		return -1;
	}
	mc.vstream = 0;
	mc.astream = 1;
	mc.last_vpts = NAN;
	dvd.user_audio = -1;
	dvd.user_spu = -2;
	dvd.vm_audio_phys = 0;
	dvd.vm_audio_logical = 0;
	dvd.vm_spu_phys = -1;
	dvd.spu_phys = -1;
	dvd.title = -1;
	fb_aspect = 4.0 / 3.0;

	const char *title = NULL;
	dvdnav_get_title_string(dvd.nav, &title);
	snprintf(mc.title, sizeof(mc.title), "%s", title && *title ? title : "DVD");
	printf("mediacenter: DVD %s\n", mc.title);
	return 0;
}

// Whether the reader should decode more. Called with dvd_lock held.
static int dvd_need_more(void)
{
	if (dvd.halt || dvd.still || dvd.wait || dvd.stopped || dvd.ff_hold) return 0;
	if (mc.vq_count >= DVD_VQ) return 0;
	if (mc.pend_len >= mc.pend_cap - MC_AUDIO_RATE / 2) return 0;
	if (!mc.vq_count || mc.vq[mc.vq_count - 1].pts <= mc.clock + DVD_AHEAD) return 1;
	return mc.audio_end < mc.clock + DVD_AHEAD;
}

// Reader thread: reads the disc and decodes ahead of the clock.
static void *dvd_reader(void *arg)
{
	(void)arg;
	while (dvd.thread_run)
	{
		DVD_LOCK();
		int flush = dvd.req_flush, seek = dvd.req_seek, audio = dvd.req_audio;
		double seek_to = dvd.seek_to;
		dvd.req_flush = dvd.req_seek = dvd.req_audio = 0;
		DVD_UNLOCK();

		if (flush || seek)
		{
			if (seek) dvdnav_time_search(dvd.nav, (uint64_t)(seek_to * 90000));
			dvd_flush();
			DVD_LOCK();
			dvd.hop = 1;
			dvd.ff_hold = 0;
			// a seek still pending when the scan ended must not hold playback
			dvd.seeking = seek && dvd.scanning;
			DVD_UNLOCK();
		}
		if (audio)
		{
			dvd.want_audio_id = dvd_audio_id();
			if (mc.adec) avcodec_flush_buffers(mc.adec);
			if (dvd.apar) { av_parser_close(dvd.apar); dvd.apar = av_parser_init(dvd.audio_codec); }
			DVD_LOCK();
			mc.pend_len = 0;
			mc.audio_end = mc.clock;
			sent_end = NAN; // the new track starts its own timing
			av_pause = 3.0;
			DVD_UNLOCK();
			audio_flush();
		}

		DVD_LOCK();
		int more = dvd_need_more();
		// a stream too heavy for the CPU: skip pictures nothing refers to
		int behind = mc.vq_count == 0 && !isnan(mc.last_vpts) && mc.last_vpts < mc.clock - 0.3;
		DVD_UNLOCK();
		if (!more)
		{
			usleep(4000);
			continue;
		}
		mc.vdec->skip_frame = behind ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
		if (!dvd_step()) usleep(4000);
	}
	return NULL;
}

static int dvd_in_menu(void)
{
	pci_t *pci = dvdnav_get_current_nav_pci(dvd.nav);
	return pci && pci->hli.hl_gi.btn_ns > 0 && dvd.hl_on;
}

// After a jump made with the controls: drop what is queued at once.
static void dvd_jumped(void)
{
	DVD_LOCK();
	dvd.req_flush = 1;
	dvd.still = dvd.wait = dvd.stopped = dvd.halt = 0;
	DVD_UNLOCK();
}

// Fast forward / rewind ends: play on from the picture on screen. A seek the
// reader has not made yet is dropped, one it is making no longer holds.
static void dvd_scan_end(void)
{
	DVD_LOCK();
	dvd.scanning = 0;
	dvd.req_seek = 0;
	dvd.seeking = 0;
	dvd.ff_hold = 0;
	dvd.hop = 1;
	DVD_UNLOCK();
}

// Plays the DVD in device. Returns END_QUIT when the application must close.
static int dvd_play(const char *device)
{
	if (dvd_open(device)) return END_DONE;

	player_t p;
	memset(&p, 0, sizeof(p));
	p.state = ST_PLAY;
	p.osd_until = now_seconds() + 3.0;
	have_frame = 0;
	exit_question = "Uscire dal DVD?";
	if (audio_open(MC_AUDIO_RATE)) fprintf(stderr, "mediacenter: playing without audio\n");
	fb_clear();
	native_fb = !getenv("MC_ARMSCALE");

	frames_shown = frames_dropped = 0;
	dvd.thread_run = 1;
	if (pthread_create(&dvd.thread, NULL, dvd_reader, NULL))
	{
		audio_close();
		dvd_close();
		return END_DONE;
	}
	dvd.thread_on = 1;

	int result = END_DONE;
	double last = now_seconds(), osd_built = 0, title_time = 0;
	while (!quit_requested)
	{
		double t = now_seconds();
		double elapsed = t - last;
		last = t;
		if (elapsed > 0.5) elapsed = 0.5;

		// A menu of the disc: the arrows and A belong to its buttons, also
		// in a menu whose button information is not in the packet just read
		// (otherwise they changed the sound and the subtitles under it).
		int menu = dvd_in_menu() || (!dvdnav_is_domain_vts(dvd.nav) && !dvdnav_is_domain_fp(dvd.nav));
		pci_t *pci = dvdnav_get_current_nav_pci(dvd.nav);

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
			if (menu && p.state == ST_PLAY)
			{
				int done = 1;
				switch (code)
				{
				case KEY_UP: dvdnav_upper_button_select(dvd.nav, pci); break;
				case KEY_DOWN: dvdnav_lower_button_select(dvd.nav, pci); break;
				case KEY_LEFT: dvdnav_left_button_select(dvd.nav, pci); break;
				case KEY_RIGHT: dvdnav_right_button_select(dvd.nav, pci); break;
				case KEY_ENTER:
				case KEY_PLAY:
				case KEY_PLAYCD:
					if (dvdnav_button_activate(dvd.nav, pci) == DVDNAV_STATUS_OK) dvd_jumped();
					else fprintf(stderr, "mediacenter: DVD menu, pulsante non attivato: %s\n", dvdnav_err_to_string(dvd.nav));
					break;
				case KEY_SPACE:
					if (dvdnav_menu_call(dvd.nav, DVD_MENU_Escape) == DVDNAV_STATUS_OK) dvd_jumped();
					break;
				default: done = 0; break;
				}
				if (done)
				{
					int32_t button = 0;
					dvdnav_get_current_highlight(dvd.nav, &button);
					fprintf(stderr, "mediacenter: DVD menu, tasto %d: pulsante %d di %d%s\n", code, (int)button,
						pci ? (int)pci->hli.hl_gi.btn_ns : -1, pci && pci->hli.hl_gi.hli_ss ? "" : " (pulsanti non attivi)");
					dvd_update_highlight();
					continue;
				}
			}
			switch (code)
			{
			case KEY_PLAY:
			case KEY_PLAYCD:
				if (p.state == ST_STOP)
				{
					dvdnav_title_play(dvd.nav, dvd.title > 0 ? dvd.title : 1);
					dvd_jumped();
				}
				else if (p.state == ST_FF || p.state == ST_REW)
				{
					dvd_scan_end();
				}
				p.state = ST_PLAY;
				break;
			case KEY_PAUSE:
			case KEY_PAUSECD:
			case KEY_PLAYPAUSE:
				if (p.state == ST_PLAY) p.state = ST_PAUSE;
				else if (p.state == ST_PAUSE) p.state = ST_PLAY;
				else if (p.state == ST_FF || p.state == ST_REW)
				{
					dvd_scan_end();
					p.state = ST_PAUSE;
				}
				break;
			case KEY_ESC:
			case KEY_STOP:
			case KEY_STOPCD:
				if (p.state == ST_STOP) break;
				p.state = ST_STOP;
				// the disc stays where it is; Play starts the title again
				DVD_LOCK();
				dvd.req_flush = 1;
				dvd.halt = 1;
				dvd.still = dvd.wait = 0;
				dvd.scanning = dvd.req_seek = dvd.seeking = dvd.ff_hold = 0;
				DVD_UNLOCK();
				fb_clear();
				have_frame = 0;
				break;
			case KEY_RIGHT:
			case KEY_NEXTSONG:
				if (dvdnav_next_pg_search(dvd.nav) == DVDNAV_STATUS_OK) dvd_jumped();
				break;
			case KEY_LEFT:
			case KEY_PREVIOUSSONG:
				if (dvdnav_prev_pg_search(dvd.nav) == DVDNAV_STATUS_OK) dvd_jumped();
				break;
			case KEY_PAGEDOWN:
			case KEY_FASTFORWARD:
			case KEY_PAGEUP:
			case KEY_REWIND:
			{
				int want = (code == KEY_PAGEDOWN || code == KEY_FASTFORWARD) ? ST_FF : ST_REW;
				if (p.state == ST_STOP || !dvdnav_is_domain_vts(dvd.nav)) break;
				if (p.state == want)
				{
					dvd_scan_end();
					p.state = ST_PLAY;
					break;
				}
				if (p.state != ST_FF && p.state != ST_REW) p.ff_time = title_time;
				audio_flush();
				DVD_LOCK();
				dvd.scanning = 1;
				DVD_UNLOCK();
				p.state = want;
				p.ff_step = 0.3; // first jump right away
				break;
			}
			case KEY_UP:
			case KEY_DOWN:
			{
				int n = dvd_count_audio();
				if (n > 1)
				{
					int cur = dvd_audio_logical();
					if (cur < 0) cur = 0;
					for (int k = 0; k < n; k++)
					{
						cur = (cur + (code == KEY_DOWN ? 1 : n - 1)) % n;
						if (dvdnav_get_audio_logical_stream(dvd.nav, cur) >= 0) break;
					}
					DVD_LOCK();
					dvd.user_audio = cur;
					dvd.req_audio = 1;
					DVD_UNLOCK();
				}
				else audio_mode = (audio_mode + (code == KEY_DOWN ? 1 : 2)) % 3;
				break;
			}
			case KEY_ENTER:
			{
				// subtitles: off, then each language
				int n = dvd_count_spu();
				if (!n) break;
				int next = dvd_spu_logical() + 1;
				while (next < n && dvdnav_get_spu_logical_stream(dvd.nav, next) < 0) next++;
				DVD_LOCK();
				dvd.user_spu = next < n ? next : -1;
				dvd_update_spu();
				DVD_UNLOCK();
				break;
			}
			case KEY_SPACE:
				if (dvdnav_menu_call(dvd.nav, DVD_MENU_Root) == DVDNAV_STATUS_OK ||
					dvdnav_menu_call(dvd.nav, DVD_MENU_Title) == DVDNAV_STATUS_OK)
				{
					dvd_jumped();
					if (p.state != ST_PLAY) p.state = ST_PLAY;
				}
				break;
			case KEY_TAB:
				p.confirm_exit = 1;
				break;
			}
		}
		if (result != END_DONE) break;

		AVFrame *show = NULL;
		int still_skip = 0, wait_skip = 0;
		if (p.state == ST_PLAY)
		{
			DVD_LOCK();
			mc.clock += elapsed;
			feed_audio_output();
			av_sync(elapsed);
			// a picture timed far ahead would hold back all the others
			while (mc.vq_count && mc.vq[0].pts > mc.clock + 3.0)
			{
				av_frame_free(&mc.vq[0].frame);
				memmove(mc.vq, mc.vq + 1, (mc.vq_count - 1) * sizeof(mc.vq[0]));
				mc.vq_count--;
			}
			show = due_frame();
			dvd_expire_subs();

			int drained = !mc.vq_count && mc.pend_len == 0 && mc.audio_end <= mc.clock;
			if (dvd.wait && drained)
			{
				dvd.wait = 0;
				wait_skip = 1;
			}
			if (dvd.still && !mc.vq_count)
			{
				// the still picture is on screen: hold it for its time
				if (isnan(dvd.still_until)) dvd.still_until = dvd.still == 255 ? INFINITY : t + dvd.still;
				// the clock keeps running so later subpictures do not appear early
				if (t >= dvd.still_until)
				{
					dvd.still = 0;
					still_skip = 1;
				}
			}
			int ended = dvd.stopped && !mc.vq_count && !mc.pend_len;
			DVD_UNLOCK();
			if (wait_skip) dvdnav_wait_skip(dvd.nav);
			if (still_skip) dvdnav_still_skip(dvd.nav);
			if (ended)
			{
				av_frame_free(&show);
				result = END_DONE;
				break;
			}
		}
		else if (p.state == ST_FF || p.state == ST_REW)
		{
			// show the picture found by the last jump, then jump again
			DVD_LOCK();
			if (mc.vq_count)
			{
				show = mc.vq[0].frame;
				mc.vq[0].frame = NULL;
				vq_clear();
			}
			mc.pend_len = 0;
			int busy = dvd.req_seek || dvd.seeking;
			DVD_UNLOCK();

			p.ff_step += elapsed;
			if (p.ff_step >= 0.3 && !busy)
			{
				double step = p.ff_step * DVD_FF_SPEED;
				p.ff_step = 0;
				p.ff_time += p.state == ST_FF ? step : -step;
				if (p.ff_time < 0) p.ff_time = 0;
				if (dvd.duration > 0 && p.ff_time > dvd.duration - 1) p.ff_time = dvd.duration - 1;
				DVD_LOCK();
				dvd.seek_to = p.ff_time;
				dvd.req_seek = 1;
				DVD_UNLOCK();
				if (p.ff_time <= 0 || (dvd.duration > 0 && p.ff_time >= dvd.duration - 1))
				{
					// the start or the end: play from there
					DVD_LOCK();
					dvd.scanning = 0;
					DVD_UNLOCK();
					p.state = ST_PLAY;
				}
			}
		}

		int64_t now90 = dvdnav_get_current_time(dvd.nav);
		if (now90 >= 0 && p.state != ST_FF && p.state != ST_REW) title_time = now90 / 90000.0;

		if (p.confirm_exit && t >= p.osd_until) p.confirm_exit = 0;
		int show_osd = p.state == ST_STOP || (t < p.osd_until && !(menu && p.state == ST_PLAY && !p.confirm_exit)) ||
			(menu && t < p.osd_until);
		if (show_osd && (p.dirty || t - osd_built >= 0.25))
		{
			osd_info_t info;
			memset(&info, 0, sizeof(info));
			info.state = p.state;
			info.confirm_exit = p.confirm_exit;
			info.time = (p.state == ST_FF || p.state == ST_REW) ? p.ff_time : title_time;
			info.duration = dvd.duration;
			int32_t title = 0, part = 0;
			if (dvdnav_current_title_info(dvd.nav, &title, &part) == DVDNAV_STATUS_OK && title > 0)
			{
				info.chapters = dvd.chapters;
				info.chapter = part;
			}
			info.audio_mode = audio_mode;
			dvd_make_labels();
			info.audio_label = dvd.audio_label[0] ? dvd.audio_label : NULL;
			if (menu)
			{
				info.state_label = "MENU DVD";
				info.legend = legend_dvd_menu;
				info.legend_n = sizeof(legend_dvd_menu) / sizeof(legend_dvd_menu[0]);
				info.audio_label = "";
				info.duration = 0;
				info.time = 0;
			}
			else
			{
				info.legend = legend_dvd_play;
				info.legend_n = sizeof(legend_dvd_play) / sizeof(legend_dvd_play[0]);
			}
			osd_build(&info);
			osd_built = t;
			p.dirty = 1;
		}
		if (show_osd != ovl_shown)
		{
			ovl_shown = show_osd;
			p.dirty = 1;
		}

		if (show) dvd_show(show);
		else if (dvd.last && dvd.overlay_dirty) dvd_show(dvd.last);
		else if (p.dirty) fb_present();
		p.dirty = 0;

		usleep(p.state == ST_PLAY ? 2000 : 10000);
	}

	dvd_stop_thread();
	native_fb = 0;
	fb_native(0, 0, 0);
	printf("mediacenter: DVD pictures shown %d, dropped late %d\n", frames_shown, frames_dropped);
	av_report();
	audio_close();
	exit_question = "Uscire dal Video CD?";
	dvd_close();
	return quit_requested ? END_QUIT : result;
}

// Returns 1 when device holds a DVD-Video.
static int dvd_detect(const char *device)
{
	dvdnav_t *nav = NULL;
	if (dvdnav_open(&nav, device) != DVDNAV_STATUS_OK) return 0;
	dvdnav_close(nav);
	return 1;
}

// mediacenter --bench <DVD>: plays the first title as fast as possible.
static int dvd_bench(const char *device)
{
	if (dvd_open(device)) return 1;
	dvdnav_title_play(dvd.nav, 1);
	int frames = 0;
	uint8_t *rgb = NULL;
	double t0 = now_seconds();
	double first = NAN;
	for (int blocks = 0; !dvd.stopped && blocks < 2000000; blocks++)
	{
		mc.clock = isnan(mc.last_vpts) ? 0 : mc.last_vpts; // keeps the timeline moving
		if (!dvd_step()) break;
		if (dvd.still || dvd.wait) break; // end of the title
		for (int i = 0; i < mc.vq_count; i++)
		{
			AVFrame *f = mc.vq[i].frame;
			if (isnan(first)) first = mc.vq[i].pts;
			int stride = (f->width * 4 + 63) & ~63;
			if (!rgb) rgb = (uint8_t *)av_malloc((size_t)stride * f->height);
			mc.sws = sws_getCachedContext(mc.sws, f->width, f->height, (enum AVPixelFormat)f->format,
				f->width, f->height, AV_PIX_FMT_BGRA, SWS_POINT, NULL, NULL, NULL);
			uint8_t *dst[4] = { rgb, NULL, NULL, NULL };
			int dst_stride[4] = { stride, 0, 0, 0 };
			if (mc.sws && rgb) sws_scale(mc.sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dst_stride);
		}
		frames += mc.vq_count;
		vq_clear();
		mc.pend_len = 0;
	}
	av_free(rgb);
	double secs = now_seconds() - t0;
	double media = isnan(first) ? 0 : mc.last_vpts - first;
	printf("mediacenter: DVD title 1: decoded %d pictures, %.1f s of video in %.1f s: %.0f%% of real time%s\n",
		frames, media, secs, secs > 0 ? 100.0 * media / secs : 0,
		secs > 0 && media / secs >= 1.0 ? " (plays)" : " (too slow)");
	dvd_close();
	return 0;
}
