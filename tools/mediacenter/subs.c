// Text subtitles for files (included by mediacenter.c after audio_ui.c).
//
// - External files next to the video, with its name: film.srt, film.ass,
//   film.ssa, film.vtt, also with a language in the name (film.ita.srt,
//   film.en.srt). Read whole when the video opens; UTF-8, or the Windows
//   code page of many .srt files (shown with its accented letters).
// - Tracks inside the file: SRT, ASS/SSA and WebVTT in MKV, mov_text in MP4.
//   Their packets come with the video and sound (decode_step) and are kept
//   as they arrive.
// Only the text is shown: no colours, positions or effects. The lines are
// drawn on the layer of the controls (ovl_a/ovl_v), so they also show on
// the accelerator's pictures, where the core blends that layer, and on the
// CRT; they hide while the controls are on screen.
// A changes the subtitles during the film: off, then each track.
// MC_SUB_LANG (Impostazioni): off, or the language to start with; without
// it an external file starts on, a track of the file only when it is marked
// default or forced.

#define SUB_TRACKS 8
#define SUB_TEXT 400
#define SUB_KEEP 64 // streams of the file looked at (keep[] of media_open)

typedef struct
{
	double start, end;
	char *text; // Latin-1 codes of the subtitle font, lines split by '\n'
} sub_cue_t;

typedef struct
{
	int stream;     // stream of the file, -1 for an external file
	int codec;      // AVCodecID of a stream
	char lang[8];   // ita, en... ("" when unknown)
	int def;        // marked default or forced in the file
	sub_cue_t *cue; // sorted by start
	int n, cap;
} sub_track_t;

static sub_track_t sub_tr[SUB_TRACKS];
static int sub_n;        // tracks
static int sub_cur = -1; // track shown, -1 off
static char sub_label[48];
static char sub_shown[SUB_TEXT * 2] = "\1"; // the text in the layer ("\1": something else)

// The controls with the subtitles' key.
static legend_t legend_subs[] =
{
	{ "START", "Play" },
	{ "SELECT", "Pausa" },
	{ "B", "Stop" },
	{ "\x10 \x11", "Capitolo" },
	{ "L", "\x10\x10" },
	{ "R", "\x11\x11" },
	{ "\x12 \x13", "Audio" },
	{ "A", "Sottotitoli" },
	{ "X", "Esci" },
};

static void sub_free(void)
{
	for (int i = 0; i < sub_n; i++)
	{
		for (int k = 0; k < sub_tr[i].n; k++) free(sub_tr[i].cue[k].text);
		free(sub_tr[i].cue);
	}
	memset(sub_tr, 0, sizeof(sub_tr));
	sub_n = 0;
	sub_cur = -1;
	ovl_band(-1); // the controls' band for what plays next
}

// Cleans one subtitle's text: tags <i> <font ...> and {\an8...} go, \N and
// \n of ASS become new lines, spaces at the ends of the lines go. The text
// comes in UTF-8 (utf8) or in the Windows code page.
static char *sub_clean(const char *src, size_t len, int utf8)
{
	char tmp[SUB_TEXT * 2];
	size_t n = 0;
	for (size_t i = 0; i < len && n + 2 < sizeof(tmp); i++)
	{
		char c = src[i];
		if (c == '<') { const char *e = memchr(src + i, '>', len - i); if (e && e - (src + i) < 64) { i = (size_t)(e - src); continue; } }
		if (c == '{') { const char *e = memchr(src + i, '}', len - i); if (e) { i = (size_t)(e - src); continue; } }
		if (c == '\\' && i + 1 < len && (src[i + 1] == 'N' || src[i + 1] == 'n')) { tmp[n++] = '\n'; i++; continue; }
		if (c == '\\' && i + 1 < len && src[i + 1] == 'h') { tmp[n++] = ' '; i++; continue; }
		if (c == '\r') continue;
		tmp[n++] = c;
	}
	tmp[n] = 0;
	char out[SUB_TEXT];
	if (utf8) aui_latin1(out, sizeof(out), tmp);
	else
	{
		// Windows-1252: the letters are Latin-1, the quotes and dashes become
		// the font's
		size_t k = 0;
		for (const unsigned char *p = (const unsigned char *)tmp; *p && k + 1 < sizeof(out); p++)
		{
			unsigned c = *p;
			if (c >= 0x80 && c < 0xA0)
				c = c == 0x85 ? 0x80 : (c == 0x91 || c == 0x92) ? '\'' : (c == 0x93 || c == 0x94) ? '"' : (c == 0x96 || c == 0x97) ? '-' : '?';
			out[k++] = (char)c;
		}
		out[k] = 0;
	}
	// spaces at the ends of the lines and empty lines go
	char res[SUB_TEXT];
	size_t r = 0;
	for (char *line = out; *line;)
	{
		char *end = strchr(line, '\n');
		size_t l = end ? (size_t)(end - line) : strlen(line);
		size_t a = 0, b = l;
		while (a < b && (line[a] == ' ' || line[a] == '\t')) a++;
		while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
		if (b > a && r + (b - a) + 2 < sizeof(res))
		{
			if (r) res[r++] = '\n';
			memcpy(res + r, line + a, b - a);
			r += b - a;
		}
		line += l + (end ? 1 : 0);
	}
	res[r] = 0;
	return r ? strdup(res) : NULL;
}

