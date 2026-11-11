// ---------------------------------------------------------------------------
// Jellyfin (included by mediacenter.c)
//
// A client for a Jellyfin media server (https://jellyfin.org) on the home
// network, written from Jellyfin's public HTTP API. The server's libraries
// are browsed like a folder; a file that this player reads is sent as it is
// (the original file, so the FPGA accelerator, the CRT output, the audio
// screen, subtitles and audio tracks work as for a file on the SD card).
// Only what the player cannot read (HEVC, AV1, VP9, 10-bit H.264, above
// 1920x1080, TrueHD sound...) is converted by the server to H.264 + AC3
// (HLS, so seeking works there too).
//
// The server: MC_JELLYFIN=http://192.168.0.10:8096 in mediacenter.ini, or
// found on the network (Jellyfin answers a UDP broadcast on port 7359).
// The account: Quick Connect (a code shown here, approved in Jellyfin on the
// computer), or MC_JELLYFIN_USER and MC_JELLYFIN_PASS in mediacenter.ini.
// The key Jellyfin gives is kept in /media/fat/linux/mediacenter_jellyfin.cfg.
// MC_JELLYFIN=0 hides the source.
// ---------------------------------------------------------------------------

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define JF_CFG "/media/fat/linux/mediacenter_jellyfin.cfg"

static char jf_server[256];  // http://host:port, no slash at the end
static char jf_token[128];   // the access key Jellyfin gave this MiSTer
static char jf_uid[64];      // the user's id
static char jf_devid[40];    // this MiSTer for Jellyfin, made up once
static int jf_net_ready;

// --- JSON -------------------------------------------------------------------
// A small parser for Jellyfin's answers: a tree of values over the text,
// whose strings are decoded in place.

typedef struct jv
{
	char t;            // 'o' object, 'a' array, 's' string, 'n' number, 'b' true/false, 'z' null
	char *k;           // name in the parent object
	char *s;           // string
	double n;          // number, or 1/0 for true/false
	struct jv *c, *nx; // first child, next sibling
} jv;

static char *jp;

static void jv_free(jv *v)
{
	while (v)
	{
		jv *nx = v->nx;
		jv_free(v->c);
		free(v);
		v = nx;
	}
}

static void jp_ws(void)
{
	while (*jp == ' ' || *jp == '\t' || *jp == '\n' || *jp == '\r') jp++;
}

static int jp_hex4(const char *p)
{
	int v = 0;
	for (int i = 0; i < 4; i++)
	{
		int c = p[i], d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else return -1;
		v = v * 16 + d;
	}
	return v;
}

static char *jp_string(void)
{
	if (*jp != '"') return NULL;
	char *out = ++jp, *w = jp;
	while (*jp && *jp != '"')
	{
		if (*jp != '\\') { *w++ = *jp++; continue; }
		jp++;
		char c = *jp++;
		switch (c)
		{
		case 'n': *w++ = '\n'; break;
		case 't': *w++ = '\t'; break;
		case 'r': *w++ = '\r'; break;
		case 'b': *w++ = '\b'; break;
		case 'f': *w++ = '\f'; break;
		case 'u':
		{
			int u = jp_hex4(jp);
			if (u < 0) return NULL;
			jp += 4;
			if (u >= 0xD800 && u < 0xDC00 && jp[0] == '\\' && jp[1] == 'u')
			{
				int lo = jp_hex4(jp + 2);
				if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); jp += 6; }
			}
			// UTF-8, never longer than the escape it replaces
			if (u < 0x80) *w++ = (char)u;
			else if (u < 0x800) { *w++ = (char)(0xC0 | u >> 6); *w++ = (char)(0x80 | (u & 63)); }
			else if (u < 0x10000) { *w++ = (char)(0xE0 | u >> 12); *w++ = (char)(0x80 | (u >> 6 & 63)); *w++ = (char)(0x80 | (u & 63)); }
			else { *w++ = (char)(0xF0 | u >> 18); *w++ = (char)(0x80 | (u >> 12 & 63)); *w++ = (char)(0x80 | (u >> 6 & 63)); *w++ = (char)(0x80 | (u & 63)); }
			break;
		}
		default: *w++ = c; break; // " \ /
		}
	}
	if (*jp != '"') return NULL;
	jp++;
	*w = 0;
	return out;
}

static jv *jp_value(int depth)
{
	jp_ws();
	if (depth > 64) return NULL;
	jv *v = (jv *)calloc(1, sizeof(jv));
	if (!v) return NULL;
	if (*jp == '{' || *jp == '[')
	{
		char close = *jp == '{' ? '}' : ']';
		v->t = *jp == '{' ? 'o' : 'a';
		jp++;
		jv **tail = &v->c;
		jp_ws();
		if (*jp == close) { jp++; return v; }
		for (;;)
		{
			char *key = NULL;
			jp_ws();
			if (v->t == 'o')
			{
				key = jp_string();
				jp_ws();
				if (!key || *jp != ':') { jv_free(v); return NULL; }
				jp++;
			}
			jv *c = jp_value(depth + 1);
			if (!c) { jv_free(v); return NULL; }
			c->k = key;
			*tail = c;
			tail = &c->nx;
			jp_ws();
			if (*jp == ',') { jp++; continue; }
			if (*jp == close) { jp++; return v; }
			jv_free(v);
			return NULL;
		}
	}
	if (*jp == '"')
	{
		v->t = 's';
		v->s = jp_string();
		if (!v->s) { free(v); return NULL; }
		return v;
	}
	if (!strncmp(jp, "true", 4)) { v->t = 'b'; v->n = 1; jp += 4; return v; }
	if (!strncmp(jp, "false", 5)) { v->t = 'b'; jp += 5; return v; }
	if (!strncmp(jp, "null", 4)) { v->t = 'z'; jp += 4; return v; }
	char *end;
	v->n = strtod(jp, &end);
	if (end == jp) { free(v); return NULL; }
	v->t = 'n';
	jp = end;
	return v;
}

