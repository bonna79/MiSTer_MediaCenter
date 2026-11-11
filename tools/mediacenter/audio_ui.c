// ---------------------------------------------------------------------------
// The audio player screen (mp3, wav, ogg, flac and the other audio files)
//
// While an audio file plays the screen shows its name, folder and format,
// a stereo band with two sine waves (blue the left channel, red the right)
// that grow with the volume and follow the bass and treble of the music,
// the time, the player buttons and a 6-band stereo equalizer.
//
// All of it is drawn by the ARM, 60 times a second, in a picture 720 pixels
// wide and as tall as the CRT mode (576 lines PAL, 480 NTSC; the lists' own
// frame buffer, menu_fb): native on the 15 kHz CRT, enlarged by the scaler
// on HDMI. The picture is drawn in normal (cached) memory and copied to a
// page of the frame buffer the screen is not showing; only the parts that
// move are drawn again and copied (the band, the time, the equalizer).
//
// The equalizer works on the sound sent to the output (6 biquad filters per
// channel, about 1-2% of one ARM core); with all its bands at 0 the sound is
// left exactly as it was. Its settings stay for the next tracks and are kept
// in /media/fat/linux/mediacenter_eq.cfg.
//
// MC_AUDIO_SIMPLE=1 brings back the old screen (title and format only).
// ---------------------------------------------------------------------------

#include "audio_font.h"

static void menu_fb(void);

// Width of the picture: 640 by default, so on the CRT it stays inside the
// tube (user 11-11); MC_AUDIO_W=720 brings back the full width. HDMI and
// CRT show the same frame buffer, so HDMI gets thin black sides too.
#define AUI_WMAX 720
static int aui_w = 640;
#define AUI_W aui_w
#define AUI_HZ 60.0
#define AUI_FFT 1024
#define AUI_RING 131072 // stereo frames of the sound sent, for the waves (2.7 s: more than MC_AUDIO_LEAD_MUSIC)

static struct
{
	int on;               // the screen is in use
	int h;                // lines of the picture (576 or 480)
	uint32_t *bg, *img;   // the still part; the picture shown
	unsigned bg_gen;      // counts the changes of bg
	unsigned page_gen[8]; // bg_gen each page of the frame buffer was last copied with
	int bg_state, bg_flash, bg_eq, bg_confirm;
	double next_t, last_t, t0;
	float lvl[2][6], peak[2][6];
	double phase[12];
	int eq_open, band;
	int saver, sv_min, sv_max, pg_min[8], pg_max[8]; // screen saver: rows drawn now and in each page
	double eq_until;
	int flash;
	double flash_until;
	char folder[256], fmt[96], title[256], file[256];
	// the log
	int frames;
	double draw_sum, draw_max, copy_sum;
} aui;

// The screen saver stays on for the next tracks (A turns it off).
static int aui_saver;
// Sound sent ahead of time for the audio files (seconds), in mediacenter.c.
#define MC_AUDIO_LEAD_MUSIC 2.0

// Position of the file in its folder (1-based, 0 not known) and audio files
// in the folder; set by the browser before each file.
static int aui_track, aui_tracks;
static const char *aui_path;

// ---------------------------------------------------------------------------
// Equalizer
// ---------------------------------------------------------------------------

static const float eq_freq[6] = { 60, 170, 470, 1300, 3600, 10000 };
static const char *const eq_label[6] = { "60", "170", "470", "1,3k", "3,6k", "10k" };
static int eq_gain[6];      // dB, -12..+12
static int eq_vol;          // dB, -5..0: lower volume, so loud tracks do not distort (CRT speakers)
static int eq_loaded;
static float eq_c[6][5];    // b0 b1 b2 a1 a2
static float eq_s[2][6][4]; // x1 x2 y1 y2 per channel and band

#define EQ_FILE "/media/fat/linux/mediacenter_eq.cfg"

// Filter of band b for its gain (Audio EQ Cookbook, R. Bristow-Johnson):
// low shelf at 60 Hz, peaks at 170-3600 Hz, high shelf at 10 kHz.
static void eq_design(int b)
{
	double A = pow(10.0, eq_gain[b] / 40.0), w = 2 * M_PI * eq_freq[b] / MC_AUDIO_RATE;
	double cs = cos(w), sn = sin(w), b0, b1, b2, a0, a1, a2;
	if (b == 0 || b == 5)
	{
		double sq = 2 * sqrt(A) * sn / 2 * sqrt(2.0); // shelf slope 1
		if (b == 0)
		{
			b0 = A * ((A + 1) - (A - 1) * cs + sq);
			b1 = 2 * A * ((A - 1) - (A + 1) * cs);
			b2 = A * ((A + 1) - (A - 1) * cs - sq);
			a0 = (A + 1) + (A - 1) * cs + sq;
			a1 = -2 * ((A - 1) + (A + 1) * cs);
			a2 = (A + 1) + (A - 1) * cs - sq;
		}
		else
		{
			b0 = A * ((A + 1) + (A - 1) * cs + sq);
			b1 = -2 * A * ((A - 1) + (A + 1) * cs);
			b2 = A * ((A + 1) + (A - 1) * cs - sq);
			a0 = (A + 1) - (A - 1) * cs + sq;
			a1 = 2 * ((A - 1) - (A + 1) * cs);
			a2 = (A + 1) - (A - 1) * cs - sq;
		}
	}
	else
	{
		double al = sn / (2 * 1.0); // Q 1
		b0 = 1 + al * A;
		b1 = -2 * cs;
		b2 = 1 - al * A;
		a0 = 1 + al / A;
		a1 = -2 * cs;
		a2 = 1 - al / A;
	}
	eq_c[b][0] = (float)(b0 / a0);
	eq_c[b][1] = (float)(b1 / a0);
	eq_c[b][2] = (float)(b2 / a0);
	eq_c[b][3] = (float)(a1 / a0);
	eq_c[b][4] = (float)(a2 / a0);
}

static void eq_load(void)
{
	if (eq_loaded) return;
	eq_loaded = 1;
	FILE *f = fopen(EQ_FILE, "r");
	if (f)
	{
		int g[6];
		if (fscanf(f, "MC_EQ=%d,%d,%d,%d,%d,%d", &g[0], &g[1], &g[2], &g[3], &g[4], &g[5]) == 6)
			for (int b = 0; b < 6; b++) eq_gain[b] = g[b] < -12 ? -12 : g[b] > 12 ? 12 : g[b];
		int v;
		if (fscanf(f, " MC_VOL=%d", &v) == 1) eq_vol = v < -5 ? -5 : v > 0 ? 0 : v;
		fclose(f);
	}
	for (int b = 0; b < 6; b++) eq_design(b);
	printf("mediacenter: equalizzatore %d %d %d %d %d %d dB, volume %d dB\n", eq_gain[0], eq_gain[1], eq_gain[2], eq_gain[3], eq_gain[4], eq_gain[5], eq_vol);
}

static void eq_save(void)
{
	if (fb_fake) return;
	FILE *f = fopen(EQ_FILE, "w");
	if (!f) return;
	fprintf(f, "MC_EQ=%d,%d,%d,%d,%d,%d\nMC_VOL=%d\n", eq_gain[0], eq_gain[1], eq_gain[2], eq_gain[3], eq_gain[4], eq_gain[5], eq_vol);
	fclose(f);
	printf("mediacenter: equalizzatore salvato %d %d %d %d %d %d dB, volume %d dB\n", eq_gain[0], eq_gain[1], eq_gain[2], eq_gain[3], eq_gain[4], eq_gain[5], eq_vol);
}

