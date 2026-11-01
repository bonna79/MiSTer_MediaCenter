// Verilator test bench of cab_dec.sv: reads a cabtrace trace on stdin and
// drives the block with the same commands; every bin, every coefficient and
// every I_PCM start and its 384 sample bytes must match FFmpeg's. Also counts the clocks, per packet
// (picture) too, to see the time at 108 MHz.
// Usage: cabtrace file | tb_cab [name]
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include "Vcab_dec.h"
#include "verilated.h"

static Vcab_dec *d;
static uint64_t clk_count, busy_count;  // all clocks / clocks the engine is working
static std::vector<uint32_t> words;  // the slice data
static size_t wp;                    // next word to give
static long errors, nbins, nres, ncoef, npcm, nslices, nskip, npics;
static long slice_no;

static void tick()
{
	// a word for the block when it wants one
	d->in_valid = wp < words.size() ? 1 : 0;
	d->in_data = d->in_valid ? words[wp] : 0;
	d->clk = 0;
	d->eval();
	bool took = d->in_valid && d->in_ready;
	if (d->busy) busy_count++;
	d->clk = 1;
	d->eval();
	if (took) wp++;
	clk_count++;
}

static void fail(const char *what, long a, long b)
{
	errors++;
	if (errors <= 20)
		fprintf(stderr, "tb_cab: ERRORE slice %ld, bin %ld: %s (Verilog %ld, FFmpeg %ld)\n", slice_no, nbins, what, a, b);
}

// gives a command and waits until it is taken
static void command(int op, int arg)
{
	d->cmd_valid = 1;
	d->cmd_op = op;
	d->cmd_arg = arg;
	for (;;)
	{
		d->clk = 0;
		d->eval();
		bool ready = d->cmd_ready;
		tick();
		if (ready) break;
		if (clk_count > (1ull << 62)) break;
	}
	d->cmd_valid = 0;
}

// waits for a result of kind k
static bool result(int k, long *pos, long *val)
{
	for (long n = 0; n < 100000; n++)
	{
		if (d->out_valid)
		{
			if (d->out_kind != k) { fail("tipo di risultato", d->out_kind, k); return false; }
			*pos = d->out_pos;
			*val = (int32_t)d->out_val;
			tick();
			return true;
		}
		tick();
	}
	fail("nessun risultato", 0, k);
	return false;
}

static int rw(unsigned *w)
{
	uint16_t v;
	if (fread(&v, 2, 1, stdin) != 1) return 0;
	*w = v;
	return 1;
}