static jv *jv_parse(char *text)
{
	if (!text) return NULL;
	jp = text;
	return jp_value(0);
}

static jv *jv_get(const jv *o, const char *key)
{
	if (!o || o->t != 'o') return NULL;
	for (jv *c = o->c; c; c = c->nx)
		if (c->k && !strcmp(c->k, key)) return c;
	return NULL;
}

static const char *jv_str(const jv *o, const char *key)
{
	jv *v = jv_get(o, key);
	return v && v->t == 's' ? v->s : "";
}

static double jv_num(const jv *o, const char *key, double def)
{
	jv *v = jv_get(o, key);
	return v && (v->t == 'n' || v->t == 'b') ? v->n : def;
}

// A string for a JSON body, with its quotes.
static void jv_quote(char *dst, size_t len, const char *s)
{
	size_t n = 0;
	if (len < 3) return;
	dst[n++] = '"';
	for (; *s && n + 8 < len; s++)
	{
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = (char)c; }
		else if (c < 0x20) n += snprintf(dst + n, len - n, "\\u%04x", c);
		else dst[n++] = (char)c;
	}
	dst[n++] = '"';
	dst[n] = 0;
}

// --- HTTP ---------------------------------------------------------------------

static void jf_net_init(void)
{
	if (jf_net_ready) return;
	avformat_network_init();
	jf_net_ready = 1;
}

// Jellyfin's Authorization header, with the key once signed in.
static void jf_headers(char *dst, size_t len)
{
	int n = snprintf(dst, len, "Authorization: MediaBrowser Client=\"MiSTer MediaCenter\", Device=\"MiSTer\", DeviceId=\"%s\", Version=\"%s\"",
		jf_devid, MC_BUILD);
	if (jf_token[0] && n > 0 && (size_t)n < len) n += snprintf(dst + n, len - n, ", Token=\"%s\"", jf_token);
	if (n > 0 && (size_t)n < len) snprintf(dst + n, len - n, "\r\nAccept: application/json\r\n");
}

// Options for FFmpeg's http: the header and a time limit, for the requests
// and for the streams.
static void jf_set_opts(AVDictionary **o)
{
	char h[512];
	jf_headers(h, sizeof(h));
	av_dict_set(o, "headers", h, 0);
	av_dict_set(o, "icy", "0", 0);
	av_dict_set(o, "rw_timeout", "10000000", 0);
}

// method "GET", "POST" (body: JSON text) or "DELETE" on path ("/Users/...").
// Returns the answer (to free), NULL on an error (*err: FFmpeg's code, for
// example AVERROR_HTTP_UNAUTHORIZED or AVERROR_HTTP_NOT_FOUND).
static char *jf_http(const char *method, const char *path, const char *body, int *err)
{
	jf_net_init();
	char url[1024];
	snprintf(url, sizeof(url), "%s%s", jf_server, path);
	AVDictionary *o = NULL;
	jf_set_opts(&o);
	if (strcmp(method, "GET")) av_dict_set(&o, "method", method, 0);
	if (!strcmp(method, "POST"))
	{
		// FFmpeg takes the body of a POST as hex digits
		if (!body) body = "{}";
		size_t bl = strlen(body);
		char *hex = (char *)malloc(bl * 2 + 1);
		if (!hex) { av_dict_free(&o); return NULL; }
		for (size_t i = 0; i < bl; i++) sprintf(hex + i * 2, "%02x", (unsigned char)body[i]);
		hex[bl * 2] = 0;
		av_dict_set(&o, "post_data", hex, 0);
		av_dict_set(&o, "content_type", "application/json", 0);
		free(hex);
	}
	AVIOContext *pb = NULL;
	int r = avio_open2(&pb, url, AVIO_FLAG_READ, NULL, &o);
	av_dict_free(&o);
	if (err) *err = r < 0 ? r : 0;
	if (r < 0) return NULL;
	size_t cap = 65536, len = 0;
	char *buf = (char *)malloc(cap);
	while (buf)
	{
		if (cap - len < 16384)
		{
			char *nb = (char *)realloc(buf, cap * 2);
			if (!nb) { free(buf); buf = NULL; break; }
			buf = nb;
			cap *= 2;
		}
		int n = avio_read(pb, (unsigned char *)buf + len, (int)(cap - len - 1));
		if (n <= 0) break;
		len += n;
	}
	avio_closep(&pb);
	if (buf) buf[len] = 0;
	return buf;
}

// The answer of a request as a tree; the text stays with the tree (in the
// root's k) and goes with jf_free.
static jv *jf_tree(char *text, int *err)
{
	jv *v = jv_parse(text);
	if (!v)
	{
		free(text);
		if (err && !*err) *err = AVERROR_INVALIDDATA;
		return NULL;
	}
	v->k = text;
	return v;
}

static jv *jf_get_json(const char *path, int *err)
{
	return jf_tree(jf_http("GET", path, NULL, err), err);
}

static void jf_free(jv *v)
{
	if (!v) return;
	char *text = v->k;
	jv_free(v);
	free(text);
}

// The newer path of a Jellyfin call (10.9 and later), then the older one
// (10.8) when the server does not know it.
static jv *jf_get_json2(const char *path, const char *old_path, int *err)
{
	int e = 0;
	jv *v = jf_get_json(path, &e);
	if (!v && old_path && (e == AVERROR_HTTP_NOT_FOUND || e == AVERROR_HTTP_BAD_REQUEST || e == AVERROR_INVALIDDATA))
		v = jf_get_json(old_path, &e);
	if (err) *err = e;
	return v;
}

// Characters that may stay as they are in a URL query.
static void jf_urlenc(char *dst, size_t len, const char *s)
{
	size_t n = 0;
	for (; *s && n + 4 < len; s++)
	{
		unsigned char c = (unsigned char)*s;
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') dst[n++] = (char)c;
		else n += snprintf(dst + n, len - n, "%%%02X", c);
	}
	dst[n] = 0;
}

// --- Settings ---------------------------------------------------------------