static void eq_set(int b, int g)
{
	if (b == 6)
	{
		eq_vol = g < -5 ? -5 : g > 0 ? 0 : g;
		return;
	}
	if (g < -12) g = -12;
	if (g > 12) g = 12;
	if (g == eq_gain[b]) return;
	if (!eq_gain[b]) memset(eq_s[0][b], 0, sizeof(eq_s[0][b])), memset(eq_s[1][b], 0, sizeof(eq_s[1][b]));
	eq_gain[b] = g;
	eq_design(b);
}

// The sound about to be sent (n stereo frames), through the bands not at 0.
static void eq_apply(int16_t *s, int n)
{
	if (eq_vol)
	{
		int m = (int)lrintf(32768 * powf(10, eq_vol / 20.0f));
		for (int i = 0; i < n * 2; i++) s[i] = (int16_t)((s[i] * m) >> 15);
	}
	int any = 0;
	for (int b = 0; b < 6; b++) any |= eq_gain[b];
	if (!any) return;
	for (int ch = 0; ch < 2; ch++)
	{
		for (int b = 0; b < 6; b++)
		{
			if (!eq_gain[b]) continue;
			const float *c = eq_c[b];
			float *st = eq_s[ch][b];
			float x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
			for (int i = 0; i < n; i++)
			{
				// the tiny constant keeps the filter out of denormal numbers
				// (very slow on the ARM) in the silences
				float x = s[i * 2 + ch] + 1e-15f;
				float y = c[0] * x + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;
				x2 = x1;
				x1 = x;
				y2 = y1;
				y1 = y;
				int v = (int)lrintf(y);
				s[i * 2 + ch] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
			}
			st[0] = x1;
			st[1] = x2;
			st[2] = y1;
			st[3] = y2;
		}
	}
}

// ---------------------------------------------------------------------------
// The sound heard, for the waves
// ---------------------------------------------------------------------------

static int16_t vis_ring[AUI_RING * 2];
static unsigned vis_wr;  // stereo frames written
static double vis_end;   // time just after the last frame written

// Called with the sound about to go to the output (mc.pend, n frames from
// time pts): the equalizer, then a copy for the waves.
static void aui_audio(int16_t *s, int n, double pts)
{
	if (!aui.on) return;
	eq_apply(s, n);
	for (int i = 0; i < n; i++)
	{
		unsigned j = vis_wr++ % AUI_RING;
		vis_ring[j * 2] = s[i * 2];
		vis_ring[j * 2 + 1] = s[i * 2 + 1];
	}
	vis_end = pts + n / (double)MC_AUDIO_RATE;
}

static float fft_cos[AUI_FFT / 2], fft_sin[AUI_FFT / 2], fft_win[AUI_FFT];

static void fft(float *re, float *im)
{
	const int n = AUI_FFT;
	for (int i = 1, j = 0; i < n; i++)
	{
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j)
		{
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (int len = 2; len <= n; len <<= 1)
	{
		int step = n / len, half = len / 2;
		for (int i = 0; i < n; i += len)
			for (int k = 0; k < half; k++)
			{
				float wr = fft_cos[k * step], wi = -fft_sin[k * step];
				int a = i + k, b = a + half;
				float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
				re[b] = re[a] - xr;
				im[b] = im[a] - xi;
				re[a] += xr;
				im[a] += xi;
			}
	}
}

// Level (0..1) of the 6 bands of each channel in the sound heard at time t.
static void aui_bands(double t, float out[2][6])
{
	static float re[AUI_FFT], im[AUI_FFT];
	memset(out, 0, sizeof(float) * 12);
	long back = (long)((vis_end - t) * MC_AUDIO_RATE);
	if (back < 0) back = 0;
	if (back + AUI_FFT > AUI_RING - MC_AUDIO_RATE / 10 || vis_wr < (unsigned)(back + AUI_FFT)) return;
	unsigned first = vis_wr - (unsigned)back - AUI_FFT;
	// both channels in one transform: left as the real part, right as the imaginary
	for (int i = 0; i < AUI_FFT; i++)
	{
		unsigned j = (first + i) % AUI_RING;
		re[i] = vis_ring[j * 2] * (fft_win[i] / 32768.0f);
		im[i] = vis_ring[j * 2 + 1] * (fft_win[i] / 32768.0f);
	}
	fft(re, im);
	const double nyq = MC_AUDIO_RATE / 2.0, half = AUI_FFT / 2;
	for (int b = 0; b < 6; b++)
	{
		double lo = b ? sqrt(eq_freq[b - 1] * eq_freq[b]) : 20, hi = b < 5 ? sqrt(eq_freq[b] * eq_freq[b + 1]) : 18000;
		int k0 = (int)floor(lo / nyq * half), k1 = (int)ceil(hi / nyq * half);
		if (k0 < 1) k0 = 1;
		if (k1 > AUI_FFT / 2 - 1) k1 = AUI_FFT / 2 - 1;
		double sl = 0, sr = 0;
		for (int k = k0; k <= k1; k++)
		{
			// X(k) = (Z(k) + conj Z(N-k)) / 2, Y(k) = (Z(k) - conj Z(N-k)) / 2i
			float ar = re[k], ai = im[k], br = re[AUI_FFT - k], bi = im[AUI_FFT - k];
			sl += hypotf(ar + br, ai - bi) * 0.5f;
			sr += hypotf(ai + bi, br - ar) * 0.5f;
		}
		int c = k1 - k0 + 1;
		// amplitude of a sine (Hann window: 4/N), as dB of full scale
		double dl = 20 * log10(sl / c * 4 / AUI_FFT + 1e-9), dr = 20 * log10(sr / c * 4 / AUI_FFT + 1e-9);
		out[0][b] = (float)((dl + 63) / 60);
		out[1][b] = (float)((dr + 63) / 60);
		for (int ch = 0; ch < 2; ch++) out[ch][b] = out[ch][b] < 0 ? 0 : out[ch][b] > 1 ? 1 : out[ch][b];
	}
}

// ---------------------------------------------------------------------------
// Drawing in the picture (AUI_W x aui.h, colours from fb_rgb)
// ---------------------------------------------------------------------------

static uint32_t C_BG, C_PANEL, C_LINE, C_TEXT, C_MUTED, C_DIM, C_L, C_R, C_SEL, C_BTN, C_EQBG, C_EQOPEN, C_PL, C_PR;
static uint32_t *aui_buf; // where the drawing goes: aui.bg or aui.img

static int AY(int y) { return aui.h == 576 ? y : (y * aui.h + 288) / 576; }

static inline uint32_t px_mix(uint32_t d, uint32_t s, int a) // a 0..255
{
	if (a >= 255) return s;
	if (a <= 0) return d;
	uint32_t r = 0xFF000000u;
	for (int sh = 0; sh < 24; sh += 8)
	{
		int dc = (d >> sh) & 255, sc = (s >> sh) & 255;
		r |= (uint32_t)(dc + ((sc - dc) * a + 127) / 255) << sh;
	}
	return r;
}

static inline void px_put(int x, int y, uint32_t c, int a)
{
	if ((unsigned)x >= AUI_W || (unsigned)y >= (unsigned)aui.h) return;
	uint32_t *p = aui_buf + (size_t)y * AUI_W + x;
	*p = px_mix(*p, c, a);
}

static void a_rect(int x, int y, int w, int h, uint32_t c)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > AUI_W) w = AUI_W - x;
	if (y + h > aui.h) h = aui.h - y;
	for (int j = 0; j < h; j++)
	{
		uint32_t *p = aui_buf + (size_t)(y + j) * AUI_W + x;
		for (int i = 0; i < w; i++) p[i] = c;
	}
}