// Adds a subtitle in time order; one already there (read again after a
// jump back) is not repeated.
static void sub_add(sub_track_t *tr, double start, double end, char *text)
{
	if (!text) return;
	if (!(end > start)) end = start + 4.0;
	int at = tr->n;
	while (at > 0 && tr->cue[at - 1].start > start) at--;
	for (int k = at - 1; k >= 0 && tr->cue[k].start > start - 0.01; k--)
		if (!strcmp(tr->cue[k].text, text)) { free(text); return; }
	if (tr->n == tr->cap)
	{
		int cap = tr->cap ? tr->cap * 2 : 256;
		sub_cue_t *c = (sub_cue_t *)realloc(tr->cue, (size_t)cap * sizeof(*c));
		if (!c) { free(text); return; }
		tr->cue = c;
		tr->cap = cap;
	}
	memmove(tr->cue + at + 1, tr->cue + at, (size_t)(tr->n - at) * sizeof(*tr->cue));
	tr->cue[at].start = start;
	tr->cue[at].end = end;
	tr->cue[at].text = text;
	tr->n++;
}

// "01:02:03,456", "1:02:03.45", "02:03.456" -> seconds; -1 when it is not a time.
static double sub_time(const char *s, const char **end)
{
	double part[3] = { 0, 0, 0 };
	int n = 0;
	while (*s == ' ') s++;
	const char *p = s;
	for (;;)
	{
		if (*p < '0' || *p > '9') return -1;
		double v = 0;
		while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
		if (n < 3) part[n++] = v;
		if (*p == ':') { p++; continue; }
		break;
	}
	if (n < 2) return -1;
	double frac = 0;
	if (*p == ',' || *p == '.')
	{
		double scale = 0.1;
		for (p++; *p >= '0' && *p <= '9'; p++, scale /= 10) frac += (*p - '0') * scale;
	}
	if (end) *end = p;
	return (n == 3 ? part[0] * 3600 + part[1] * 60 + part[2] : part[0] * 60 + part[1]) + frac;
}

static int sub_valid_utf8(const unsigned char *s, size_t len)
{
	for (size_t i = 0; i < len;)
	{
		unsigned c = s[i];
		int more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
		if (more < 0 || i + more >= len + (more == 0)) return 0; // a sequence cut by the end
		for (int k = 1; k <= more; k++)
			if ((s[i + k] & 0xC0) != 0x80) return 0;
		i += 1 + more;
	}
	return 1;
}

// SRT and WebVTT: blocks with a "start --> end" line, then the text.
static void sub_parse_srt(sub_track_t *tr, char *buf, int utf8)
{
	char *line = buf;
	while (*line)
	{
		char *nl = strchr(line, '\n');
		char *next = nl ? nl + 1 : line + strlen(line);
		char *arrow = nl ? NULL : strstr(line, "-->");
		if (nl) { *nl = 0; arrow = strstr(line, "-->"); }
		if (arrow)
		{
			double a = sub_time(line, NULL), b = sub_time(arrow + 3, NULL);
			// the text: the lines up to an empty one
			char *t = next, *e = next;
			while (*e)
			{
				char *n2 = strchr(e, '\n');
				size_t l = n2 ? (size_t)(n2 - e) : strlen(e);
				if (l == 0 || (l == 1 && e[0] == '\r')) break;
				e += l + (n2 ? 1 : 0);
			}
			if (a >= 0 && b >= 0) sub_add(tr, a, b, sub_clean(t, (size_t)(e - t), utf8));
			next = e;
		}
		if (nl) *nl = '\n';
		line = next;
	}
}