static void jf_load_cfg(void)
{
	FILE *f = fopen(JF_CFG, "r");
	if (!f) return;
	char line[512], server[256] = "";
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "SERVER=", 7)) snprintf(server, sizeof(server), "%s", line + 7);
		else if (!strncmp(line, "TOKEN=", 6)) snprintf(jf_token, sizeof(jf_token), "%s", line + 6);
		else if (!strncmp(line, "USERID=", 7)) snprintf(jf_uid, sizeof(jf_uid), "%s", line + 7);
		else if (!strncmp(line, "DEVICEID=", 9)) snprintf(jf_devid, sizeof(jf_devid), "%s", line + 9);
	}
	fclose(f);
	if (!jf_server[0]) snprintf(jf_server, sizeof(jf_server), "%s", server);
	// a key belongs to the server it came from
	else if (strcmp(server, jf_server)) jf_token[0] = jf_uid[0] = 0;
}

static void jf_save_cfg(void)
{
	FILE *f = fopen(JF_CFG, "w");
	if (!f) return;
	fprintf(f, "SERVER=%s\nTOKEN=%s\nUSERID=%s\nDEVICEID=%s\n", jf_server, jf_token, jf_uid, jf_devid);
	fclose(f);
	sync();
}

static void jf_make_devid(void)
{
	if (jf_devid[0]) return;
	unsigned char r[16];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || read(fd, r, sizeof(r)) != (ssize_t)sizeof(r))
		for (unsigned i = 0; i < sizeof(r); i++) r[i] = (unsigned char)(rand() ^ (int)time(NULL) >> (i % 8));
	if (fd >= 0) close(fd);
	for (unsigned i = 0; i < sizeof(r); i++) sprintf(jf_devid + i * 2, "%02x", r[i]);
}

// A network address other than the loopback: the source is offered.
static int jf_has_network(void)
{
	struct ifaddrs *ifa = NULL, *i;
	int found = 0;
	if (getifaddrs(&ifa)) return 0;
	for (i = ifa; i && !found; i = i->ifa_next)
		found = i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(i->ifa_flags & IFF_LOOPBACK) && (i->ifa_flags & IFF_UP);
	freeifaddrs(ifa);
	return found;
}

static int jf_enabled(void)
{
	const char *e = getenv("MC_JELLYFIN");
	if (e && !strcmp(e, "0")) return 0;
	if (e && *e) return 1;
	struct stat st;
	return !stat(JF_CFG, &st) || jf_has_network();
}

// Jellyfin's discovery: "who is JellyfinServer?" broadcast on UDP port 7359,
// the servers answer {"Address":"http://...:8096","Id":...,"Name":...}.
static int jf_discover(char *addr, size_t len, char *name, size_t nlen)
{
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0) return -1;
	int on = 1;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(7359);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	const char *ask = "who is JellyfinServer?";
	int found = 0;
	for (int round = 0; round < 3 && !found; round++)
	{
		sendto(s, ask, strlen(ask), 0, (struct sockaddr *)&to, sizeof(to));
		double until = now_seconds() + 0.7;
		while (!found && now_seconds() < until)
		{
			struct timeval tv = { 0, 100000 };
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(s, &fds);
			if (select(s + 1, &fds, NULL, NULL, &tv) <= 0) continue;
			char buf[1024];
			ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
			if (n <= 0) continue;
			buf[n] = 0;
			jv *v = jv_parse(buf);
			const char *a = jv_str(v, "Address");
			if (!strncmp(a, "http://", 7))
			{
				snprintf(addr, len, "%s", a);
				snprintf(name, nlen, "%s", jv_str(v, "Name"));
				found = 1;
			}
			jv_free(v);
		}
	}
	close(s);
	return found ? 0 : -1;
}

// --- Text for the lists -----------------------------------------------------

// The lists draw ASCII: letters with accents lose them ("Città" -> "Citta").
static void jf_ascii(char *dst, size_t len, const char *s)
{
	static const char lat[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
	size_t n = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p && n + 2 < len)
	{
		if (*p < 0x80) { dst[n++] = (char)*p++; continue; }
		int u = 0, extra = 0;
		if ((*p & 0xE0) == 0xC0) { u = *p & 0x1F; extra = 1; }
		else if ((*p & 0xF0) == 0xE0) { u = *p & 0x0F; extra = 2; }
		else if ((*p & 0xF8) == 0xF0) { u = *p & 0x07; extra = 3; }
		p++;
		for (; extra && (*p & 0xC0) == 0x80; extra--) u = u << 6 | (*p++ & 0x3F);
		if (u >= 0xC0 && u <= 0xFF) dst[n++] = lat[u - 0xC0];
		else if (u == 0x2019 || u == 0x2018) dst[n++] = '\'';
		else if (u == 0x201C || u == 0x201D) dst[n++] = '"';
		else if (u == 0x2013 || u == 0x2014) dst[n++] = '-';
		else if (u == 0x2026 && n + 4 < len) { dst[n++] = '.'; dst[n++] = '.'; dst[n++] = '.'; }
		else dst[n++] = '?';
	}
	dst[n] = 0;
}