static void a_rect_a(int x, int y, int w, int h, uint32_t c, int a)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) px_put(x + i, y + j, c, a);
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// Rounded rectangle with soft corners.
static void a_round(int x, int y, int w, int h, int r, uint32_t c)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
		{
			float dx = 0, dy = 0;
			if (i < r) dx = r - i - 0.5f;
			else if (i >= w - r) dx = i - (w - r) + 0.5f;
			if (j < r) dy = r - j - 0.5f;
			else if (j >= h - r) dy = j - (h - r) + 0.5f;
			int a = 255;
			if (dx > 0 && dy > 0) a = (int)(255 * clamp01(r + 0.5f - sqrtf(dx * dx + dy * dy)));
			px_put(x + i, y + j, c, a);
		}
}

// Rounded rectangle outline, line lw pixels wide.
static void a_round_line(int x, int y, int w, int h, int r, int lw, uint32_t c)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
		{
			float dx = 0, dy = 0, edge;
			if (i < r) dx = r - i - 0.5f;
			else if (i >= w - r) dx = i - (w - r) + 0.5f;
			if (j < r) dy = r - j - 0.5f;
			else if (j >= h - r) dy = j - (h - r) + 0.5f;
			if (dx > 0 && dy > 0) edge = r - sqrtf(dx * dx + dy * dy);
			else edge = fminf(fminf(i + 0.5f, w - i - 0.5f), fminf(j + 0.5f, h - j - 0.5f));
			if (dx > 0 && dy > 0) edge += 0;
			float a = clamp01(edge + 0.5f) * clamp01(lw + 0.5f - edge);
			if (a > 0) px_put(x + i, y + j, c, (int)(255 * a));
		}
}

// Disc of radius r, filled with fill and edged with a line of width 2.
static void a_disc(float cx, float cy, float r, uint32_t fill, uint32_t edge)
{
	int x0 = (int)(cx - r - 2), x1 = (int)(cx + r + 2), y0 = (int)(cy - r - 2), y1 = (int)(cy + r + 2);
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++)
		{
			float d = hypotf(x + 0.5f - cx, y + 0.5f - cy);
			float fa = clamp01(r + 0.5f - d);
			if (fa > 0) px_put(x, y, fill, (int)(255 * fa));
			if (edge)
			{
				float ea = clamp01(1.5f - fabsf(d - r));
				if (ea > 0) px_put(x, y, edge, (int)(255 * ea));
			}
		}
}

// The icons of the buttons, drawn with 4x4 samples per pixel.
static int icon_hit(int kind, float x, float y, float s)
{
	// triangle pointing dir (1 right, -1 left) centred on cx
#define TRI(cx, dir) ({ float _u = ((x) - (cx)) * (dir); _u >= -s * .45f && _u <= s * .55f && fabsf(y) <= (s * .55f - _u) * .55f / 1.0f; })
	switch (kind)
	{
	case 0: return TRI(s * .08f, 1); // play
	case 1: return fabsf(y) <= s * .55f && ((x >= -s * .45f && x <= -s * .13f) || (x >= s * .13f && x <= s * .45f)); // pause
	case 2: return fabsf(x) <= s * .48f && fabsf(y) <= s * .48f; // stop
	case 3: return TRI(-s * .32f, 1) || TRI(s * .42f, 1); // ff
	case 4: return TRI(s * .32f, -1) || TRI(-s * .42f, -1); // rew
	case 5: return TRI(-s * .1f, 1) || (x >= s * .45f && x <= s * .65f && fabsf(y) <= s * .55f); // next
	case 6: return TRI(s * .1f, -1) || (x >= -s * .65f && x <= -s * .45f && fabsf(y) <= s * .55f); // previous
	}
#undef TRI
	return 0;
}

static void a_icon(int kind, int cx, int cy, float s, uint32_t c)
{
	int r = (int)(s * .8f) + 2;
	for (int y = cy - r; y <= cy + r; y++)
		for (int x = cx - r; x <= cx + r; x++)
		{
			int n = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++)
					n += icon_hit(kind, x + (sx + .5f) / 4 - cx, y + (sy + .5f) / 4 - cy, s);
			if (n) px_put(x, y, c, n * 255 / 16);
		}
}

// UTF-8 to the font's codes (Latin-1; the arrows and the ellipsis on their
// own codes; other characters as '?').
static void aui_latin1(char *dst, size_t len, const char *src)
{
	size_t n = 0;
	const unsigned char *s = (const unsigned char *)src;
	while (*s && n + 1 < len)
	{
		unsigned c = *s++;
		if (c >= 0x80)
		{
			int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
			c &= more == 3 ? 7 : more == 2 ? 15 : 31;
			for (; more && (*s & 0xC0) == 0x80; more--) c = (c << 6) | (*s++ & 63);
			if (c == 0x2026) c = 0x80;
			else if (c == 0x25C4 || c == 0x25C0) c = 0x81;
			else if (c == 0x25BA || c == 0x25B6) c = 0x82;
			else if (c == 0x25B2) c = 0x83;
			else if (c == 0x25BC) c = 0x84;
			else if (c == 0x2018 || c == 0x2019) c = '\'';
			else if (c == 0x201C || c == 0x201D) c = '"';
			else if (c == 0x2013 || c == 0x2014) c = '-';
			else if (c < 0xA0 || c > 0xFF) c = '?';
		}
		dst[n++] = (char)c;
	}
	dst[n] = 0;
}

static int a_text_w(const afont_t *f, const char *s)
{
	int w = 0;
	for (; *s; s++) w += f->g[(unsigned char)*s].adv;
	return w;
}

// Text in Latin-1 codes with its baseline at y; returns the x after it.
static int a_text(const afont_t *f, int x, int y, const char *s, uint32_t c)
{
	for (; *s; s++)
	{
		const afont_glyph_t *g = &f->g[(unsigned char)*s];
		const unsigned char *a = f->a + g->off;
		for (int j = 0; j < g->h; j++)
			for (int i = 0; i < g->w; i++)
				if (a[j * g->w + i]) px_put(x + g->x + i, y + g->y + j, c, a[j * g->w + i]);
		x += g->adv;
	}
	return x;
}

static void a_text_r(const afont_t *f, int x, int y, const char *s, uint32_t c) { a_text(f, x - a_text_w(f, s), y, s, c); }
static void a_text_c(const afont_t *f, int x, int y, const char *s, uint32_t c) { a_text(f, x - a_text_w(f, s) / 2, y, s, c); }

// Cuts s (Latin-1) to w pixels, with an ellipsis.
static void a_fit(const afont_t *f, char *s, int w)
{
	if (a_text_w(f, s) <= w) return;
	int n = (int)strlen(s), ell = f->g[0x80].adv;
	while (n > 1 && a_text_w(f, s) + ell > w) s[--n] = 0;
	s[n] = (char)0x80;
	s[n + 1] = 0;
}

// ---------------------------------------------------------------------------
// The screen
// ---------------------------------------------------------------------------

#define X0 (AUI_W < 720 ? 24 : 40)
#define X1 (AUI_W - X0)
#define CW (X1 - X0)
// band, time bar, equalizer (in lines of the 576-line picture: AY())
#define B_TOP AY(142)
#define B_BOT AY(334)
#define P_Y AY(358)
#define E_X (X0 + (AUI_W < 720 ? 322 : 340))
#define E_TOP AY(408)
#define E_BOT AY(490)

static void aui_colours(void)
{
	C_BG = fb_rgb(0x07, 0x09, 0x0d);
	C_PANEL = fb_rgb(0x0f, 0x13, 0x1b);
	C_LINE = fb_rgb(0x1e, 0x25, 0x32);
	C_TEXT = fb_rgb(0xf2, 0xf4, 0xf8);
	C_MUTED = fb_rgb(0x8a, 0x93, 0xa6);
	C_DIM = fb_rgb(0x4a, 0x52, 0x63);
	C_L = fb_rgb(0x3f, 0xd8, 0xff);
	C_R = fb_rgb(0xff, 0x5a, 0x7a);
	C_SEL = fb_rgb(0xff, 0xd3, 0x4d);
	C_BTN = fb_rgb(0x1b, 0x22, 0x30);
	C_EQBG = fb_rgb(0x18, 0x20, 0x2c);
	C_EQOPEN = fb_rgb(0x12, 0x1a, 0x26);
	C_PL = fb_rgb(0xc6, 0xf3, 0xff);
	C_PR = fb_rgb(0xff, 0xc2, 0xcd);
}

