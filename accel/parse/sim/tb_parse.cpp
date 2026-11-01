// Verilator testbench of acc_parse (step 2 of 4.0): reads the stream of
// "refcmd file -" (slice records and, after each picture, the commands the
// ARM writes today), gives every CABAC slice to acc_parse as the ARM would
// (descriptor + slice data, see the header of acc_parse.sv), models the
// motion memory and compares the words acc_parse writes with the ARM's
// macroblock words (A1, A2 and A4 left out), word for word, every picture.
//
// Usage: refcmd file - | Vacc_parse name   (run where the cab_*.hex files are)
// Env: TBLAT memory read latency (default 8), TBSTALL 1 random stalls on the
// output and the memory, TBDBG 1 dump the macroblock of the first mismatches.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <deque>
#include "Vacc_parse.h"
#include "verilated.h"

static Vacc_parse *top;
static uint64_t clk_count, busy_count;
static std::vector<uint32_t> mem;
static std::deque<std::pair<uint64_t, uint32_t>> rq;   // read data, ready at clock
static int lat = 8, stall = 0;
static std::vector<uint32_t> outw;                     // this picture's words from acc_parse
static uint32_t rnd = 12345;
static uint32_t rand32() { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }

static const uint32_t SLOT_WORDS = 294912;

static void tick()
{
	// inputs for this clock (set before the rising edge)
	top->out_ready = stall ? (rand32() % 4 != 0) : 1;
	bool ack = top->mem_req && (stall ? (rand32() % 3 != 0) : true);
	top->mem_ack = ack;
	if (!rq.empty() && rq.front().first <= clk_count)
	{
		top->mem_rvalid = 1;
		top->mem_rdata = rq.front().second;
		rq.pop_front();
	}
	else
		top->mem_rvalid = 0;
	top->eval();
	// what happens at this edge
	if (top->out_valid && top->out_ready) outw.push_back(top->out_data);
	if (top->mem_req && ack)
	{
		uint32_t a = top->mem_addr;
		if (a >= mem.size()) { fprintf(stderr, "tb: memory address %08x out of range\n", a); exit(1); }
		if (top->mem_we) mem[a] = top->mem_wdata;
		else rq.push_back({clk_count + lat, mem[a]});
	}
	if (top->busy) busy_count++;
	top->clk = 1;
	top->eval();
	top->clk = 0;
	top->eval();
	clk_count++;
}

static int rd32(FILE *f, int32_t *v) { return fread(v, 4, 1, f) == 1; }