static void jf_ticks_text(char *dst, size_t len, double ticks)
{
	int s = (int)(ticks / 10000000.0);
	if (s >= 3600) snprintf(dst, len, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else snprintf(dst, len, "%d:%02d", s / 60, s % 60);
}

// --- Screens ------------------------------------------------------------------

// Lines of text on the list screen (no selection).
static void jf_screen(const char *title, const char *const *lines, int n)
{
	draw_browser(title, lines, NULL, n, -1, NULL);
}

static void jf_message(const char *title, const char *text)
{
	const char *lines[1] = { text };
	jf_screen(title, lines, 1);
	wait_key_for(4.0);
}

static const char *jf_err_text(int e)
{
	if (e == AVERROR_HTTP_UNAUTHORIZED || e == AVERROR_HTTP_FORBIDDEN) return "Accesso rifiutato dal server";
	if (e == AVERROR_HTTP_NOT_FOUND) return "Il server non conosce questa richiesta";
	if (e == AVERROR_INVALIDDATA) return "Risposta del server non valida";
	return "Server Jellyfin non raggiungibile";
}

// Signed in from the answer of AuthenticateByName / AuthenticateWithQuickConnect.
static int jf_take_auth(jv *v)
{
	const char *tok = jv_str(v, "AccessToken");
	jv *user = jv_get(v, "User");
	const char *uid = jv_str(user, "Id");
	if (!tok[0] || !uid[0]) return -1;
	snprintf(jf_token, sizeof(jf_token), "%s", tok);
	snprintf(jf_uid, sizeof(jf_uid), "%s", uid);
	printf("mediacenter: Jellyfin: entrato come %s\n", jv_str(user, "Name"));
	jf_save_cfg();
	return 0;
}

static jv *jf_post_json(const char *path, const char *body, int *err)
{
	return jf_tree(jf_http("POST", path, body, err), err);
}

// User name and password from mediacenter.ini.
static int jf_login_password(const char *user)
{
	const char *pw = getenv("MC_JELLYFIN_PASS");
	char u[256], p[256], body[600];
	jv_quote(u, sizeof(u), user);
	jv_quote(p, sizeof(p), pw ? pw : "");
	snprintf(body, sizeof(body), "{\"Username\":%s,\"Pw\":%s}", u, p);
	int e = 0;
	jv *v = jf_post_json("/Users/AuthenticateByName", body, &e);
	int r = v ? jf_take_auth(v) : -1;
	jf_free(v);
	if (r) jf_message("Jellyfin", e ? jf_err_text(e) : "Utente o password non accettati");
	return r;
}

// Quick Connect: a code shown here, approved in Jellyfin on the computer
// (Profilo > Quick Connect). B gives up.
static int jf_login_quick_connect(void)
{
	int e = 0;
	jv *en = jf_get_json("/QuickConnect/Enabled", &e);
	int enabled = en && en->t == 'b' && en->n;
	jf_free(en);
	if (!enabled)
	{
		const char *lines[] = {
			"Quick Connect e' spento nel server.",
			"Accendilo in Jellyfin: Pannello di controllo",
			"> Generale > Abilita Quick Connect,",
			"oppure scrivi in mediacenter.ini",
			"MC_JELLYFIN_USER e MC_JELLYFIN_PASS.",
		};
		jf_screen("Jellyfin: accesso", lines, 5);
		wait_key();
		return -1;
	}
	jv *init = jf_post_json("/QuickConnect/Initiate", NULL, &e);
	if (!init) init = jf_get_json("/QuickConnect/Initiate", &e); // 10.8
	char secret[128], code[32];
	snprintf(secret, sizeof(secret), "%s", jv_str(init, "Secret"));
	snprintf(code, sizeof(code), "%s", jv_str(init, "Code"));
	jf_free(init);
	if (!secret[0] || !code[0])
	{
		jf_message("Jellyfin: accesso", jf_err_text(e));
		return -1;
	}
	char code_line[64];
	snprintf(code_line, sizeof(code_line), "       Codice:   %s", code);
	const char *lines[] = {
		"Sul computer apri Jellyfin, poi",
		"Profilo > Quick Connect e scrivi:",
		"",
		code_line,
		"",
		"In attesa... (B per annullare)",
	};
	jf_screen("Jellyfin: accesso", lines, 6);
	printf("mediacenter: Jellyfin: Quick Connect, codice %s\n", code);
	char path[256], enc[160];
	jf_urlenc(enc, sizeof(enc), secret);
	double next = now_seconds() + 2.0, until = now_seconds() + 600.0;
	while (!quit_requested && now_seconds() < until)
	{
		int k = input_key(now_seconds());
		if (k == KEY_ESC || k == KEY_BACKSPACE || k == KEY_TAB) return -1;
		usleep(20000);
		if (now_seconds() < next) continue;
		next = now_seconds() + 2.0;
		snprintf(path, sizeof(path), "/QuickConnect/Connect?secret=%s", enc);
		jv *st = jf_get_json(path, &e);
		int ok = jv_num(st, "Authenticated", 0) != 0;
		jf_free(st);
		if (!ok) continue;
		char body[256], q[160];
		jv_quote(q, sizeof(q), secret);
		snprintf(body, sizeof(body), "{\"Secret\":%s}", q);
		jv *v = jf_post_json("/Users/AuthenticateWithQuickConnect", body, &e);
		int r = v ? jf_take_auth(v) : -1;
		jf_free(v);
		if (r) jf_message("Jellyfin: accesso", jf_err_text(e));
		return r;
	}
	return -1;
}

// The server and the account, before the libraries. 0 when ready.
static int jf_connect(void)
{
	jf_server[0] = 0;
	const char *e = getenv("MC_JELLYFIN");
	if (e && *e && strcmp(e, "0"))
	{
		snprintf(jf_server, sizeof(jf_server), "%s%s", strncmp(e, "http", 4) ? "http://" : "", e);
		size_t n = strlen(jf_server);
		while (n && jf_server[n - 1] == '/') jf_server[--n] = 0;
		// only the port missing: Jellyfin's
		if (!strchr(jf_server + 7, ':')) snprintf(jf_server + n, sizeof(jf_server) - n, ":8096");
	}
	jf_token[0] = jf_uid[0] = 0;
	jf_load_cfg();
	jf_make_devid();
	jf_net_init();

	int err = 0;
	if (jf_server[0] && jf_token[0] && jf_uid[0])
	{
		// the key kept from last time: still good? A server found on the
		// network may have changed address since: looked for again.
		for (int tries = 0; tries < 2; tries++)
		{
			jv *me = jf_get_json("/Users/Me", &err);
			if (!me && err == AVERROR_HTTP_NOT_FOUND)
			{
				char p[160];
				snprintf(p, sizeof(p), "/Users/%s", jf_uid);
				me = jf_get_json(p, &err);
			}
			jf_free(me);
			if (me)
			{
				if (tries) jf_save_cfg();
				return 0;
			}
			if (err == AVERROR_HTTP_UNAUTHORIZED || err == AVERROR_HTTP_FORBIDDEN) break;
			char addr[256], name[128];
			if (tries || (e && *e) || jf_discover(addr, sizeof(addr), name, sizeof(name)) || !strcmp(addr, jf_server))
			{
				jf_message("Jellyfin", jf_err_text(err));
				return -1;
			}
			printf("mediacenter: Jellyfin: il server ora e' %s (%s)\n", addr, name);
			snprintf(jf_server, sizeof(jf_server), "%s", addr);
		}
		jf_token[0] = jf_uid[0] = 0;
	}
	if (!jf_server[0])
	{
		const char *lines[] = { "Cerco un server Jellyfin nella rete..." };
		jf_screen("Jellyfin", lines, 1);
		char name[128];
		if (jf_discover(jf_server, sizeof(jf_server), name, sizeof(name)))
		{
			const char *l2[] = {
				"Nessun server Jellyfin trovato.",
				"Controlla che sia acceso, oppure scrivi",
				"in mediacenter.ini il suo indirizzo:",
				"MC_JELLYFIN=http://192.168.0.10:8096",
			};
			jf_screen("Jellyfin", l2, 4);
			wait_key();
			return -1;
		}
		printf("mediacenter: Jellyfin: trovato %s (%s)\n", jf_server, name);
	}
	jv *info = jf_get_json("/System/Info/Public", &err);
	if (!info)
	{
		jf_message("Jellyfin", jf_err_text(err));
		return -1;
	}
	printf("mediacenter: Jellyfin: server %s, versione %s\n", jv_str(info, "ServerName"), jv_str(info, "Version"));
	jf_free(info);
	const char *user = getenv("MC_JELLYFIN_USER");
	if (user && *user) return jf_login_password(user);
	return jf_login_quick_connect();
}

// --- Libraries and titles -------------------------------------------------------

#define JF_MAX 2000

typedef struct
{
	char id[40];
	char name[200];   // as Jellyfin writes it (UTF-8)
	char label[200];  // for the list
	char type[24];    // Movie, Series, Season, Episode, MusicAlbum, Audio, Folder...
	char coll[16];    // a library's kind: movies, tvshows, music...
	char album[160];  // the album or the series, for the audio screen
	int folder;
} jf_item_t;

static jf_item_t *jf_items;
static int jf_count;

static int jf_is_audio(const jf_item_t *it)
{
	return !strcmp(it->type, "Audio");
}

static void jf_fill(jv *list, int add)
{
	if (!add) jf_count = 0;
	jv *items = list && list->t == 'a' ? list : jv_get(list, "Items");
	for (jv *c = items ? items->c : NULL; c && jf_count < JF_MAX; c = c->nx)
	{
		jf_item_t *it = &jf_items[jf_count];
		memset(it, 0, sizeof(*it));
		snprintf(it->id, sizeof(it->id), "%s", jv_str(c, "Id"));
		snprintf(it->name, sizeof(it->name), "%s", jv_str(c, "Name"));
		snprintf(it->type, sizeof(it->type), "%s", jv_str(c, "Type"));
		snprintf(it->coll, sizeof(it->coll), "%s", jv_str(c, "CollectionType"));
		it->folder = jv_num(c, "IsFolder", 0) != 0;
		if (!it->id[0]) continue;
		const char *coll = it->coll;
		// live TV and the like: not files
		if (!strcmp(coll, "livetv") || !strcmp(coll, "books") || !strcmp(coll, "photos")) continue;
		if (!strcmp(it->type, "Photo") || !strcmp(it->type, "Book") || !strcmp(it->type, "PhotoAlbum")) continue;
		char text[260];
		int idx = (int)jv_num(c, "IndexNumber", -1), par = (int)jv_num(c, "ParentIndexNumber", -1);
		int year = (int)jv_num(c, "ProductionYear", 0);
		const char *series = jv_str(c, "SeriesName"), *album = jv_str(c, "Album"), *artist = jv_str(c, "AlbumArtist");
		if (!strcmp(it->type, "Episode"))
		{
			if (add && series[0]) snprintf(text, sizeof(text), "%s %dx%02d  %s", series, par, idx, it->name);
			else if (idx >= 0) snprintf(text, sizeof(text), "%02d  %s", idx, it->name);
			else snprintf(text, sizeof(text), "%s", it->name);
			snprintf(it->album, sizeof(it->album), "%s", series);
		}
		else if (!strcmp(it->type, "Audio"))
		{
			if (idx > 0) snprintf(text, sizeof(text), "%02d  %s", idx, it->name);
			else snprintf(text, sizeof(text), "%s", it->name);
			snprintf(it->album, sizeof(it->album), "%s%s%s", artist, artist[0] && album[0] ? " - " : "", album);
		}
		else if (!strcmp(it->type, "MusicAlbum") && artist[0]) snprintf(text, sizeof(text), "%s - %s", artist, it->name);
		else if ((!strcmp(it->type, "Movie") || !strcmp(it->type, "Series")) && year > 0) snprintf(text, sizeof(text), "%s (%d)", it->name, year);
		else snprintf(text, sizeof(text), "%s", it->name);
		jf_ascii(it->label, sizeof(it->label), text);
		jf_count++;
	}
}

// What a level of the list shows.
typedef struct
{
	char id[40];      // "" the libraries, "resume" Continue watching, else a library or folder
	char coll[16];    // the library's kind, for its first level
	char title[200];  // where we are, for the list's header
	int sel;
} jf_level_t;

static int jf_load_level(const jf_level_t *lv)
{
	char path[768], old[768];
	int err = 0;
	jv *v = NULL;
	if (!lv->id[0])
	{
		// the libraries, after Continue watching when there is something
		jf_count = 0;
		snprintf(path, sizeof(path), "/UserItems/Resume?userId=%s&Limit=1&MediaTypes=Video", jf_uid);
		snprintf(old, sizeof(old), "/Users/%s/Items/Resume?Limit=1&MediaTypes=Video", jf_uid);
		v = jf_get_json2(path, old, &err);
		jv *items = jv_get(v, "Items");
		int resume = items && items->c;
		jf_free(v);
		if (resume)
		{
			jf_item_t *it = &jf_items[jf_count++];
			memset(it, 0, sizeof(*it));
			snprintf(it->id, sizeof(it->id), "resume");
			snprintf(it->label, sizeof(it->label), "Continua a guardare");
			it->folder = 1;
		}
		snprintf(path, sizeof(path), "/UserViews?userId=%s", jf_uid);
		snprintf(old, sizeof(old), "/Users/%s/Views", jf_uid);
		v = jf_get_json2(path, old, &err);
		if (!v) return err ? err : -1;
		jf_fill(v, 1);
		jf_free(v);
		// the libraries' own labels: their names, no added series names
		return 0;
	}
	if (!strcmp(lv->id, "resume"))
	{
		snprintf(path, sizeof(path), "/UserItems/Resume?userId=%s&Limit=200&MediaTypes=Video", jf_uid);
		snprintf(old, sizeof(old), "/Users/%s/Items/Resume?Limit=200&MediaTypes=Video", jf_uid);
		v = jf_get_json2(path, old, &err);
		if (!v) return err ? err : -1;
		jf_count = 0;
		jf_fill(v, 1); // episodes with their series' name
		jf_free(v);
		return 0;
	}
	// a library's first level as Jellyfin's own apps show it, else the
	// folder's children in order (seasons, episodes, tracks)
	const char *q;
	if (!strcmp(lv->coll, "movies")) q = "Recursive=true&IncludeItemTypes=Movie&SortBy=SortName";
	else if (!strcmp(lv->coll, "tvshows")) q = "Recursive=true&IncludeItemTypes=Series&SortBy=SortName";
	else if (!strcmp(lv->coll, "music")) q = "Recursive=true&IncludeItemTypes=MusicAlbum&SortBy=AlbumArtist,SortName";
	else if (!strcmp(lv->coll, "musicvideos")) q = "Recursive=true&IncludeItemTypes=MusicVideo&SortBy=SortName";
	else q = "SortBy=ParentIndexNumber,IndexNumber,SortName";
	snprintf(path, sizeof(path), "/Items?userId=%s&ParentId=%s&%s&SortOrder=Ascending&Limit=%d&Fields=ProductionYear", jf_uid, lv->id, q, JF_MAX);
	snprintf(old, sizeof(old), "/Users/%s/Items?ParentId=%s&%s&SortOrder=Ascending&Limit=%d&Fields=ProductionYear", jf_uid, lv->id, q, JF_MAX);
	v = jf_get_json2(path, old, &err);
	if (!v) return err ? err : -1;
	jf_fill(v, 0);
	jf_free(v);
	return 0;
}

// --- Playing ------------------------------------------------------------------

static const char *const jf_video_ok[] = { "h264", "mpeg2video", "mpeg1video", "mpeg4", "msmpeg4v3", "msmpeg4v2", "msmpeg4v1",
	"msmpeg4", "h263", "mjpeg", "wmv1", "wmv2", "wmv3", "vc1", NULL };
static const char *const jf_audio_ok[] = { "mp3", "mp2", "mp1", "ac3", "eac3", "dts", "dca", "aac", "vorbis", "flac", "alac", "opus",
	"wmav1", "wmav2", "pcm_s16le", "pcm_s16be", "pcm_s24le", "pcm_u8", "pcm_dvd", "pcm_bluray", NULL };
static const char *const jf_cont_ok[] = { "mkv", "matroska", "webm", "mp4", "mov", "m4v", "m4a", "avi", "ts", "mpegts", "m2ts", "mpeg",
	"mpg", "vob", "ps", "asf", "wmv", "wma", "ogg", "ogv", "oga", "mp3", "flac", "wav", "aac", "ac3", "dts", "opus", "mka", NULL };

static int jf_in(const char *s, const char *const *list)
{
	for (int i = 0; list[i]; i++)
		if (!strcasecmp(s, list[i])) return 1;
	return 0;
}

// "mov,mp4,m4a,3gp,3g2,mj2": one of them is enough.
static int jf_container_ok(const char *c)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", c);
	char *save = NULL;
	for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
		if (jf_in(t, jf_cont_ok)) return 1;
	return 0;
}