static const char *aui_codec_name(enum AVCodecID id)
{
	switch (id)
	{
	case AV_CODEC_ID_MP3: return "MP3";
	case AV_CODEC_ID_MP2: return "MP2";
	case AV_CODEC_ID_MP1: return "MP1";
	case AV_CODEC_ID_FLAC: return "FLAC";
	case AV_CODEC_ID_VORBIS: return "OGG Vorbis";
	case AV_CODEC_ID_AAC: return "AAC";
	case AV_CODEC_ID_ALAC: return "ALAC";
	case AV_CODEC_ID_AC3: return "AC3";
	case AV_CODEC_ID_EAC3: return "E-AC3";
	case AV_CODEC_ID_DTS: return "DTS";
	case AV_CODEC_ID_WMAV1: case AV_CODEC_ID_WMAV2: return "WMA";
	default: break;
	}
	if (id >= AV_CODEC_ID_PCM_S16LE && id < AV_CODEC_ID_ADPCM_IMA_QT) return "WAV";
	return avcodec_get_name(id);
}

// The texts of the file: folder, format, title, file name.
static void aui_info(const char *path)
{
	char tmp[512];
	// folder: "SD: Musica / Album", "USB: ...", "Rete: ..."
	const char *dir_end = path ? strrchr(path, '/') : NULL;
	tmp[0] = 0;
	if (path && dir_end)
	{
		char dir[512];
		snprintf(dir, sizeof(dir), "%.*s", (int)(dir_end - path), path);
		const char *rest = dir, *label = NULL;
		if (!strncmp(dir, "/media/fat/cifs", 15)) { label = "Rete"; rest = dir + 15; }
		else if (!strncmp(dir, "/media/fat", 10)) { label = "SD"; rest = dir + 10; }
		else if (!strncmp(dir, "/media/usb", 10)) { label = "USB"; rest = dir + 10; while (*rest && *rest != '/') rest++; }
		else if (!strncmp(dir, "/media/", 7)) { label = "Disco"; rest = dir + 7; while (*rest && *rest != '/') rest++; }
		size_t n = 0;
		if (label) n = snprintf(tmp, sizeof(tmp), "%s:", label);
		for (const char *p = rest; *p && n + 4 < sizeof(tmp); p++)
		{
			if (*p == '/')
			{
				if (n && tmp[n - 1] != ':') { tmp[n++] = ' '; tmp[n++] = '/'; }
				tmp[n++] = ' ';
			}
			else tmp[n++] = *p;
		}
		tmp[n] = 0;
	}
	const char *f = tmp;
	while (*f == ' ') f++;
	aui_latin1(aui.folder, sizeof(aui.folder), f);

	// format: "FLAC  44,1 kHz  16 bit  Stereo"
	aui.fmt[0] = 0;
	if (mc.adec)
	{
		AVCodecParameters *cp = mc.fmt->streams[mc.astream]->codecpar;
		int rate = mc.adec->sample_rate, ch = mc.adec->ch_layout.nb_channels;
		char khz[16];
		if (rate % 1000) snprintf(khz, sizeof(khz), "%d,%d", rate / 1000, (rate % 1000) / 100);
		else snprintf(khz, sizeof(khz), "%d", rate / 1000);
		char extra[32] = "";
		int bits = cp->bits_per_raw_sample ? cp->bits_per_raw_sample : cp->bits_per_coded_sample;
		int lossless = cp->codec_id == AV_CODEC_ID_FLAC || cp->codec_id == AV_CODEC_ID_ALAC ||
			(cp->codec_id >= AV_CODEC_ID_PCM_S16LE && cp->codec_id < AV_CODEC_ID_ADPCM_IMA_QT);
		if (lossless && bits) snprintf(extra, sizeof(extra), "  %d bit", bits);
		else if (cp->bit_rate > 0) snprintf(extra, sizeof(extra), "  %d kbit/s", (int)(cp->bit_rate / 1000));
		else if (mc.fmt->bit_rate > 0) snprintf(extra, sizeof(extra), "  %d kbit/s", (int)(mc.fmt->bit_rate / 1000));
		snprintf(tmp, sizeof(tmp), "%s  %s kHz%s  %s", aui_codec_name(cp->codec_id), khz, extra,
			ch == 1 ? "Mono" : ch == 2 ? "Stereo" : "Multicanale");
		aui_latin1(aui.fmt, sizeof(aui.fmt), tmp);
	}

	// file name; the title from the tags, otherwise the name without extension
	const char *base = path ? strrchr(path, '/') : NULL;
	base = base ? base + 1 : (path ? path : mc.title);
	aui_latin1(aui.file, sizeof(aui.file), base);
	AVDictionaryEntry *title = mc.fmt ? av_dict_get(mc.fmt->metadata, "title", NULL, 0) : NULL;
	if (title) aui_latin1(aui.title, sizeof(aui.title), mc.title);
	else
	{
		aui_latin1(aui.title, sizeof(aui.title), base);
		char *dot = strrchr(aui.title, '.');
		if (dot && dot != aui.title) *dot = 0;
	}
}

static int aui_flash_kind(int code)
{
	switch (code)
	{
	case KEY_PLAY: case KEY_PLAYCD: return 1;
	case KEY_PAUSE: case KEY_PAUSECD: case KEY_PLAYPAUSE: return 2;
	case KEY_ESC: case KEY_STOP: case KEY_STOPCD: return 3;
	case KEY_LEFT: case KEY_PREVIOUSSONG: return 4;
	case KEY_RIGHT: case KEY_NEXTSONG: return 5;
	case KEY_PAGEUP: case KEY_REWIND: return 6;
	case KEY_PAGEDOWN: case KEY_FASTFORWARD: return 7;
	}
	return 0;
}

