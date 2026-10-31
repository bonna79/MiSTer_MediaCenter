// Independent C model of the CABAC block (step 1 of 4.0), written from the
// H.264 standard (9.3): it reads a cabtrace trace on stdin, decodes every bin
// itself with codIRange / codIOffset and checks it against FFmpeg's, and
// decodes every residual block on its own (significance map, levels, signs)
// and checks the coefficients. The contexts start from the init tables
// (m, n) and the slice QP. I_PCM: the byte where the samples start is
// worked out from the bit position and checked against FFmpeg's.
// Usage: cabtrace file | cabcheck [name]
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "cab_tables.h"

static uint8_t *data;
static unsigned nbytes;
static uint64_t bitpos;           // next bit to read, from the slice data start
static unsigned range, offset;
static uint8_t st[1024];          // pStateIdx << 1 | valMPS
static long nbins, nres, ncoef, npcm, nslices, nskip, npics, errors;
static long slice_no;

static int rdbit(void)
{
	unsigned b = bitpos < 8ull * nbytes ? (data[bitpos >> 3] >> (7 - (bitpos & 7))) & 1 : 0;
	bitpos++;
	return b;
}

static void engine_init(void)
{
	range = 510;
	offset = 0;
	for (int i = 0; i < 9; i++) offset = offset << 1 | rdbit();
}

static void ctx_init(int type, int idc, int qp)
{
	int set = type == 2 ? 0 : 1 + idc;
	int q = qp < 0 ? 0 : qp > 51 ? 51 : qp;
	for (int i = 0; i < 1024; i++)
	{
		int m = cab_init[set][i][0], n = cab_init[set][i][1];
		int pre = ((m * q) >> 4) + n;
		pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
		st[i] = pre <= 63 ? (uint8_t)((63 - pre) << 1) : (uint8_t)(((pre - 64) << 1) | 1);
	}
}

static int dec(int ctx)
{
	int p = st[ctx] >> 1, mps = st[ctx] & 1, bin;
	unsigned rlps = cab_rlps[p][(range >> 6) & 3];
	range -= rlps;
	if (offset >= range)
	{
		bin = !mps;
		offset -= range;
		range = rlps;
		if (p == 0) mps = !mps;
		p = cab_tlps[p];
	}
	else
	{
		bin = mps;
		p = cab_tmps[p];
	}
	st[ctx] = (uint8_t)(p << 1 | mps);
	while (range < 256)
	{
		range <<= 1;
		offset = offset << 1 | rdbit();
	}
	nbins++;
	return bin;
}

static int byp(void)
{
	offset = offset << 1 | rdbit();
	nbins++;
	if (offset >= range) { offset -= range; return 1; }
	return 0;
}

static int term(void)
{
	range -= 2;
	nbins++;
	if (offset >= range) return 1;
	while (range < 256)
	{
		range <<= 1;
		offset = offset << 1 | rdbit();
	}
	return 0;
}

// residual block of category cat (4:2:0, frame macroblocks); coded_block_flag
// is already decoded (a normal decision in the trace). Writes the coefficients
// last first, as FFmpeg's trace does.
static int residual(int cat, int *pos, int *lev)
{
	static const int maxc[6] = { 16, 15, 16, 4, 15, 64 };
	static const int sigb[6] = { 105 + 0, 105 + 15, 105 + 29, 105 + 44, 105 + 47, 402 };
	static const int lastb[6] = { 166 + 0, 166 + 15, 166 + 29, 166 + 44, 166 + 47, 417 };
	static const int absb[6] = { 227 + 0, 227 + 10, 227 + 20, 227 + 30, 227 + 39, 426 };
	int m = maxc[cat], n = 0, idx[64];
	int i;
	for (i = 0; i < m - 1; i++)
	{
		int si = cat == 5 ? cab_sig8[i] : cat == 3 ? (i < 2 ? i : 2) : i;
		int li = cat == 5 ? cab_last8[i] : cat == 3 ? (i < 2 ? i : 2) : i;
		if (dec(sigb[cat] + si))
		{
			idx[n++] = i;
			if (dec(lastb[cat] + li)) break;
		}
	}
	if (i == m - 1) idx[n++] = m - 1;
	int gt1 = 0, eq1 = 0;
	for (int k = n - 1, o = 0; k >= 0; k--, o++)
	{
		int inc = gt1 ? 0 : (1 + eq1 < 4 ? 1 + eq1 : 4);
		unsigned a;
		if (!dec(absb[cat] + inc)) a = 1;
		else
		{
			int inc2 = 5 + (gt1 < 4 - (cat == 3) ? gt1 : 4 - (cat == 3));
			unsigned pre = 1;
			while (pre < 14 && dec(absb[cat] + inc2)) pre++;
			a = pre + 1;
			if (pre == 14)
			{
				// suffix: Exp-Golomb k = 0, bypass bins
				unsigned k0 = 0, suf = 0;
				while (byp()) { suf += 1u << k0; k0++; if (k0 > 30) return -1; }
				while (k0--) suf += (unsigned)byp() << k0;
				a += suf;
			}
		}
		if (a == 1) eq1++; else gt1++;
		int s = byp();
		pos[o] = idx[k];
		lev[o] = s ? -(int)a : (int)a;
	}
	return n;
}