// Whether the original file plays here as it is; why not in why.
static int jf_direct_ok(jv *src, int audio_item, char *why, size_t wlen)
{
	why[0] = 0;
	const char *cont = jv_str(src, "Container");
	if (cont[0] && !jf_container_ok(cont)) { snprintf(why, wlen, "contenitore %s", cont); return 0; }
	const char *vt = jv_str(src, "VideoType");
	if (vt[0] && strcmp(vt, "VideoFile")) { snprintf(why, wlen, "%s", vt); return 0; }
	jv *streams = jv_get(src, "MediaStreams");
	int audio_any = 0, audio_good = 0, video_bad = 0;
	for (jv *s = streams ? streams->c : NULL; s; s = s->nx)
	{
		const char *type = jv_str(s, "Type"), *codec = jv_str(s, "Codec");
		if (!strcmp(type, "Video") && !audio_item)
		{
			// a cover picture in a music file is not the video
			if (!strcmp(codec, "mjpeg") || !strcmp(codec, "png")) continue;
			int w = (int)jv_num(s, "Width", 0), h = (int)jv_num(s, "Height", 0), depth = (int)jv_num(s, "BitDepth", 8);
			const char *prof = jv_str(s, "Profile");
			if (!jf_in(codec, jf_video_ok)) { snprintf(why, wlen, "video %s", codec); video_bad = 1; }
			else if (w > 1920 || h > 1088) { snprintf(why, wlen, "video %dx%d", w, h); video_bad = 1; }
			else if (depth > 8 || strstr(prof, "10")) { snprintf(why, wlen, "video %s %d bit", codec, depth > 8 ? depth : 10); video_bad = 1; }
			if (video_bad) return 0;
		}
		else if (!strcmp(type, "Audio") && !jv_num(s, "IsExternal", 0))
		{
			audio_any = 1;
			if (jf_in(codec, jf_audio_ok) || !strncmp(codec, "pcm_", 4)) audio_good = 1;
			else if (!why[0]) snprintf(why, wlen, "audio %s", codec);
		}
	}
	if (audio_any && !audio_good) return 0;
	why[0] = 0;
	return 1;
}