// The still part of the screen: texts, band, buttons, equalizer panel.
static void aui_draw_bg(int state)
{
	aui_buf = aui.bg;
	a_rect(0, 0, AUI_W, aui.h, C_BG);
	char line[300];

	// header: folder and format
	int fw = a_text_w(&afont_text, aui.fmt);
	snprintf(line, sizeof(line), "%s", aui.folder);
	a_fit(&afont_text, line, CW - fw - 24);
	a_text(&afont_text, X0, AY(52), line, C_MUTED);
	a_text_r(&afont_text, X1, AY(52), aui.fmt, C_MUTED);

	// title and track
	snprintf(line, sizeof(line), "%s", aui.title);
	a_fit(&afont_title, line, CW);
	a_text(&afont_title, X0, AY(96), line, C_TEXT);
	if (aui_tracks > 0) snprintf(line, sizeof(line), "Traccia %d di %d   \xb7   %s", aui_track, aui_tracks, aui.file);
	else snprintf(line, sizeof(line), "%s", aui.file);
	a_fit(&afont_text, line, CW);
	a_text(&afont_text, X0, AY(122), line, C_MUTED);

	// stereo band
	int bt = B_TOP, bb = B_BOT, mid = (bt + bb) / 2;
	a_round(X0, bt, CW, bb - bt, 10, C_PANEL);
	a_rect(X0 + 10, mid - 1, CW - 20, 2, C_LINE);
	a_text(&afont_small, X0 + 12, bt + 20, "L", C_L);
	a_text(&afont_small, X0 + 26, bt + 20, "R", C_R);
	if (state == ST_PAUSE || state == ST_STOP)
		a_text_c(&afont_text, AUI_W / 2, mid - 12, state == ST_PAUSE ? "IN PAUSA" : "FERMO", C_MUTED);

	// buttons: previous, rewind, play/pause, stop, forward, next
	static const char *const hint[6] = { "\x81", "L", NULL, "B", "R", "\x82" };
	static const int kind_of[6] = { 6, 4, 0, 2, 3, 5 };
	int ty = AY(440);
	for (int i = 0; i < 6; i++)
	{
		int big = i == 2, r = big ? 24 : 19, cx = X0 + 22 + i * 52 + (big ? 4 : 0);
		int kind = kind_of[i];
		if (big) kind = state == ST_PLAY ? 1 : 0;
		// lit while its key was just pressed, and forward/rewind while running
		static const int flash_of[6] = { 4, 6, 0, 3, 7, 5 };
		int on = (aui.flash && aui.flash == flash_of[i]) || (big && (aui.flash == 1 || aui.flash == 2)) ||
			(i == 1 && state == ST_REW) || (i == 4 && state == ST_FF);
		a_disc(cx, ty, r, on ? C_TEXT : big ? C_BTN : C_PANEL, C_LINE);
		a_icon(kind, cx, ty, big ? 15 : 11, on ? C_BG : C_TEXT);
		const char *h = hint[i] ? hint[i] : (state == ST_PLAY ? "Select" : "Start");
		a_text_c(&afont_small, cx, ty + r + AY(18), h, C_DIM);
	}

	// equalizer panel
	int ex = E_X, ew = X1 - ex, et = E_TOP, eb = E_BOT;
	a_round(ex - 8, et - AY(26), ew + 8, eb - et + AY(64), 8, aui.eq_open ? C_EQOPEN : C_PANEL);
	if (aui.eq_open) a_round_line(ex - 8, et - AY(26), ew + 8, eb - et + AY(64), 8, 2, C_SEL);
	a_text(&afont_small, ex, et - AY(10), aui.eq_open ? "EQUALIZZATORE   \x83\x84 guadagno   \x81\x82 banda" : "EQUALIZZATORE   (Y per regolare)",
		aui.eq_open ? C_SEL : C_MUTED);

	// time bar track
	a_rect(X0 + 62, P_Y - 2, CW - 124, 4, C_LINE);

	// keys
	a_text_c(&afont_small, AUI_W / 2, AY(556), "Start Play  Select Pausa  B Stop  \x81 \x82 Brano  L/R Veloce  Y Equalizzatore  A Salvaschermo  X Esci", C_DIM);
	aui.bg_gen++;
}

// The moving parts, rows and columns: band, time, equalizer.
typedef struct { int x, y, w, h; } arect_t;

static int aui_dyn(arect_t *r)
{
	r[0] = (arect_t){ X0, B_TOP, CW, B_BOT - B_TOP };
	r[1] = (arect_t){ 0, P_Y - 18, AUI_W, 30 };
	r[2] = (arect_t){ E_X - 4, E_TOP - 2, X1 - E_X, E_BOT - E_TOP + AY(36) };
	return 3;
}

// The lines of the waves: full on the line, then a soft halo up to LR
// pixels away, added to what is under them (where two cross they turn
// white). Each column takes the distance across the line from its slope,
// so steep parts keep the same thickness.
#define LR 6
#define LQ 4
typedef struct { uint32_t c[LR * LQ + 1]; } aline_t;
static float glow_i[LR * LQ + 1];
#define SIN_N 4096
static float sin_t[SIN_N];

static void aui_tables(void)
{
	for (int k = 0; k <= LR * LQ; k++)
	{
		float d = (float)k / LQ, v;
		if (d <= 1.0f) v = 1;
		else if (d <= 2.0f) v = 1 - (d - 1) * 0.68f;
		else if (d <= 3.5f) v = 0.32f - (d - 2) * 0.12f;
		else v = 0.14f * (LR - d) / (LR - 3.5f);
		glow_i[k] = v < 0 ? 0 : v;
	}
	for (int i = 0; i < SIN_N; i++) sin_t[i] = sinf(2 * (float)M_PI * i / SIN_N);
}

// sin(2 pi x) from the table
static inline float sin_turn(float x)
{
	return sin_t[(int)((x - floorf(x)) * SIN_N) & (SIN_N - 1)];
}

static void aui_line_lut(aline_t *l, uint32_t col, float gain)
{
	for (int k = 0; k <= LR * LQ; k++)
	{
		float v = glow_i[k] * gain;
		uint32_t o = 0;
		for (int sh = 0; sh < 24; sh += 8)
		{
			int c = (int)(((col >> sh) & 255) * v + 0.5f);
			o |= (uint32_t)(c > 255 ? 255 : c) << sh;
		}
		l->c[k] = o;
	}
}