// ASS / SSA: the Dialogue lines of [Events], with the fields of its Format line.
static void sub_parse_ass(sub_track_t *tr, char *buf, int utf8)
{
	int f_start = 1, f_end = 2, f_text = 9, nfields = 10, events = 0;
	for (char *line = buf; *line;)
	{
		char *nl = strchr(line, '\n');
		size_t l = nl ? (size_t)(nl - line) : strlen(line);
		if (line[0] == '[') events = !strncasecmp(line, "[Events]", 8);
		else if (events && !strncmp(line, "Format:", 7))
		{
			// which fields hold the times and the text
			int k = 0;
			for (char *p = line + 7; p < line + l; k++)
			{
				while (*p == ' ') p++;
				if (!strncasecmp(p, "Start", 5)) f_start = k;
				else if (!strncasecmp(p, "End", 3)) f_end = k;
				else if (!strncasecmp(p, "Text", 4)) f_text = k;
				char *c = memchr(p, ',', (size_t)(line + l - p));
				if (!c) { k++; break; }
				p = c + 1;
			}
			nfields = k;
			if (f_text != nfields - 1) f_text = nfields - 1;
		}
		else if (!strncmp(line, "Dialogue:", 9))
		{
			const char *field[16] = { 0 };
			int k = 0;
			const char *p = line + 9;
			field[k++] = p;
			while (k < nfields && k < 16 && (p = memchr(p, ',', (size_t)(line + l - p)))) field[k++] = ++p;
			if (k == nfields && f_start < k && f_end < k)
			{
				double a = sub_time(field[f_start], NULL), b = sub_time(field[f_end], NULL);
				const char *t = field[f_text];
				if (a >= 0 && b >= 0) sub_add(tr, a, b, sub_clean(t, (size_t)(line + l - t), utf8));
			}
		}
		line += l + (nl ? 1 : 0);
	}
}

static int sub_load_file(const char *path, const char *lang)
{
	if (sub_n >= SUB_TRACKS) return -1;
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); return -1; }
	char *buf = (char *)malloc((size_t)sz + 1);
	if (!buf) { fclose(f); return -1; }
	size_t len = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[len] = 0;
	char *s = buf;
	if (len >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) { s += 3; len -= 3; }
	if (len >= 2 && (!memcmp(s, "\xFF\xFE", 2) || !memcmp(s, "\xFE\xFF", 2)))
	{
		fprintf(stderr, "mediacenter: sottotitoli %s in UTF-16: non letti\n", path);
		free(buf);
		return -1;
	}
	int utf8 = sub_valid_utf8((const unsigned char *)s, len);
	sub_track_t *tr = &sub_tr[sub_n];
	memset(tr, 0, sizeof(*tr));
	tr->stream = -1;
	snprintf(tr->lang, sizeof(tr->lang), "%s", lang);
	const char *ext = strrchr(path, '.');
	if (ext && (!strcasecmp(ext, ".ass") || !strcasecmp(ext, ".ssa"))) sub_parse_ass(tr, s, utf8);
	else sub_parse_srt(tr, s, utf8);
	free(buf);
	if (!tr->n) { free(tr->cue); memset(tr, 0, sizeof(*tr)); return -1; }
	printf("mediacenter: sottotitoli %s: %d righe%s\n", path, tr->n, utf8 ? "" : " (codifica Windows)");
	sub_n++;
	return 0;
}