// Tells Jellyfin what is playing and where it stopped: it keeps the place
// (Continue watching, also on the computer) and marks what was watched.
static void jf_report(const char *what, const char *id, const char *src_id, const char *session, double pos, int direct)
{
	char body[512];
	snprintf(body, sizeof(body),
		"{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PlaySessionId\":\"%s\",\"PositionTicks\":%lld,\"PlayMethod\":\"%s\",\"CanSeek\":true}",
		id, src_id, session, (long long)(pos * 10000000.0), direct ? "DirectPlay" : "Transcode");
	char path[64];
	snprintf(path, sizeof(path), "/Sessions/Playing%s", what);
	int e = 0;
	free(jf_http("POST", path, body, &e));
	if (e) printf("mediacenter: Jellyfin: resoconto %s non accettato (%d)\n", what[0] ? what : "/Start", e);
}

// Asks whether to start where it was left. -1 back, 0 from the start, 1 resume.
static int jf_ask_resume(const char *title, double ticks)
{
	char t[32], line[64], head[220];
	jf_ticks_text(t, sizeof(t), ticks);
	snprintf(line, sizeof(line), "Riprendi da %s", t);
	jf_ascii(head, sizeof(head), title);
	const char *labels[2] = { line, "Dall'inizio" };
	int sel = 0;
	for (;;)
	{
		draw_browser(head, labels, NULL, 2, sel, NULL);
		int code = wait_key();
		if (!code || quit_requested) return -1;
		code = menu_nav(code, &sel, 2, 1);
		if (!code) continue;
		if (code == KEY_ESC || code == KEY_BACKSPACE || code == KEY_TAB) return -1;
		if (code == KEY_ENTER || code == KEY_PLAY || code == KEY_SPACE) return sel == 0;
	}
}