int main(int argc, char **argv)
{
	Verilated::commandArgs(argc, argv);
	const char *name = argc > 1 ? argv[1] : "stream";
	if (getenv("TBLAT")) lat = atoi(getenv("TBLAT"));
	if (getenv("TBSTALL")) stall = atoi(getenv("TBSTALL"));
	int dbg = getenv("TBDBG") ? atoi(getenv("TBDBG")) : 0;
	mem.assign((size_t)64 * SLOT_WORDS, 0);
	top = new Vacc_parse;
	top->clk = 0;
	top->reset = 1;
	top->in_valid = 0;
	for (int i = 0; i < 4; i++) tick();
	top->reset = 0;
	tick();

	FILE *f = stdin;
	static char buf[1 << 20];
	setvbuf(f, buf, _IOFBF, sizeof(buf));
	long npics = 0, nslices = 0, nerr = 0, nskip = 0, nslerr = 0, nspat = 0, ntemp = 0, nimpl = 0;
	uint64_t nwords = 0, pic_clk0 = 0, worst = 0, worst_busy = 0, pic_busy0 = 0;
	bool have_pic = false;
	int last_pic = -1;
	std::vector<uint32_t> w;
	int32_t v;
	while (rd32(f, &v))
	{
		if ((uint32_t)v == 0x50494331u)
		{
			// the ARM's commands of the picture just decoded
			uint32_t n;
			if (fread(&n, 4, 1, f) != 1) break;
			std::vector<uint32_t> c(n);
			if (fread(c.data(), 4, n, f) != n) break;
			if (!have_pic) { nskip++; continue; }
			std::vector<uint32_t> ref;
			size_t i = 2;
			while (i < n)
			{
				uint32_t t = c[i] >> 24;
				if (t == 0xA2)
				{
					int n0 = c[i] & 63, n1 = (c[i] >> 6) & 63, two = (c[i] >> 12) & 1;
					i += 1 + 3 * (n0 + (two ? n1 : 0));
				}
				else if (t == 0xA4) i++;
				else if (t == 0xA5 || t == 0xA0) { for (int k = 0; k < 3; k++) ref.push_back(c[i++]); }
				else if (t == 0xA3)
				{
					int type = (c[i] >> 16) & 7;
					size_t m = type == 4 ? 97 : 1 + 1 + (type ? 4 : 36);
					if (type != 4) m += 1 + c[i + m];
					for (size_t k = 0; k < m; k++) ref.push_back(c[i++]);
				}
				else { fprintf(stderr, "tb: unknown word %08x\n", c[i]); exit(1); }
			}
			npics++;
			nwords += ref.size();
			size_t m = std::min(ref.size(), outw.size());
			bool bad = ref.size() != outw.size();
			size_t at = m;
			for (size_t k = 0; k < m; k++) if (ref[k] != outw[k]) { bad = true; at = k; break; }
			if (bad)
			{
				nerr++;
				if (nerr <= 10)
				{
					size_t j = at < outw.size() ? at : (outw.empty() ? 0 : outw.size() - 1);
					while (j > 0 && (ref[j] >> 24) != 0xA3) j--;
					fprintf(stderr, "tb: picture %d, word %zu of %zu (acc_parse %zu words), MB %d,%d +%zu: acc_parse %08x, ARM %08x\n",
					        last_pic, at, ref.size(), outw.size(), j < ref.size() ? ref[j] & 0xFF : -1, j < ref.size() ? (ref[j] >> 8) & 0xFF : -1,
					        at - j, at < outw.size() ? outw[at] : 0, at < ref.size() ? ref[at] : 0);
					if (dbg)
						for (size_t k = j >= 3 ? j - 3 : 0; k < j + 45 && k < m; k++)
							fprintf(stderr, "  %+3ld  %08x  %08x%s\n", (long)k - (long)j, outw[k], ref[k], outw[k] != ref[k] ? "  <" : "");
				}
			}
			uint64_t pc = clk_count - pic_clk0, pb = busy_count - pic_busy0;
			if (pc > worst) worst = pc;
			if (pb > worst_busy) worst_busy = pb;
			outw.clear();
			have_pic = false;
			continue;
		}
		if ((uint32_t)v != 0x534C0001u) { fprintf(stderr, "tb: bad record %08x\n", v); return 1; }
		int32_t h[25];
		if (fread(h, 4, 25, f) != 25) break;
		int32_t slots[2][32], dsf[32], mapc[2][48], impl[32][32], sm4[6][16], sm8[6][64], nb;
		for (int l = 0; l < 2; l++) for (int i = 0; i < 32; i++) rd32(f, &slots[l][i]);
		for (int i = 0; i < 32; i++) rd32(f, &dsf[i]);
		for (int l = 0; l < 2; l++) for (int i = 0; i < 48; i++) rd32(f, &mapc[l][i]);
		for (int a = 0; a < 32; a++) for (int b = 0; b < 32; b++) rd32(f, &impl[a][b]);
		for (int i = 0; i < 6; i++) for (int j = 0; j < 16; j++) rd32(f, &sm4[i][j]);
		for (int i = 0; i < 6; i++) for (int j = 0; j < 64; j++) rd32(f, &sm8[i][j]);
		rd32(f, &nb);
		std::vector<uint8_t> data(((nb + 3) & ~3) + 4, 0);
		if (fread(data.data(), 1, (nb + 3) & ~3, f) != (size_t)((nb + 3) & ~3)) break;
		int pic = h[0], slot = h[1], mbw = h[2], mbh = h[3], x0 = h[4], y0 = h[5], sn = h[6], type = h[7], qp = h[8], idc = h[9],
		    sp = h[10], rc0 = h[11], rc1 = h[12], lc = h[13], t8 = h[14], cip = h[15], dbk = h[16], alpha = h[17], beta = h[18],
		    cq0 = h[19], cq1 = h[20], uw = h[21], d8 = h[22], x264 = h[23], l1l = h[24];
		if (pic != last_pic)
		{
			if (have_pic) { fprintf(stderr, "tb: picture %d has no commands\n", last_pic); return 1; }
			last_pic = pic;
			pic_clk0 = clk_count;
			pic_busy0 = busy_count;
		}
		have_pic = true;
		// the descriptor
		w.clear();
		w.push_back(0x5C000000u | (slot & 255) << 16 | (slots[1][0] & 255) << 8 | (pic & 255));
		w.push_back(mbw | mbh << 8 | x0 << 16 | (uint32_t)y0 << 24);
		w.push_back((sn & 0xFFFF) | qp << 16 | type << 22 | idc << 24 | lc << 26 | dbk << 28 | (uint32_t)uw << 30);
		w.push_back(rc0 | rc1 << 6 | sp << 12 | t8 << 13 | cip << 14 | d8 << 15 | x264 << 16 | l1l << 17 | (cq0 & 31) << 18 | (cq1 & 31) << 23);
		w.push_back((alpha & 255) | (beta & 255) << 8);
		if (type == 1 && !sp)
		{
			for (int i = 0; i < 32; i++) w.push_back(dsf[i] & 0xFFFF);
			for (int l = 0; l < 2; l++) for (int i = 0; i < 32; i++) w.push_back(mapc[l][i] & 255);
		}
		if (uw == 2)
			for (int a = 0; a < rc0; a++) for (int b = 0; b < rc1; b++) w.push_back(impl[a][b] & 0x1FF);
		{
			uint8_t b[96];
			for (int i = 0; i < 6; i++) for (int j = 0; j < 16; j++) b[i * 16 + j] = sm4[i][j];
			for (int i = 0; i < 24; i++) w.push_back(b[4 * i] | b[4 * i + 1] << 8 | b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24);
		}
		if (t8)
		{
			uint8_t b[128];
			for (int j = 0; j < 64; j++) { b[j] = sm8[0][j]; b[64 + j] = sm8[3][j]; }
			for (int i = 0; i < 32; i++) w.push_back(b[4 * i] | b[4 * i + 1] << 8 | b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24);
		}
		w.push_back(nb);
		for (int i = 0; i < (nb + 3) / 4; i++)
			w.push_back((uint32_t)data[4 * i] << 24 | data[4 * i + 1] << 16 | data[4 * i + 2] << 8 | data[4 * i + 3]);
		nslices++;
		if (type == 1) { if (sp) nspat++; else ntemp++; }
		if (uw == 2) nimpl++;
		// feed it, then run until the slice is done
		size_t k = 0;
		bool done = false;
		uint64_t t0 = clk_count;
		while (!done)
		{
			top->in_valid = k < w.size() && !(stall && rand32() % 5 == 0);
			top->in_data = k < w.size() ? w[k] : 0;
			top->eval();
			bool take = top->in_valid && top->in_ready;
			bool sd = top->sl_done, se = top->sl_err;
			tick();
			if (take) k++;
			if (se) { nslerr++; if (nslerr <= 10) fprintf(stderr, "tb: picture %d slice %d: acc_parse reports an error\n", pic, sn); }
			if (sd) done = true;
			if (clk_count - t0 > 400000000ull) { fprintf(stderr, "tb: picture %d slice %d: no end (state %d)\n", pic, sn, 0); return 1; }
		}
		top->in_valid = 0;
		if (k != w.size()) { fprintf(stderr, "tb: picture %d slice %d: %zu of %zu words taken\n", pic, sn, k, w.size()); }
		// the last output words
		for (int i = 0; i < 200; i++) tick();
	}
	printf("%s: Verilog acc_parse %ld immagini, %ld slice, %llu parole, errori %ld (slice con errore %ld); "
	       "%llu colpi (%.1f s a 108 MHz), immagine peggiore %.1f ms, al lavoro %.1f ms\n",
	       name, npics, nslices, (unsigned long long)nwords, nerr, nslerr, (unsigned long long)clk_count,
	       clk_count / 108e6, worst / 108e3, worst_busy / 108e3);
	fprintf(stderr, "%s: slice B diretto spaziale %ld, temporale %ld, pesi impliciti %ld\n", name, nspat, ntemp, nimpl);
	if (nskip) printf("%s: %ld immagini senza slice CABAC\n", name, nskip);
	delete top;
	return nerr != 0 || nslerr != 0;
}