static inline uint32_t sat_add(uint32_t a, uint32_t b)
{
#if defined(__arm__)
	uint32_t r;
	__asm__("uqadd8 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
	return r;
#else
	uint32_t r = 0;
	for (int sh = 0; sh < 32; sh += 8)
	{
		unsigned v = ((a >> sh) & 255) + ((b >> sh) & 255);
		r |= (v > 255 ? 255u : v) << sh;
	}
	return r;
#endif
}

// Line through ys[k] (row) at column x0 + k, added to aui.img between rows
// ylo and yhi; rows touched widen *rmin..*rmax.
static void aui_line(const float *ys, int x0, int n, const aline_t *l, int ylo, int yhi, int *rmin, int *rmax)
{
	for (int k = 0; k < n; k++)
	{
		float y = ys[k];
		float s = k > 0 && k < n - 1 ? (ys[k + 1] - ys[k - 1]) * 0.5f : k ? y - ys[k - 1] : n > 1 ? ys[1] - y : 0;
		float q = 1 + s * s, inv = 1 / sqrtf(q), r = LR * sqrtf(q);
		if (r > 60) r = 60;
		int a = (int)floorf(y - r), b = (int)ceilf(y + r);
		if (a < ylo) a = ylo;
		if (b > yhi - 1) b = yhi - 1;
		if (a > b) continue;
		if (a < *rmin) *rmin = a;
		if (b > *rmax) *rmax = b;
		uint32_t *p = aui.img + (size_t)a * AUI_W + x0 + k;
		float d = (a + 0.5f - y) * inv * LQ, dd = inv * LQ;
		for (int row = a; row <= b; row++, p += AUI_W, d += dd)
		{
			int i = (int)fabsf(d);
			if (i <= LR * LQ) *p = sat_add(*p, l->c[i]);
		}
	}
}

static void aui_waves(int live, double dt)
{
	static const float NK[6] = { 1.5f, 3, 5, 8, 13, 21 }, SP[6] = { 1.6f, 2.4f, 3.4f, 4.8f, 6.6f, 9.0f };
	static const float WT[6] = { 1.3f, 1.1f, 1, .9f, .8f, .6f };
	int bh = B_BOT - B_TOP;
	float mid = B_TOP + bh / 2.0f, half = bh / 2.0f - 6;
	int n = CW - 16;
	static float env[AUI_WMAX], ys[AUI_WMAX];
	static int env_n;
	static aline_t lut[2];
	static uint32_t lut_col[2];
	if (env_n != n)
	{
		for (int x = 0; x <= n; x++) env[x] = powf(fmaxf(0, sinf((float)M_PI * x / n)), 1.4f);
		env_n = n;
	}
	for (int i = 0; i < 12; i++)
		if (live) aui.phase[i] = fmod(aui.phase[i] + dt * SP[i % 6] * (i >= 6 ? 1.07 : 1), 2 * M_PI * 1000);
	int rmin = 1 << 30, rmax = -1;
	for (int ch = 0; ch < 2; ch++)
	{
		uint32_t col = ch ? C_R : C_L;
		if (lut_col[ch] != col) { aui_line_lut(&lut[ch], col, 1); lut_col[ch] = col; }
		float vol = 0;
		for (int b = 0; b < 6; b++) vol += aui.lvl[ch][b] * WT[b];
		vol = vol / 4.2f > 1 ? 1 : vol / 4.2f;
		float amp = half * (0.35f + 0.9f * vol) / 1.9f;
		float k[6], ph[6];
		for (int b = 0; b < 6; b++)
		{
			k[b] = aui.lvl[ch][b] / (1 + b * 0.35f);
			ph[b] = (float)((-aui.phase[ch * 6 + b] + (ch ? 0.6 * b : 0)) / (2 * M_PI));
		}
		for (int x = 0; x <= n; x++)
		{
			float u = (float)x / n, s = 0;
			for (int b = 0; b < 6; b++)
				if (k[b] > 0.001f) s += k[b] * sin_turn(NK[b] * u + ph[b]);
			ys[x] = mid - env[x] * s * amp;
		}
		aui_line(ys, X0 + 8, n + 1, &lut[ch], B_TOP + 1, B_BOT - 1, &rmin, &rmax);
	}
}

// Screen saver: only bright lines on black, over the whole screen, always
// moving and following the music; nothing stays in place (no marks left on
// a CRT or an LCD). Only the rows with lines are cleared and copied.
#define SV_LINES 5
static void aui_saver_draw(double t, double dt)
{
	static const float NK[6] = { 0.7f, 1.3f, 2.1f, 3.4f, 5.5f, 8.9f }, SP[6] = { 0.9f, 1.3f, 1.9f, 2.6f, 3.5f, 4.6f };
	static aline_t lut[SV_LINES];
	static uint32_t lut_col[SV_LINES];
	static float ys[AUI_WMAX];
	static double ph[SV_LINES][6];
	uint32_t cols[SV_LINES] = { C_L, C_R, fb_rgb(0xa7, 0x7b, 0xff), C_SEL, fb_rgb(0x5a, 0xff, 0xb4) };
	int H = aui.h;

	// clear what the last picture drew
	if (aui.sv_max >= aui.sv_min) memset(aui.img + (size_t)aui.sv_min * AUI_W, 0, (size_t)(aui.sv_max - aui.sv_min + 1) * AUI_W * 4);
	int rmin = 1 << 30, rmax = -1;
	for (int i = 0; i < SV_LINES; i++)
	{
		int ch = i & 1;
		if (lut_col[i] != cols[i]) { aui_line_lut(&lut[i], cols[i], 1); lut_col[i] = cols[i]; }
		// each line follows mostly its own bands: the first the bass, the last the treble
		float w[6], wsum = 0, mix = 0;
		for (int b = 0; b < 6; b++)
		{
			float c = b - i * 1.25f;
			w[b] = expf(-c * c / 2.5f);
			wsum += w[b];
			mix += w[b] * aui.lvl[ch][b];
		}
		mix /= wsum;
		// always moving, faster with the music
		for (int b = 0; b < 6; b++)
			ph[i][b] = fmod(ph[i][b] + dt * SP[b] * (0.8 + 0.12 * i) * (0.35 + 1.4 * mix), 1000.0);
		float yc = H / 2.0f + H * 0.26f * (float)sin(t * 0.05 * (1 + 0.17 * i) + i * 1.7);
		float amp = H * 0.32f * (0.3f + 1.2f * mix);
		float k[6];
		for (int b = 0; b < 6; b++) k[b] = w[b] * (0.12f + aui.lvl[ch][b]) / wsum;
		for (int x = 0; x < AUI_W; x++)
		{
			float u = (float)x / AUI_W, s = 0;
			for (int b = 0; b < 6; b++) s += k[b] * sin_turn(NK[b] * (1 + 0.1f * i) * u - (float)ph[i][b]);
			ys[x] = yc - s * amp * (0.55f + 0.45f * sin_turn(0.5f * u));
		}
		aui_line(ys, 0, AUI_W, &lut[i], 0, H, &rmin, &rmax);
	}
	aui.sv_min = rmin;
	aui.sv_max = rmax;
}

static void aui_mmss(char *s, size_t len, double t)
{
	int v = t > 0 ? (int)t : 0;
	if (v >= 3600) snprintf(s, len, "%d:%02d:%02d", v / 3600, v / 60 % 60, v % 60);
	else snprintf(s, len, "%02d:%02d", v / 60, v % 60);
}

// The moving parts of the picture.
static void aui_draw_dyn(int state, int confirm, double time, double dur, int live, double dt)
{
	aui_buf = aui.img;
	arect_t r[4];
	int n = aui_dyn(r);
	for (int i = 0; i < n; i++)
		for (int y = r[i].y; y < r[i].y + r[i].h; y++)
			memcpy(aui.img + (size_t)y * AUI_W + r[i].x, aui.bg + (size_t)y * AUI_W + r[i].x, (size_t)r[i].w * 4);

	aui_waves(live, dt);

	// time and bar
	char s[32];
	if (dur > 0 && time > dur) time = dur;
	aui_mmss(s, sizeof(s), time);
	a_text(&afont_text, X0, P_Y + 6, s, C_TEXT);
	if (dur > 0) aui_mmss(s, sizeof(s), dur);
	else snprintf(s, sizeof(s), "--:--");
	a_text_r(&afont_text, X1, P_Y + 6, s, C_MUTED);
	int bx = X0 + 62, bw = CW - 124;
	float f = dur > 0 ? (float)(time / dur) : 0;
	f = clamp01(f);
	a_round(bx, P_Y - 2, (int)(bw * f) > 4 ? (int)(bw * f) : 4, 4, 2, C_L);
	a_disc(bx + bw * f, P_Y, 6, C_TEXT, 0);

	// equalizer: level of each band and channel, peak, gain
	int ex = E_X, ew = X1 - ex, et = E_TOP, eb = E_BOT, h = eb - et;
	float cw = ew / 7.0f, zero = (et + eb) / 2.0f;
	for (int b = 0; b < 6; b++)
	{
		int cx = (int)(ex + b * cw + cw / 2), sel = aui.eq_open && aui.band == b;
		for (int ch = 0; ch < 2; ch++)
		{
			int x = cx - 10 + ch * 11, lh = (int)(h * aui.lvl[ch][b]), ph = (int)(h * aui.peak[ch][b]);
			a_rect(x, et, 8, h, C_EQBG);
			a_rect_a(x, eb - lh, 8, lh, ch ? C_R : C_L, 217);
			a_rect(x, eb - ph - 2, 8, 2, ch ? C_PR : C_PL);
		}
		int gy = (int)lrintf(zero - eq_gain[b] / 12.0f * h / 2);
		a_rect(cx - 14, gy - 1, 28, 3, sel ? C_SEL : C_TEXT);
		a_text_c(&afont_small, cx, eb + AY(16), eq_label[b], sel ? C_SEL : C_MUTED);
		if (aui.eq_open || eq_gain[b])
		{
			snprintf(s, sizeof(s), "%s%d", eq_gain[b] > 0 ? "+" : "", eq_gain[b]);
			a_text_c(&afont_small, cx, eb + AY(30), s, sel ? C_SEL : C_DIM);
		}
	}
	a_rect_a(ex, (int)zero, (int)(6 * cw) - 2, 1, C_TEXT, 31);
	{
		// 7th column: volume, 0 dB at the top, -5 dB at the bottom
		int cx = (int)(ex + 6 * cw + cw / 2), sel = aui.eq_open && aui.band == 6;
		a_rect(cx - 4, et, 8, h, C_EQBG);
		int gy = et + (int)lrintf(-eq_vol * h / 5.0f);
		a_rect(cx - 14, gy - 1, 28, 3, sel ? C_SEL : C_TEXT);
		a_text_c(&afont_small, cx, eb + AY(16), "Vol", sel ? C_SEL : C_MUTED);
		if (aui.eq_open || eq_vol)
		{
			snprintf(s, sizeof(s), "%d", eq_vol);
			a_text_c(&afont_small, cx, eb + AY(30), s, sel ? C_SEL : C_DIM);
		}
	}

	// the exit question over the band
	if (confirm)
	{
		const char *q1 = "Tornare all'elenco?", *q2 = "A conferma     altro tasto annulla";
		int w = a_text_w(&afont_title, q1);
		if (a_text_w(&afont_text, q2) > w) w = a_text_w(&afont_text, q2);
		w += 48;
		int bh2 = AY(96), bx2 = (AUI_W - w) / 2, by2 = (B_TOP + B_BOT) / 2 - bh2 / 2;
		a_round(bx2, by2, w, bh2, 10, C_BTN);
		a_round_line(bx2, by2, w, bh2, 10, 2, C_SEL);
		a_text_c(&afont_title, AUI_W / 2, by2 + AY(44), q1, C_TEXT);
		a_text_c(&afont_text, AUI_W / 2, by2 + AY(76), q2, C_MUTED);
	}
	(void)state;
}

// Copies the picture to the page of the frame buffer being drawn (called by
// fb_present_paged): all of it when the page has an older still part,
// otherwise only the moving parts.
static void aui_copy(void)
{
	if (!fb_mem || !aui.img) return;
	int full = aui.page_gen[fb_page & 7] != aui.bg_gen;
	aui.page_gen[fb_page & 7] = aui.bg_gen;
	// the picture narrower than the frame buffer: black around it
	if (full && (fb_w > AUI_W || fb_h != aui.h))
		for (int y = 0; y < fb_h; y++) memset(fb_mem + (size_t)y * fb_stride, 0, (size_t)fb_w * 4);
	if (fb_h * 2 == aui.h)
	{
		// 288p / 240p: two lines of the picture into one
		for (int y = 0; y < fb_h; y++)
		{
			uint32_t *d = (uint32_t *)(fb_mem + (size_t)y * fb_stride);
			const uint32_t *a = aui.img + (size_t)(2 * y) * AUI_W, *b = a + AUI_W;
			int w = fb_w < AUI_W ? fb_w : AUI_W;
			if (fb_w > AUI_W) d += (fb_w - AUI_W) / 2;
			for (int x = 0; x < w; x++) d[x] = ((a[x] >> 1) & 0x7F7F7F7Fu) + ((b[x] >> 1) & 0x7F7F7F7Fu) + (a[x] & b[x] & 0x01010101u);
		}
		return;
	}
	int ox = fb_w > AUI_W ? (fb_w - AUI_W) / 2 : 0, oy = fb_h > aui.h ? (fb_h - aui.h) / 2 : 0;
	int w = fb_w < AUI_W ? fb_w : AUI_W, h = fb_h < aui.h ? fb_h : aui.h;
	arect_t r[4];
	int n;
	int pg = fb_page & 7;
	if (full)
	{
		r[0] = (arect_t){ 0, 0, w, h };
		n = 1;
	}
	else if (aui.saver)
	{
		// the rows with lines now and those this page still shows from before
		int a = aui.sv_min < aui.pg_min[pg] ? aui.sv_min : aui.pg_min[pg];
		int b = aui.sv_max > aui.pg_max[pg] ? aui.sv_max : aui.pg_max[pg];
		r[0] = (arect_t){ 0, a, w, b - a + 1 };
		n = b >= a ? 1 : 0;
	}
	else n = aui_dyn(r);
	aui.pg_min[pg] = aui.saver ? aui.sv_min : 1 << 30;
	aui.pg_max[pg] = aui.saver ? aui.sv_max : -1;
	if (full && aui.saver) { aui.pg_min[pg] = aui.sv_min; aui.pg_max[pg] = aui.sv_max; }
	for (int i = 0; i < n; i++)
	{
		int x0 = r[i].x, x1 = r[i].x + r[i].w;
		if (x1 > w) x1 = w;
		for (int y = r[i].y; y < r[i].y + r[i].h && y < h; y++)
			memcpy(fb_mem + (size_t)(y + oy) * fb_stride + (size_t)(x0 + ox) * 4, aui.img + (size_t)y * AUI_W + x0, (size_t)(x1 - x0) * 4);
	}
}

static void aui_end(void);

// Start of an audio file: the screen in the lists' frame buffer.
static int aui_begin(void)
{
	aui_end();
	const char *e = getenv("MC_AUDIO_SIMPLE");
	if (e && atoi(e)) return 0;
	menu_fb();
	if (!fb_mem) return 0;
	aui.h = (fb_h == 480 || fb_h == 240) ? 480 : 576;
	const char *we = getenv("MC_AUDIO_W");
	aui_w = we && atoi(we) >= 720 ? 720 : 640;
	size_t px = (size_t)AUI_W * aui.h;
	aui.bg = (uint32_t *)malloc(px * 4);
	aui.img = (uint32_t *)malloc(px * 4);
	if (!aui.bg || !aui.img)
	{
		aui_end();
		return 0;
	}
	if (!fft_win[AUI_FFT / 2])
	{
		for (int i = 0; i < AUI_FFT; i++) fft_win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / AUI_FFT);
		for (int i = 0; i < AUI_FFT / 2; i++)
		{
			fft_cos[i] = cosf(2 * (float)M_PI * i / AUI_FFT);
			fft_sin[i] = sinf(2 * (float)M_PI * i / AUI_FFT);
		}
		aui_tables();
	}
	eq_load();
	memset(eq_s, 0, sizeof(eq_s));
	aui_colours();
	aui_info(aui_path);
	vis_wr = 0;
	vis_end = 0;
	memset(aui.lvl, 0, sizeof(aui.lvl));
	memset(aui.peak, 0, sizeof(aui.peak));
	memset(aui.page_gen, 0xFF, sizeof(aui.page_gen));
	aui.eq_open = 0;
	aui.flash = 0;
	aui.bg_state = -1;
	aui.saver = -1; // set at the first picture
	aui.sv_min = 1 << 30;
	aui.sv_max = -1;
	for (int i = 0; i < 8; i++) { aui.pg_min[i] = 1 << 30; aui.pg_max[i] = -1; }
	aui.frames = 0;
	aui.draw_sum = aui.draw_max = aui.copy_sum = 0;
	audio_lead = MC_AUDIO_LEAD_MUSIC;
	// a short pipe to aplay: the pause stops the sound sooner
	if (audio_pipe && fcntl(fileno(audio_pipe), F_SETPIPE_SZ, 16384) < 0) fprintf(stderr, "mediacenter: pipe size not set\n");
	aui.t0 = aui.last_t = aui.next_t = now_seconds();
	aui.on = 1;
	fb_present_hook = aui_copy;
	fprintf(stderr, "mediacenter: schermata audio %dx%d (frame buffer %dx%d)\n", AUI_W, aui.h, fb_w, fb_h);
	return 1;
}