static int jf_play(const jf_item_t *it, int in_playlist, int ask)
{
	char path[512], old[512];
	int err = 0;
	snprintf(path, sizeof(path), "/Items/%s?userId=%s", it->id, jf_uid);
	snprintf(old, sizeof(old), "/Users/%s/Items/%s", jf_uid, it->id);
	jv *d = jf_get_json2(path, old, &err);
	if (!d)
	{
		jf_message("Jellyfin", jf_err_text(err));
		return END_DONE;
	}
	jv *srcs = jv_get(d, "MediaSources");
	jv *src = srcs && srcs->t == 'a' ? srcs->c : NULL;
	const char *media_type = jv_str(d, "MediaType");
	int audio = !strcmp(media_type, "Audio");
	char src_id[64], why[96], session[40];
	snprintf(src_id, sizeof(src_id), "%s", src ? jv_str(src, "Id") : it->id);
	if (!src_id[0]) snprintf(src_id, sizeof(src_id), "%s", it->id);
	int direct = src ? jf_direct_ok(src, audio, why, sizeof(why)) : 1;
	double resume_ticks = jv_num(jv_get(d, "UserData"), "PlaybackPositionTicks", 0);
	char name[200], series[160];
	snprintf(name, sizeof(name), "%s", jv_str(d, "Name"));
	snprintf(series, sizeof(series), "%s", jv_str(d, "SeriesName"));
	int idx = (int)jv_num(d, "IndexNumber", -1), par = (int)jv_num(d, "ParentIndexNumber", -1);
	jf_free(d);
	unsigned r32 = (unsigned)rand() ^ (unsigned)time(NULL);
	snprintf(session, sizeof(session), "%08x%s", r32, jf_devid + 8);

	char url[1024];
	if (audio)
	{
		if (direct) snprintf(url, sizeof(url), "%s/Audio/%s/stream?static=true&MediaSourceId=%s", jf_server, it->id, src_id);
		else snprintf(url, sizeof(url), "%s/Audio/%s/stream.flac?AudioCodec=flac&MediaSourceId=%s&DeviceId=%s&PlaySessionId=%s",
			jf_server, it->id, src_id, jf_devid, session);
	}
	else if (direct) snprintf(url, sizeof(url), "%s/Videos/%s/stream?static=true&MediaSourceId=%s", jf_server, it->id, src_id);
	else
		// HLS from the server: H.264 up to 1920x1080, 8 bit, AC3 5.1
		snprintf(url, sizeof(url), "%s/Videos/%s/master.m3u8?MediaSourceId=%s&DeviceId=%s&PlaySessionId=%s"
			"&VideoCodec=h264&AudioCodec=ac3&MaxWidth=1920&MaxHeight=1080&MaxVideoBitDepth=8&VideoBitrate=12000000"
			"&AudioBitrate=448000&MaxAudioChannels=6&TranscodingMaxAudioChannels=6&SegmentContainer=ts&BreakOnNonKeyFrames=false",
			jf_server, it->id, src_id, jf_devid, session);
	printf("mediacenter: Jellyfin: %s, %s%s%s\n", name, direct ? "file originale" : "convertito dal server (",
		direct ? "" : why, direct ? "" : ")");

	double start = 0;
	if (ask && !audio && resume_ticks > 10000000.0 * 30)
	{
		int a = jf_ask_resume(name, resume_ticks);
		if (a < 0) return END_BACK;
		if (a) start = resume_ticks / 10000000.0;
	}

	if (boot_wait()) return END_BACK;
	jf_set_opts(&media_opts);
	av_dict_set(&media_opts, "reconnect", "1", 0);
	av_dict_set(&media_opts, "reconnect_on_network_error", "1", 0);
	av_dict_set(&media_opts, "reconnect_delay_max", "5", 0);
	if (media_open(url))
	{
		av_dict_free(&media_opts);
		jf_message("Jellyfin", "Non riesco ad aprire questo titolo");
		return END_DONE;
	}
	if (start > 0) media_seek(start);

	// the title for the controls and the audio screen
	char title[300], disp[600], clean[200], cseries[160];
	if (!strcmp(it->type, "Episode") && series[0]) snprintf(title, sizeof(title), "%s %dx%02d %s", series, par, idx, name);
	else snprintf(title, sizeof(title), "%s", name);
	AVDictionaryEntry *tag = av_dict_get(mc.fmt->metadata, "title", NULL, 0);
	if (!audio || !tag) jf_ascii(mc.title, sizeof(mc.title), title);
	// "Jellyfin: Artist / Album/Name": the audio screen shows the folder and the name
	snprintf(clean, sizeof(clean), "%s", name);
	for (char *p = clean; *p; p++) if (*p == '/') *p = '-';
	snprintf(cseries, sizeof(cseries), "%s", it->album);
	for (char *p = cseries; *p; p++) if (*p == '/') *p = '-';
	snprintf(disp, sizeof(disp), "Jellyfin%s%s/%s", cseries[0] ? ": " : "", cseries, clean);

	jf_report("", it->id, src_id, session, start, direct);
	native_retry();
	native_fb = !mc.audio_only && !getenv("MC_ARMSCALE");
	aui_path = disp;
	int r = media_play(in_playlist);
	// Stop takes the video back to the start: the place it had then
	double pos = stop_pos >= 0 ? stop_pos : mc.clock;
	aui_path = NULL;
	native_fb = 0;
	if (!(mc.audio_only && fb_menu)) fb_native(0, 0, 0);
	media_close();
	av_dict_free(&media_opts);
	jf_report("/Stopped", it->id, src_id, session, pos, direct);
	if (!direct)
	{
		// the conversion on the server stops
		snprintf(path, sizeof(path), "/Videos/ActiveEncodings?DeviceId=%s&PlaySessionId=%s", jf_devid, session);
		free(jf_http("DELETE", path, NULL, &err));
	}
	return r;
}