static int rw(FILE *f, unsigned *w)
{
	uint16_t v;
	if (fread(&v, 2, 1, f) != 1) return 0;
	*w = v;
	return 1;
}

static void fail(const char *what, long a, long b)
{
	errors++;
	if (errors <= 20)
		fprintf(stderr, "cabcheck: ERRORE slice %ld, bin %ld: %s (modello %ld, FFmpeg %ld)\n", slice_no, nbins, what, a, b);
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "trace";
	FILE *f = stdin;
	static char ibuf[1 << 20];
	setvbuf(f, ibuf, _IOFBF, sizeof(ibuf));
	unsigned w;
	int active = 0;               // the slice is one the block takes (CABAC, 4:2:0, 8 bit, frame)
	while (rw(f, &w))
	{
		if (w == 0x3000)
		{
			unsigned ty, idc, qp, fl, lo, hi;
			rw(f, &ty); rw(f, &idc); rw(f, &qp); rw(f, &fl); rw(f, &lo); rw(f, &hi);
			nbytes = lo | hi << 16;
			free(data);
			data = malloc(nbytes + 2);
			if (fread(data, 1, nbytes + (nbytes & 1), f) != nbytes + (nbytes & 1)) { fprintf(stderr, "cabcheck: troncato\n"); break; }
			slice_no = nslices++;
			active = !(fl & 15);
			if (!active) { nskip++; continue; }
			bitpos = 0;
			ctx_init((int)ty, (int)idc, (int)qp);
			engine_init();
		}
		else if (w == 0x3F00) { rw(f, &w); npics++; }
		else if (w == 0x3100)
		{
			unsigned lo, hi;
			rw(f, &lo); rw(f, &hi);
			if (!active) continue;
			// after the terminate bin (1): the engine has read 9 bits ahead of
			// the arithmetic code; the samples start at the next byte boundary
			uint64_t b = bitpos;
			long at = (long)((b + 7) >> 3);
			if (at != (long)(lo | hi << 16)) fail("I_PCM inizio", at, (long)(lo | hi << 16));
			bitpos = 8ull * ((lo | hi << 16) + 384);
			engine_init();
			npcm++;
		}
		else if ((w & 0xF000) == 0x2000)
		{
			int cat = (w >> 7) & 31, n = w & 127, pos[64], lev[64];
			int fp[64], fv[64];
			for (int k = 0; k < n; k++)
			{
				unsigned p, lo, hi;
				rw(f, &p); rw(f, &lo); rw(f, &hi);
				fp[k] = (int)p; fv[k] = (int)(int32_t)(lo | hi << 16);
			}
			if (!active) continue;
			if (cat > 5) { fail("categoria", cat, cat); continue; }
			int m = residual(cat, pos, lev);
			nres++;
			ncoef += n;
			if (m != n) { fail("numero coefficienti", m, n); continue; }
			for (int k = 0; k < n; k++)
			{
				if (pos[k] != fp[k]) fail("posizione", pos[k], fp[k]);
				if (lev[k] != fv[k]) fail("valore", lev[k], fv[k]);
			}
		}
		else if (w < 0x0800)
		{
			if (!active) continue;
			int b = dec((int)(w & 0x3FF));
			if (b != (int)((w >> 10) & 1)) fail("bin di contesto", b, (w >> 10) & 1);
		}
		else if ((w & 0xFFFE) == 0x0800)
		{
			if (!active) continue;
			int b = byp();
			if (b != (int)(w & 1)) fail("bin bypass", b, w & 1);
		}
		else if ((w & 0xFFFE) == 0x0802)
		{
			if (!active) continue;
			int b = term();
			if (b != (int)(w & 1)) { fail("bin terminate", b, w & 1); if (errors <= 3) fprintf(stderr, "  bitpos %llu di %u, range %u offset %u, ultimi byte %02x %02x %02x\n", (unsigned long long)bitpos, 8 * nbytes, range, offset, data[nbytes-3], data[nbytes-2], data[nbytes-1]); }
		}
		else { fail("parola sconosciuta", w, w); break; }
	}
	printf("%s: %ld pacchetti, %ld slice (%ld saltate), %ld bin, %ld blocchi residui, %ld coefficienti, %ld I_PCM, errori %ld\n",
		name, npics, nslices, nskip, nbins, nres, ncoef, npcm, errors);
	return errors != 0;
}