static void aui_end(void)
{
	if (aui.on && aui.frames)
	{
		double s = aui.last_t - aui.t0;
		printf("mediacenter: schermata audio: %d immagini in %.0f s (%.1f al secondo), disegno e copia medio %.1f ms (copia %.1f), peggiore %.1f ms%s\n",
			aui.frames, s, s > 0 ? aui.frames / s : 0, 1000 * aui.draw_sum / aui.frames, 1000 * aui.copy_sum / aui.frames,
			1000 * aui.draw_max, aui_saver ? ", salvaschermo" : "");
	}
	if (aui.on && aui.eq_open) eq_save();
	aui.on = 0;
	fb_present_hook = NULL;
	free(aui.bg);
	free(aui.img);
	aui.bg = aui.img = NULL;
	audio_lead = MC_AUDIO_LEAD;
}

// A key during an audio file: 1 when the screen used it (the equalizer).
static int aui_key(int code, double t)
{
	if (!aui.on) return 0;
	// A: the screen saver on or off
	if (code == KEY_ENTER)
	{
		if (aui.eq_open) { aui.eq_open = 0; eq_save(); }
		aui_saver = !aui_saver;
		printf("mediacenter: salvaschermo %s\n", aui_saver ? "acceso" : "spento");
		return 1;
	}
	// X (the exit question) shows the normal screen
	if (code == KEY_TAB && aui_saver) aui_saver = 0;
	if (code == KEY_SPACE || code == KEY_E)
	{
		aui_saver = 0;
		aui.eq_open = !aui.eq_open;
		aui.eq_until = t + 10;
		if (!aui.eq_open) eq_save();
		return 1;
	}
	if (aui.eq_open)
	{
		switch (code)
		{
		case KEY_LEFT: aui.band = (aui.band + 6) % 7; break;
		case KEY_RIGHT: aui.band = (aui.band + 1) % 7; break;
		case KEY_UP: eq_set(aui.band, (aui.band == 6 ? eq_vol : eq_gain[aui.band]) + 1); break;
		case KEY_DOWN: eq_set(aui.band, (aui.band == 6 ? eq_vol : eq_gain[aui.band]) - 1); break;
		case KEY_ESC: aui.eq_open = 0; eq_save(); break;
		default: aui.eq_open = 0; eq_save(); goto pass; // another key closes it and does its job
		}
		aui.eq_until = t + 10;
		return 1;
	}
pass:
	{
		int k = aui_flash_kind(code);
		if (k)
		{
			aui.flash = k;
			aui.flash_until = t + 0.26;
		}
	}
	return 0;
}