// The Jellyfin source: libraries, their folders and titles. Returns 1 to
// quit the application.
static int jf_browse(void)
{
	if (jf_connect()) return 0;
	if (!jf_items) jf_items = (jf_item_t *)malloc(sizeof(jf_item_t) * JF_MAX);
	if (!jf_items) return 0;
	static jf_level_t stack[16];
	int depth = 0;
	memset(&stack[0], 0, sizeof(stack[0]));
	snprintf(stack[0].title, sizeof(stack[0].title), "Jellyfin");
	static const char *labels[JF_MAX];
	static int dirs[JF_MAX];
	int reload = 1;

	for (;;)
	{
		jf_level_t *lv = &stack[depth];
		if (reload)
		{
			const char *wait[] = { "Carico l'elenco..." };
			jf_screen(lv->title, wait, 1);
			int e = jf_load_level(lv);
			if (e)
			{
				jf_message(lv->title, jf_err_text(e));
				if (e == AVERROR_HTTP_UNAUTHORIZED)
				{
					// the key was taken back in Jellyfin: sign in again next time
					jf_token[0] = jf_uid[0] = 0;
					jf_save_cfg();
				}
				if (!depth) return 0;
				depth--;
				continue;
			}
			reload = 0;
			for (int i = 0; i < jf_count; i++)
			{
				labels[i] = jf_items[i].label;
				dirs[i] = jf_items[i].folder;
			}
		}
		if (lv->sel >= jf_count) lv->sel = jf_count ? jf_count - 1 : 0;
		draw_browser(lv->title, labels, dirs, jf_count, lv->sel, "Niente da riprodurre qui");
		int top, row_h, page;
		browser_layout(&top, &row_h, &page);
		int code = wait_key();
		if (!code || quit_requested) return 1;
		code = menu_nav(code, &lv->sel, jf_count, page > 1 ? page : 1);
		if (!code) continue;
		if (code == KEY_TAB) return 1;
		if (code == KEY_ESC || code == KEY_BACKSPACE)
		{
			if (!depth) return 0;
			depth--;
			reload = 1;
			continue;
		}
		if ((code != KEY_ENTER && code != KEY_PLAY && code != KEY_SPACE) || !jf_count) continue;
		jf_item_t *it = &jf_items[lv->sel];
		if (it->folder)
		{
			if (depth + 1 >= (int)(sizeof(stack) / sizeof(stack[0]))) continue;
			jf_level_t *nx = &stack[depth + 1];
			memset(nx, 0, sizeof(*nx));
			snprintf(nx->id, sizeof(nx->id), "%s", it->id);
			snprintf(nx->coll, sizeof(nx->coll), "%s", it->coll);
			char lab[200];
			snprintf(lab, sizeof(lab), "%s", it->label);
			snprintf(nx->title, sizeof(nx->title), "%.60s / %.120s", depth ? lv->title : "Jellyfin", lab);
			depth++;
			reload = 1;
			continue;
		}

		// tracks of an album play one after the other, as in a folder
		int i = lv->sel;
		while (i >= 0 && i < jf_count && !quit_requested)
		{
			jf_item_t cur = jf_items[i];
			int audio = jf_is_audio(&cur);
			aui_track = aui_tracks = 0;
			if (audio)
				for (int k = 0; k < jf_count; k++)
					if (jf_is_audio(&jf_items[k]))
					{
						aui_tracks++;
						if (k == i) aui_track = aui_tracks;
					}
			int r = jf_play(&cur, audio, 1);
			aui_track = aui_tracks = 0;
			if (r == END_QUIT) return 1;
			lv->sel = i;
			if (!audio || r == END_BACK || r == END_DONE) break;
			int dir = r == END_PREV ? -1 : 1;
			do i += dir; while (i >= 0 && i < jf_count && !jf_is_audio(&jf_items[i]));
		}
		// Continue watching changes after a video
		if (!strcmp(lv->id, "resume") || !depth) reload = 1;
	}
}