// External files with the video's name: film.srt, film.ita.srt...
static void sub_find_files(const char *path)
{
	if (path[0] != '/') return;
	const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
	if (!dot || dot < slash) dot = path + strlen(path);
	char dir[512], base[256];
	snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
	snprintf(base, sizeof(base), "%.*s", (int)(dot - slash - 1), slash + 1);
	size_t bl = strlen(base);
	DIR *d = opendir(dir[0] ? dir : "/");
	if (!d) return;
	char names[SUB_TRACKS][256];
	int found = 0;
	for (struct dirent *e; (e = readdir(d)) && found < SUB_TRACKS;)
	{
		const char *n = e->d_name;
		if (strncmp(n, base, bl) || n[bl] != '.') continue;
		const char *x = strrchr(n, '.');
		if (strcasecmp(x, ".srt") && strcasecmp(x, ".ass") && strcasecmp(x, ".ssa") && strcasecmp(x, ".vtt")) continue;
		snprintf(names[found++], sizeof(names[0]), "%s", n);
	}
	closedir(d);
	// film.srt (no language in the name) first, then in name order
	for (int i = 0; i < found; i++)
		for (int k = i + 1; k < found; k++)
		{
			int ti = strchr(names[i] + bl + 1, '.') != NULL, tk = strchr(names[k] + bl + 1, '.') != NULL;
			if (tk < ti || (tk == ti && strcmp(names[k], names[i]) < 0))
			{
				char t[256];
				memcpy(t, names[i], 256);
				memcpy(names[i], names[k], 256);
				memcpy(names[k], t, 256);
			}
		}
	for (int i = 0; i < found; i++)
	{
		// the language between the name and the extension (film.ita.srt)
		char lang[8] = "";
		const char *mid = names[i] + bl + 1, *x = strrchr(names[i], '.');
		if (x > mid && x - mid < (int)sizeof(lang)) snprintf(lang, sizeof(lang), "%.*s", (int)(x - mid), mid);
		char full[800];
		snprintf(full, sizeof(full), "%s/%s", dir, names[i]);
		sub_load_file(full, lang);
	}
}

static int sub_codec_ok(int id)
{
	return id == AV_CODEC_ID_SUBRIP || id == AV_CODEC_ID_TEXT || id == AV_CODEC_ID_ASS || id == AV_CODEC_ID_SSA ||
		id == AV_CODEC_ID_WEBVTT || id == AV_CODEC_ID_MOV_TEXT;
}

static void sub_update_label(void);

// When the video opens: external files, then the text tracks of the file;
// the track to start with. Returns 1 for each stream of the file to read
// (its packets reach sub_packet) through keep[].
static void sub_open(const char *path, char *keep)
{
	sub_free();
	sub_shown[0] = 1;
	sub_label[0] = 0;
	if (mc.vstream < 0 || svcd_on) return;
	sub_find_files(path);
	int ext = sub_n;
	for (unsigned i = 0; i < mc.fmt->nb_streams && sub_n < SUB_TRACKS; i++)
	{
		AVStream *st = mc.fmt->streams[i];
		if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE || !sub_codec_ok(st->codecpar->codec_id) || i >= SUB_KEEP) continue;
		sub_track_t *tr = &sub_tr[sub_n++];
		memset(tr, 0, sizeof(*tr));
		tr->stream = (int)i;
		tr->codec = st->codecpar->codec_id;
		tr->def = !!(st->disposition & (AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED));
		AVDictionaryEntry *l = av_dict_get(st->metadata, "language", NULL, 0);
		if (l && strcmp(l->value, "und")) snprintf(tr->lang, sizeof(tr->lang), "%s", l->value);
		keep[i] = 1;
		printf("mediacenter: sottotitoli nel file: flusso %u, %s%s%s\n", i, avcodec_get_name(st->codecpar->codec_id),
			tr->lang[0] ? ", " : "", tr->lang);
	}
	const char *want = getenv("MC_SUB_LANG");
	sub_cur = -1;
	if (want && !strcasecmp(want, "off")) {}
	else if (want && want[0])
	{
		for (int i = 0; i < sub_n && sub_cur < 0; i++)
			if (sub_tr[i].lang[0] && lang_is(sub_tr[i].lang, want)) sub_cur = i;
		for (int i = 0; i < ext && sub_cur < 0; i++)
			if (!sub_tr[i].lang[0]) sub_cur = i; // film.srt: put there to be seen
	}
	else
	{
		if (ext) sub_cur = 0;
		for (int i = ext; i < sub_n && sub_cur < 0; i++)
			if (sub_tr[i].def) sub_cur = i;
	}
	if (sub_n) printf("mediacenter: sottotitoli: %d tracce, all'inizio %d\n", sub_n, sub_cur + 1);
	sub_update_label();
}

