// ---------------------------------------------------------------------------
// Tracker modules (included by mediacenter.c)
//
// MOD, XM (FastTracker 2), S3M, IT and the other formats libxmp loads are
// played by libxmp (MIT licence). Its sound, 48 kHz 16-bit stereo, reaches
// FFmpeg as a WAV file made up here (as for an audio CD, cdda.c), so pause,
// seek, fast forward, the equalizer and the audio screen work as for any
// other audio file. A seek starts the module again at the row nearest that
// time.
// ---------------------------------------------------------------------------

#include <ctype.h>
#include <xmp.h>

#define MOD_HDR 44

static struct
{
	xmp_context ctx;
	int64_t pos;       // position in the WAV file
	int64_t size;      // its size: header + the module's estimated length
	int64_t played;    // bytes of sound made since the last start (from pos0)
	int64_t pos0;      // WAV position the module was last started at
	int ended;
	uint8_t hdr[MOD_HDR];
	char type[64];
} mp;

// The module from WAV position pos (after the header): libxmp starts at the
// row nearest that time.
static void mod_start_at(int64_t pos)
{
	int64_t bytes = pos > MOD_HDR ? pos - MOD_HDR : 0;
	int ms = (int)(bytes / 4 * 1000 / MC_AUDIO_RATE);
	xmp_restart_module(mp.ctx);
	if (ms > 0) xmp_seek_time(mp.ctx, ms);
	mp.pos0 = pos;
	mp.played = 0;
	mp.ended = 0;
}

static int mod_io_read(void *opaque, uint8_t *buf, int size)
{
	(void)opaque;
	if (mp.pos >= mp.size || quit_requested) return AVERROR_EOF;
	if (mp.pos < MOD_HDR)
	{
		int n = MOD_HDR - (int)mp.pos;
		if (n > size) n = size;
		memcpy(buf, mp.hdr + mp.pos, n);
		mp.pos += n;
		return n;
	}
	if (mp.pos != mp.pos0 + mp.played) mod_start_at(mp.pos);
	if (mp.ended) return AVERROR_EOF;
	int n = size & ~3;
	if (n > mp.size - mp.pos) n = (int)(mp.size - mp.pos);
	if (n <= 0) return AVERROR_EOF;
	if (xmp_play_buffer(mp.ctx, buf, n, 0) < 0)
	{
		// the module's end (or its loop back, which is not played)
		mp.ended = 1;
		return AVERROR_EOF;
	}
	mp.played += n;
	mp.pos += n;
	return n;
}

static int64_t mod_io_seek(void *opaque, int64_t offset, int whence)
{
	(void)opaque;
	if (whence == AVSEEK_SIZE) return mp.size;
	whence &= ~AVSEEK_FORCE;
	if (whence == SEEK_CUR) offset += mp.pos;
	else if (whence == SEEK_END) offset += mp.size;
	else if (whence != SEEK_SET) return -1;
	if (offset < 0) return -1;
	mp.pos = offset < mp.size ? offset : mp.size;
	return mp.pos;
}

// The format of the module for the audio screen ("XM  FastTracker II").
static const char *mod_format_label(void)
{
	return mp.type[0] ? mp.type : NULL;
}

static int mod_play(const char *path, int in_playlist)
{
	mp.ctx = xmp_create_context();
	if (!mp.ctx) return END_DONE;
	if (xmp_load_module(mp.ctx, path) < 0)
	{
		fprintf(stderr, "mediacenter: modulo non leggibile: %s\n", path);
		xmp_free_context(mp.ctx);
		mp.ctx = NULL;
		return END_DONE;
	}
	if (xmp_start_player(mp.ctx, MC_AUDIO_RATE, 0) < 0)
	{
		xmp_release_module(mp.ctx);
		xmp_free_context(mp.ctx);
		mp.ctx = NULL;
		return END_DONE;
	}
	xmp_set_player(mp.ctx, XMP_PLAYER_INTERP, XMP_INTERP_SPLINE);
	struct xmp_module_info mi;
	struct xmp_frame_info fi;
	xmp_get_module_info(mp.ctx, &mi);
	xmp_get_frame_info(mp.ctx, &fi);
	int ms = fi.total_time > 0 ? fi.total_time : 10 * 60 * 1000;
	uint32_t data = (uint32_t)((int64_t)ms * MC_AUDIO_RATE / 1000) * 4;
	mp.size = MOD_HDR + data;
	uint8_t *h = mp.hdr;
	memcpy(h, "RIFF", 4);
	put32(h + 4, 36 + data);
	memcpy(h + 8, "WAVEfmt ", 8);
	put32(h + 16, 16);
	h[20] = 1; h[21] = 0;
	h[22] = 2; h[23] = 0;
	put32(h + 24, MC_AUDIO_RATE);
	put32(h + 28, MC_AUDIO_RATE * 4);
	h[32] = 4; h[33] = 0;
	h[34] = 16; h[35] = 0;
	memcpy(h + 36, "data", 4);
	put32(h + 40, data);
	mp.pos = 0;
	mod_start_at(MOD_HDR);

	// "XM  FastTracker II" from libxmp's "Fast Tracker II (XM)"
	const char *ext = strrchr(path, '.');
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char kind[16] = "MOD";
	if (ext && ext > base && strncasecmp(base, "mod.", 4))
	{
		snprintf(kind, sizeof(kind), "%s", ext + 1);
		for (char *p = kind; *p; p++) *p = (char)toupper((unsigned char)*p);
	}
	char tracker[64];
	snprintf(tracker, sizeof(tracker), "%s", mi.mod->type);
	char *paren = strstr(tracker, " (");
	if (paren) *paren = 0;
	snprintf(mp.type, sizeof(mp.type), "%s  %s  %d canali", kind, tracker, mi.mod->chn);
	printf("mediacenter: modulo %s: \"%s\", %s, %d canali, %d:%02d\n", path, mi.mod->name, mi.mod->type, mi.mod->chn,
		ms / 60000, ms / 1000 % 60);

	const int io_size = 16384;
	uint8_t *io_buf = (uint8_t *)av_malloc(io_size);
	media_io = io_buf ? avio_alloc_context(io_buf, io_size, 0, NULL, mod_io_read, NULL, mod_io_seek) : NULL;
	int r = END_DONE;
	if (media_io && !media_open(path))
	{
		// the module's title, otherwise the file name
		if (mi.mod->name[0])
		{
			snprintf(mc.title, sizeof(mc.title), "%s", mi.mod->name);
			av_dict_set(&mc.fmt->metadata, "title", mi.mod->name, 0);
		}
		aui_fmt_label = mod_format_label;
		aui_path = path;
		r = media_play(in_playlist);
		aui_path = NULL;
		aui_fmt_label = NULL;
		media_close();
	}
	else av_free(media_io ? NULL : io_buf);
	if (media_io)
	{
		av_freep(&media_io->buffer);
		avio_context_free(&media_io);
	}
	xmp_end_player(mp.ctx);
	xmp_release_module(mp.ctx);
	xmp_free_context(mp.ctx);
	mp.ctx = NULL;
	mp.type[0] = 0;
	return r;
}