// Pause of an audio file: the sound sent ahead (MC_AUDIO_LEAD_MUSIC) is
// thrown away and the file goes back to the moment heard, so the sound
// stops at once and starts again from there.
static void aui_pause(void)
{
	if (!aui.on) return;
	double h = mc.clock;
	if (audio_pipe && !isnan(sent_end)) h = sent_end - audio_queued();
	media_seek(h);
}

// An audio file left with a key (next or previous track, back to the list):
// the sound still queued is thrown away and aplay is stopped at once
// (SIGTERM: it drops its buffer), instead of playing it to the end.
static void aui_cut_sound(void)
{
	if (!audio_pipe) return;
	audio_flush();
	int me = getpid(), n = 0;
	DIR *d = opendir("/proc");
	if (!d) return;
	for (struct dirent *de; (de = readdir(d));)
	{
		int pid = atoi(de->d_name), ppid = 0, pp2 = 0;
		char name[20], pname[20];
		if (pid <= 1 || pid == me || !proc_name(pid, name, sizeof(name), &ppid) || strcmp(name, "aplay")) continue;
		if (ppid != me && !(proc_name(ppid, pname, sizeof(pname), &pp2) && pp2 == me)) continue;
		if (!kill(pid, SIGTERM)) n++;
	}
	closedir(d);
	printf("mediacenter: suono del brano fermato subito (%d)\n", n);
}

// B (stop) on an audio file: the sound already sent (pipe, aplay and the
// MiSTer output, about half a second) is dropped at once. aplay is stopped
// and started again with a quarter second of silence, which restarts the
// output like a new track does.
static void aui_stop_sound(void)
{
	if (!aui.on || !audio_pipe) return;
	aui_cut_sound();
	audio_close();
	if (audio_open(MC_AUDIO_RATE)) return;
	if (fcntl(fileno(audio_pipe), F_SETPIPE_SZ, 16384) < 0) fprintf(stderr, "mediacenter: pipe size not set\n");
	pthread_mutex_lock(&audio_lock);
	for (int i = 0; i < MC_AUDIO_RATE / 4; i++)
	{
		unsigned j = audio_wr % AUDIO_RING;
		audio_ring[j * 2] = audio_ring[j * 2 + 1] = 0;
		audio_wr++;
	}
	pthread_cond_signal(&audio_cond);
	pthread_mutex_unlock(&audio_lock);
	printf("mediacenter: stop, uscita audio azzerata con silenzio\n");
}

// Back to the list from an audio file: after aplay was stopped, the MiSTer
// sound output still played about a second of the track (seen 11-10). A
// new short stream of silence restarts the output, which drops that rest
// (when the next track starts, its own stream does the same).
static void aui_silence(void)
{
	FILE *f = popen("aplay -q -t raw -f S16_LE -c 2 -r 48000 --buffer-time=200000 --period-time=50000 - 2>/dev/null", "w");
	if (!f) return;
	static const int16_t zero[2400 * 2];
	for (int i = 0; i < 5; i++) fwrite(zero, 4, 2400, f); // 0.25 s
	pclose(f);
	printf("mediacenter: uscita audio azzerata con silenzio\n");
}

// Called at every round of the player: draws and shows the next picture
// when it is due (AUI_HZ a second).
static void aui_tick(double t, int state, int confirm, double time, double dur, double heard)
{
	if (!aui.on || t < aui.next_t) return;
	aui.next_t += 1.0 / AUI_HZ;
	if (aui.next_t < t) aui.next_t = t + 1.0 / AUI_HZ; // late: no catching up
	double dt = t - aui.last_t;
	if (dt > 0.1) dt = 0.1;
	aui.last_t = t;
	double t0 = now_seconds();

	if (aui.eq_open && t > aui.eq_until) { aui.eq_open = 0; eq_save(); }
	if (aui.flash && t > aui.flash_until) aui.flash = 0;

	// band levels of the sound heard now
	int live = state == ST_PLAY;
	float raw[2][6];
	if (live) aui_bands(heard, raw);
	else memset(raw, 0, sizeof(raw));
	for (int ch = 0; ch < 2; ch++)
		for (int b = 0; b < 6; b++)
		{
			float v = raw[ch][b], *l = &aui.lvl[ch][b];
			float k = (float)dt * (v > *l ? 22 : 7);
			*l += (v - *l) * (k > 1 ? 1 : k);
			float p = aui.peak[ch][b] - (float)dt * 0.5f;
			aui.peak[ch][b] = *l > p ? *l : p;
		}

	if (aui_saver != aui.saver)
	{
		// every page is copied whole once after the switch
		aui.saver = aui_saver;
		aui.bg_gen++;
		aui.bg_state = -1;
		if (aui.saver)
		{
			memset(aui.img, 0, (size_t)AUI_W * aui.h * 4);
			aui.sv_min = 1 << 30;
			aui.sv_max = -1;
		}
	}
	double tc;
	if (aui.saver)
	{
		aui_saver_draw(t, dt);
		tc = now_seconds();
		fb_present_paged();
		goto done;
	}
	if (state != aui.bg_state || aui.flash != aui.bg_flash || aui.eq_open != aui.bg_eq)
	{
		aui.bg_state = state;
		aui.bg_flash = aui.flash;
		aui.bg_eq = aui.eq_open;
		aui_draw_bg(state);
		memcpy(aui.img, aui.bg, (size_t)AUI_W * aui.h * 4);
	}
	aui_draw_dyn(state, confirm, time, dur, live, dt);
	tc = now_seconds();
	fb_present_paged();

done:;
	double d = now_seconds() - t0;
	aui.copy_sum += now_seconds() - tc;
	aui.frames++;
	aui.draw_sum += d;
	if (d > aui.draw_max) aui.draw_max = d;
}

#undef X0
#undef X1
#undef CW
#undef B_TOP
#undef B_BOT
#undef P_Y
#undef E_X
#undef E_TOP
#undef E_BOT
#undef LR
#undef LQ