int main(int argc, char **argv)
{
	Verilated::commandArgs(argc, argv);
	const char *name = argc > 1 ? argv[1] : "trace";
	static char ibuf[1 << 20];
	setvbuf(stdin, ibuf, _IOFBF, sizeof(ibuf));
	d = new Vcab_dec;
	d->reset = 1;
	d->cmd_valid = 0;
	for (int i = 0; i < 4; i++) tick();
	d->reset = 0;
	tick();
	unsigned w;
	int active = 0;
	uint64_t pkt_clk = clk_count, max_pkt = 0, max_pkt_bins = 0, pkt_busy = 0, max_busy = 0;
	long pkt_bins = 0;
	std::vector<uint8_t> data;
	while (rw(&w))
	{
		if (w == 0x3000)
		{
			unsigned ty, idc, qp, fl, lo, hi;
			rw(&ty); rw(&idc); rw(&qp); rw(&fl); rw(&lo); rw(&hi);
			unsigned nb = lo | hi << 16;
			data.assign(nb + (nb & 1), 0);
			if (fread(data.data(), 1, data.size(), stdin) != data.size()) { fprintf(stderr, "tb_cab: troncato\n"); break; }
			slice_no = nslices++;
			active = !(fl & 15);
			if (!active) { nskip++; continue; }
			// the words of this slice, given after the INIT is taken
			std::vector<uint32_t> nw((nb + 3) / 4 + 2, 0);
			for (unsigned i = 0; i < nb; i++) nw[i / 4] |= (uint32_t)data[i] << (24 - 8 * (i % 4));
			d->cmd_valid = 1;
			d->cmd_op = 0;
			d->cmd_arg = (ty & 3) | (idc & 3) << 2 | (qp & 63) << 4;
			for (;;)
			{
				d->clk = 0;
				d->eval();
				bool ready = d->cmd_ready;
				if (ready)
				{
					// the old slice's word given in this clock is dropped by the block
					tick();
					words.swap(nw);
					wp = 0;
					break;
				}
				tick();
			}
			d->cmd_valid = 0;
		}
		else if (w == 0x3F00)
		{
			rw(&w);
			uint64_t c = clk_count - pkt_clk;
			if (npics && c > max_pkt) { max_pkt = c; max_pkt_bins = pkt_bins; }
			if (npics && busy_count - pkt_busy > max_busy) max_busy = busy_count - pkt_busy;
			pkt_clk = clk_count;
			pkt_busy = busy_count;
			pkt_bins = 0;
			npics++;
		}
		else if (w == 0x3100)
		{
			unsigned lo, hi;
			rw(&lo); rw(&hi);
			if (!active) continue;
			command(5, 0);
			long p, v;
			unsigned at = lo | hi << 16;
			if (result(3, &p, &v) && (p != 0 || v != (long)at)) fail("I_PCM inizio", v, at);
			// then the 384 sample bytes
			for (unsigned i = 0; i < 384; i++)
			{
				if (!result(3, &p, &v)) break;
				long want = at + i < data.size() ? data[at + i] : -1;
				if (p != 1 || v != want) { fail("I_PCM campione", v, want); break; }
			}
			npcm++;
		}
		else if ((w & 0xF000) == 0x2000)
		{
			int cat = (w >> 7) & 31, n = w & 127;
			std::vector<long> fp(n), fv(n);
			for (int k = 0; k < n; k++)
			{
				unsigned p, lo, hi;
				rw(&p); rw(&lo); rw(&hi);
				fp[k] = p; fv[k] = (int32_t)(lo | hi << 16);
			}
			if (!active) continue;
			command(4, cat);
			nres++;
			ncoef += n;
			for (int k = 0;; k++)
			{
				long p, v;
				// a coefficient or the end of the block
				long t;
				for (t = 0; t < 100000 && !d->out_valid; t++) tick();
				if (!d->out_valid) { fail("blocco senza fine", 0, 0); break; }
				if (d->out_kind == 2)
				{
					if ((long)d->out_pos != n) fail("numero coefficienti", d->out_pos, n);
					tick();
					break;
				}
				result(1, &p, &v);
				if (k >= n) { fail("coefficiente in piu'", p, -1); continue; }
				if (p != fp[k]) fail("posizione", p, fp[k]);
				if (v != fv[k]) fail("valore", v, fv[k]);
			}
		}
		else if (w < 0x0800 || (w & 0xFFFC) == 0x0800)
		{
			if (!active) continue;
			int op = w < 0x0800 ? 1 : (w & 2) ? 3 : 2;
			command(op, w & 0x3FF);
			long p, v;
			if (result(0, &p, &v) && v != (long)((w < 0x0800 ? w >> 10 : w) & 1))
				fail(op == 1 ? "bin di contesto" : op == 2 ? "bin bypass" : "bin terminate", v, (w < 0x0800 ? w >> 10 : w) & 1);
			nbins++;
			pkt_bins++;
		}
		else { fail("parola sconosciuta", w, w); break; }
		if (errors > 1000) break;
	}
	printf("%s: Verilog %ld pacchetti, %ld slice (%ld saltate), %ld bin fuori dai blocchi, %ld blocchi residui, %ld coefficienti, %ld I_PCM, errori %ld; "
		"%llu colpi di clock (%.1f s a 108 MHz), pacchetto peggiore %llu colpi (%.1f ms); blocco al lavoro %llu colpi (%.1f s), pacchetto peggiore al lavoro %.1f ms\n",
		name, npics, nslices, nskip, nbins, nres, ncoef, npcm, errors,
		(unsigned long long)clk_count, clk_count / 108e6, (unsigned long long)max_pkt, max_pkt / 108e3,
		(unsigned long long)busy_count, busy_count / 108e6, max_busy / 108e3);
	delete d;
	return errors != 0;
}