// A packet of a text track of the file (decode_step).
static void sub_packet(const AVPacket *pkt)
{
	sub_track_t *tr = NULL;
	for (int i = 0; i < sub_n; i++)
		if (sub_tr[i].stream == pkt->stream_index) tr = &sub_tr[i];
	if (!tr || !pkt->data || pkt->size <= 0 || pkt->pts == AV_NOPTS_VALUE) return;
	AVRational tb = mc.fmt->streams[pkt->stream_index]->time_base;
	double start = ts_seconds(pkt->pts, tb) - mc.start;
	double end = pkt->duration > 0 ? start + pkt->duration * av_q2d(tb) : start + 4.0;
	const char *t = (const char *)pkt->data;
	size_t len = (size_t)pkt->size;
	if (tr->codec == AV_CODEC_ID_MOV_TEXT)
	{
		// 2 bytes of length, the text, then style boxes
		if (len < 2) return;
		size_t l = ((unsigned char)t[0] << 8) | (unsigned char)t[1];
		if (l > len - 2) l = len - 2;
		t += 2;
		len = l;
	}
	else if (tr->codec == AV_CODEC_ID_ASS || tr->codec == AV_CODEC_ID_SSA)
	{
		// ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
		const char *p = t;
		for (int k = 0; k < 8 && p; k++)
		{
			p = memchr(p, ',', (size_t)(t + len - p));
			if (p) p++;
		}
		if (!p) return;
		len -= (size_t)(p - t);
		t = p;
	}
	sub_add(tr, start, end, sub_clean(t, len, 1));
}

// The text on screen at time t (NULL: none), the subtitles shown together
// one under the other.
static const char *sub_text_at(double t)
{
	static char buf[SUB_TEXT * 2];
	if (sub_cur < 0 || sub_cur >= sub_n) return NULL;
	sub_track_t *tr = &sub_tr[sub_cur];
	// after the last subtitle started by t
	int lo = 0, hi = tr->n;
	while (lo < hi) { int m = (lo + hi) / 2; if (tr->cue[m].start <= t) lo = m + 1; else hi = m; }
	const sub_cue_t *on[3];
	int n = 0;
	for (int k = lo - 1; k >= 0 && k >= lo - 16 && n < 3; k--)
		if (tr->cue[k].end > t) on[n++] = &tr->cue[k];
	if (!n) return NULL;
	buf[0] = 0;
	for (int k = n - 1; k >= 0; k--)
	{
		size_t l = strlen(buf);
		snprintf(buf + l, sizeof(buf) - l, "%s%s", l ? "\n" : "", on[k]->text);
	}
	return buf;
}

// "  Sott. 1/2 ita" for the status line, "" without tracks.
static void sub_update_label(void)
{
	sub_label[0] = 0;
	if (!sub_n) return;
	char lang[10] = "";
	if (sub_cur >= 0 && sub_tr[sub_cur].lang[0]) snprintf(lang, sizeof(lang), " %s", sub_tr[sub_cur].lang);
	if (sub_cur < 0) snprintf(sub_label, sizeof(sub_label), "%s", TR("  Sott. no"));
	else snprintf(sub_label, sizeof(sub_label), TR("  Sott. %d/%d%s"), sub_cur + 1, sub_n, lang);
}

// A: off, then each track.
static void sub_next(void)
{
	if (!sub_n) return;
	sub_cur = sub_cur + 1 >= sub_n ? -1 : sub_cur + 1;
	sub_update_label();
	printf("mediacenter: sottotitoli: %d\n", sub_cur + 1);
}

// Draws the text at the bottom of the layer of the controls: the font of the
// audio screen (larger on big frame buffers), white with a dark border.
static void sub_build(const char *text)
{
	const afont_t *f = &afont_title;
	int k = fb_h >= 900 ? 2 : 1;           // pixels of the frame buffer per font pixel
	int border = fb_h >= 900 ? 4 : 2;
	// the band: up to half the screen; with the accelerator no taller than
	// the core blends (ACC_OVL_LEN), a smaller font when two lines do not fit
	int most = fb_h / 2;
	if (acc.on && fb_w > 0 && (int)(ACC_OVL_LEN / ((size_t)fb_w * 2)) < most) most = (int)(ACC_OVL_LEN / ((size_t)fb_w * 2));
	if (k == 2 && 2 * (f->size + 6) * 2 + SY(12) + 2 * border > most) { k = 1; border = 2; }
	int line_h = (f->size + 6) * k;
	int maxw = fb_w - 2 * safe_x();
	// the lines, each cut at a space when it is too wide
	char lines[6][160];
	int n = 0;
	for (const char *p = text; *p && n < 6;)
	{
		const char *e = strchr(p, '\n');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		char seg[400];
		snprintf(seg, sizeof(seg), "%.*s", (int)l, p);
		char *s = seg;
		while (*s && n < 6)
		{
			// the most words that fit
			size_t cut = strlen(s);
			char save;
			for (;;)
			{
				save = s[cut];
				s[cut] = 0;
				int w = a_text_w(f, s) * k;
				s[cut] = save;
				if (w <= maxw || cut == 0) break;
				size_t sp = cut;
				while (sp > 0 && s[sp - 1] != ' ') sp--;
				if (sp == 0) { while (cut > 0 && a_text_w(f, s) * k > maxw) cut--; break; }
				cut = sp - 1;
			}
			if (cut == 0) cut = strlen(s);
			snprintf(lines[n++], sizeof(lines[0]), "%.*s", (int)cut, s);
			s += cut;
			while (*s == ' ') s++;
		}
		p += l + (e ? 1 : 0);
	}
	// the band as tall as the lines (from the bottom, as many as fit)
	int rows = n * line_h + SY(12) + 2 * border;
	if (rows > most) rows = most;
	ovl_band(fb_h - rows);
	memset(ovl_a, 0, (size_t)fb_w * ovl_h);
	memset(ovl_v, 0, (size_t)fb_w * ovl_h);
	// from the bottom, as many as fit in the layer
	int bottom = ovl_h - SY(12);
	int fit = (bottom - border) / line_h;
	if (fit < 1) return;
	int first = n > fit ? n - fit : 0;
	int y = bottom - (n - first) * line_h;
	for (int i = first; i < n; i++, y += line_h)
	{
		int w = a_text_w(f, lines[i]) * k;
		int x0 = (fb_w - w) / 2;
		// the line's glyphs in a mask, the mask spread by the border (a
		// square: across, then down), then the border dark and the text white
		int mw = w + 2 * border, mh = line_h + 2 * border;
		uint8_t *mask = (uint8_t *)calloc((size_t)mw * mh, 3);
		if (!mask) break;
		uint8_t *row = mask + (size_t)mw * mh, *dil = row + (size_t)mw * mh;
		int x = border, base = border + f->ascent * k;
		for (const unsigned char *c = (const unsigned char *)lines[i]; *c; c++)
		{
			const afont_glyph_t *g = &f->g[*c];
			const unsigned char *a = f->a + g->off;
			for (int j = 0; j < g->h * k; j++)
				for (int ii = 0; ii < g->w * k; ii++)
				{
					int px = x + g->x * k + ii, py = base + g->y * k + j;
					if (px < 0 || px >= mw || py < 0 || py >= mh) continue;
					uint8_t al = a[(j / k) * g->w + ii / k], *m = &mask[(size_t)py * mw + px];
					if (al > *m) *m = al;
				}
			x += g->adv * k;
		}
		for (int yy = 0; yy < mh; yy++)
			for (int xx = 0; xx < mw; xx++)
			{
				uint8_t mx = 0;
				for (int d = -border; d <= border; d++)
				{
					int q = xx + d;
					if (q >= 0 && q < mw && mask[(size_t)yy * mw + q] > mx) mx = mask[(size_t)yy * mw + q];
				}
				row[(size_t)yy * mw + xx] = mx;
			}
		for (int yy = 0; yy < mh; yy++)
			for (int xx = 0; xx < mw; xx++)
			{
				uint8_t mx = 0;
				for (int d = -border; d <= border; d++)
				{
					int q = yy + d;
					if (q >= 0 && q < mh && row[(size_t)q * mw + xx] > mx) mx = row[(size_t)q * mw + xx];
				}
				dil[(size_t)yy * mw + xx] = mx;
			}
		int ox = x0 - border, oy = y - border;
		for (int yy = 0; yy < mh; yy++)
			for (int xx = 0; xx < mw; xx++)
			{
				size_t o = (size_t)yy * mw + xx;
				if (dil[o]) ovl_px(ox + xx, oy + yy, 0, (uint8_t)(dil[o] * 200 / 255));
				if (mask[o]) ovl_px(ox + xx, oy + yy, 255, (uint8_t)(mask[o] * TEXT_A / 255));
			}
		free(mask);
	}
	ovl_smooth();
	ovl_gen++;
}
