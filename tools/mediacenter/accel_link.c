// MiSTer MediaCenter FPGA accelerator: the link with the menu core and the
// display of its pictures (included by mediacenter.c after zerocopy.c, and
// by accel/sim/acctest.c on the PC with ACC_FAKE, against a pretend core).
//
// The commands of each picture (accel.c) go into a ring of 4 MB that the
// core's mc_acc block reads (fpga/menu/rtl/mc_acc.sv describes the mailbox).
// The ring is either memory of the player (locked pages, written through a
// second mapping that skips the caches: MC_ACC_RING=wc, the default) or 4 MB
// outside Linux just below the picture slots (MC_ACC_RING=so: no caches to
// worry about, but each store waits for the memory). The core rebuilds every
// picture into its slot (0x30000000 + slot * 4 MB) and says how many pictures
// it finished; the zero-copy block then copies the slot into the frame
// buffer, as it does with the decoder's own pictures.
//
// The ARM's writes to the ring pass the L2 cache controller (PL310), whose
// store buffer may keep them (erratum 769419): after each piece of ring the
// player asks it to drain (its Cache Sync register) before moving the write
// pointer.

#include <sys/mman.h>

#define ACC_MBOX       0x23E00000u   // mc_acc's mailbox
#define ACC_MBOX_LEN   0x2000u       // the mailbox page and the ring page table
#define ACC_MTABLE     1024          // the ring page table, in 32-bit words from the mailbox
#define ACC_MAGIC      0x4341434Du   // "MCAC"
#define ACC_OKAY       0x4B4F4341u   // "ACOK"
#ifndef ACC_RING_PAGES
#define ACC_RING_PAGES 1024          // 4 MB
#endif
#define ACC_RING_SO    0x2FC00000u   // MC_ACC_RING=so
#define ACC_SLOT_BASE  0x30000000u
#define ACC_SLOT_SIZE  0x400000u
#define ACC_Y_STRIDE   2048          // a slot: Y rows of 2048 bytes, U and V rows of 1024
#define ACC_C_STRIDE   1024
#define ACC_U_OFF      (278528u * 8)
#define ACC_V_OFF      (348160u * 8)
#define ACC_SLOT_PAGES ((ACC_V_OFF + 544u * ACC_C_STRIDE + 4095) / 4096)
#define ACC_ZC_TABLES  0xC0000u      // page tables of the slots, in the zero-copy mailbox (4 KB each)
#define ACC_CHUNK      65536         // the write pointer moves at least this often
#define ACC_L2_REGS    0xFFFEF000u   // PL310
#define ACC_L2_SYNC    (0x730 / 4)

static struct
{
	int fd, pagemap;
	volatile uint32_t *mb;     // the mailbox
	volatile uint32_t *l2;     // the L2 cache controller
	uint8_t *ring;             // the ring, as the ARM writes it
	size_t ring_len;
	void *pool;                // MC_ACC_RING=wc: the locked pages behind it
	int so;
	int up;                    // ring ready, session started
	int pinned;
	uint32_t wp;               // bytes written into the ring in this session
	uint32_t fsess;            // the core's session
	unsigned fbase;            // picture number (decoding order) of the session's first picture
	// diag
	unsigned shown, late, lost, ring_waits, late_row;
	double ring_wait_t, wait_t;
	uint32_t last_pics;
	unsigned fp_n;
	double fp_since, fp_starve, fp_mwait;
	struct { unsigned shown, late, lost, ring_waits, sent, fp_n; uint64_t words; double send, wait, fp_since, fp_starve, fp_mwait, start; } w;
} al = { .fd = -1, .pagemap = -1 };

static void al_barrier(void)
{
#if defined(__arm__)
	__asm__ volatile("dsb sy" ::: "memory");
#else
	__sync_synchronize();
#endif
}

// Everything written so far is in the memory.
static void al_drain(void)
{
	al_barrier();
	if (al.l2)
	{
		al.l2[ACC_L2_SYNC] = 0;
		for (int i = 0; i < 100000 && (al.l2[ACC_L2_SYNC] & 1); i++);
		al_barrier();
	}
}

#ifdef ACC_FAKE
void *acc_fake_map(uint32_t phys, size_t len, void *at);
#endif

static void *al_map(uint32_t phys, size_t len, void *at)
{
#ifdef ACC_FAKE
	return acc_fake_map(phys, len, at);
#else
	void *m = mmap(at, len, PROT_READ | PROT_WRITE, MAP_SHARED | (at ? MAP_FIXED : 0), al.fd, phys);
	return m == MAP_FAILED ? NULL : m;
#endif
}

// Pushes everything out of the caches: L1 to L2 (cacheflush), then 32 MB of
// other memory through the 512 KB L2.
static void al_evict(void *p, size_t len)
{
	__builtin___clear_cache((char *)p, (char *)p + len);
	size_t n = 32u << 20;
	uint8_t *b = (uint8_t *)malloc(n);
	if (!b) return;
	memset(b, 1, n);
	volatile uint32_t sum = 0;
	for (size_t i = 0; i < n; i += 32) sum += b[i];
	free(b);
}

static void al_ring_free(void)
{
#ifndef ACC_FAKE
	if (al.ring) munmap(al.ring, al.ring_len);
	if (al.pool) munmap(al.pool, al.ring_len);
#endif
	al.ring = NULL;
	al.pool = NULL;
}

// The ring in the player's own memory, written without the caches.
static int al_ring_wc(char *why, size_t why_len)
{
	const size_t len = (size_t)ACC_RING_PAGES * 4096;
	al.ring_len = len;
	uint64_t *pfn = NULL;
	al.pool = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED | MAP_POPULATE, -1, 0);
	if (al.pool == MAP_FAILED)
	{
		al.pool = NULL;
		snprintf(why, why_len, "no locked memory for the ring");
		goto fail;
	}
	pfn = (uint64_t *)calloc(ACC_RING_PAGES, sizeof(uint64_t));
	if (al.pagemap < 0) al.pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (!pfn || al.pagemap < 0 ||
		pread(al.pagemap, pfn, ACC_RING_PAGES * 8, (off_t)((uintptr_t)al.pool / 4096) * 8) != ACC_RING_PAGES * 8)
	{
		snprintf(why, why_len, "the ring's pages are unknown (pagemap)");
		goto fail;
	}
	for (int i = 0; i < ACC_RING_PAGES; i++)
	{
		if (!(pfn[i] >> 63) || (pfn[i] & ((1ULL << 55) - 1)) >= (1u << 20))
		{
			snprintf(why, why_len, "a page of the ring is not in memory");
			goto fail;
		}
		pfn[i] &= (1ULL << 55) - 1;
	}
	// the same pages again, one after the other and without the caches
	void *va = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (va == MAP_FAILED)
	{
		snprintf(why, why_len, "no address space for the ring");
		goto fail;
	}
	al.ring = (uint8_t *)va;
	for (int i = 0; i < ACC_RING_PAGES;)
	{
		int j = i + 1;
		while (j < ACC_RING_PAGES && pfn[j] == pfn[j - 1] + 1) j++;
		if (!al_map((uint32_t)(pfn[i] * 4096), (size_t)(j - i) * 4096, al.ring + (size_t)i * 4096))
		{
			snprintf(why, why_len, "the ring cannot be mapped (/dev/mem)");
			goto fail;
		}
		i = j;
	}
	// Linux cleared the pages through the caches: those lines must leave the
	// caches before the core reads the ring, or they would overwrite it later.
	// Checked: a pattern written without the caches survives a second pass.
	al_evict(al.pool, len);
	uint32_t *r = (uint32_t *)al.ring;
	for (size_t i = 0; i < len / 4; i++) r[i] = (uint32_t)i * 2654435761u;
	al_drain();
	al_evict(al.pool, len);
	for (size_t i = 0; i < len / 4; i++)
		if (r[i] != (uint32_t)i * 2654435761u)
		{
			snprintf(why, why_len, "the ring check failed at byte %zu", i * 4);
			goto fail;
		}
	for (int i = 0; i < ACC_RING_PAGES; i++) al.mb[ACC_MTABLE + i] = (uint32_t)pfn[i];
	free(pfn);
	return 0;
fail:
	free(pfn);
	al_ring_free();
	return -1;
}

// The ring outside Linux, written without the caches.
static int al_ring_so(char *why, size_t why_len)
{
	al.ring_len = (size_t)ACC_RING_PAGES * 4096;
	al.ring = (uint8_t *)al_map(ACC_RING_SO, al.ring_len, NULL);
	if (!al.ring)
	{
		snprintf(why, why_len, "the ring cannot be mapped (/dev/mem)");
		return -1;
	}
	for (int i = 0; i < ACC_RING_PAGES; i++) al.mb[ACC_MTABLE + i] = ACC_RING_SO / 4096 + (uint32_t)i;
	return 0;
}

// A new session of the core: it resets the engine and starts reading the
// ring from its start. A core without the accelerator never answers.
static int al_session(void)
{
	volatile uint32_t *m = al.mb;
	for (int tries = 0; tries < 2; tries++)
	{
		// the core takes the mailbox's first session as one it has seen: a
		// core just loaded may need a second one
		uint32_t s = m[1] + 1;
		if (!s) s = 1;
		m[2] = ACC_MBOX + ACC_MTABLE * 4;
		m[3] = ACC_RING_PAGES;
		m[4] = 0;
		m[0] = ACC_MAGIC;
		al_drain();
		m[1] = s;
		al_drain();
		double t0 = acc_now();
		while (acc_now() - t0 < 0.3)
		{
			// the first answer of the session: nothing read, no pictures
			if (m[16] == ACC_OKAY && m[17] == s && m[18] == 0 && m[19] == 0)
			{
				al.fsess = s;
				al.wp = 0;
				al.last_pics = 0;
				al.late_row = 0;
				return 0;
			}
			usleep(250);
		}
	}
	return -1;
}

// Bytes of the ring the core has read in this session.
static uint32_t al_read_done(void)
{
	volatile uint32_t *m = al.mb;
	if (m[16] != ACC_OKAY || m[17] != al.fsess) return al.wp - (uint32_t)al.ring_len; // no room
	return m[18];
}

static void al_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
	if (!al.so)
	{
		memcpy(dst, src, n);
		return;
	}
	// strongly ordered memory: aligned 64-bit stores only
	volatile uint64_t *d = (volatile uint64_t *)dst;
	const uint64_t *s = (const uint64_t *)src;
	for (size_t i = 0; i < n / 8; i++) d[i] = s[i];
}

// acc.send: a finished picture's commands into the ring (acc.lock held).
static int al_send(const uint32_t *w, size_t n, unsigned seq)
{
	(void)seq;
	const uint8_t *src = (const uint8_t *)w;
	size_t bytes = n * 4;
	double t0 = 0;
	while (bytes)
	{
		uint32_t used = al.wp - al_read_done();
		if (used > al.ring_len) used = (uint32_t)al.ring_len;
		size_t room = al.ring_len - used;
		if (room < 8)
		{
			double t = acc_now();
			if (!t0)
			{
				t0 = t;
				al.ring_waits++;
			}
			if (t - t0 > 1.0 || (al.mb[20] & 3)) return -1;
			usleep(200);
			continue;
		}
		size_t off = al.wp % al.ring_len;
		size_t k = bytes;
		if (k > room) k = room;
		if (k > al.ring_len - off) k = al.ring_len - off;
		if (k > ACC_CHUNK) k = ACC_CHUNK;
		al_copy(al.ring + off, src, k);
		al_drain();
		al.wp += (uint32_t)k;
		al.mb[4] = al.wp;
		src += k;
		bytes -= k;
	}
	if (t0) al.ring_wait_t += acc_now() - t0;
	return 0;
}

// Is the picture of b rebuilt? 1 yes, 0 not after a wait of half a second,
// -1 never (it is from before a seek, or the core stopped).
static int al_wait(const acc_buf_t *b)
{
	volatile uint32_t *m = al.mb;
	if (!al.up || b->session != acc.session || acc.failed) return -1;
	uint32_t need = b->seq - al.fbase + 1;
	double t0 = acc_now();
	for (;;)
	{
		if (m[16] == ACC_OKAY && m[17] == al.fsess)
		{
			uint32_t st = m[20], pics = m[19];
			if (st & 3)
			{
				acc_fail(NULL, st & 2 ? "the FPGA memory does not answer" : "the FPGA engine got an unexpected command");
				return -1;
			}
			if (pics != al.last_pics)
			{
				// timings of the last picture the core finished
				al.last_pics = pics;
				al.fp_n++;
				al.fp_since += m[21];
				al.fp_starve += m[22];
				al.fp_mwait += m[23];
			}
			if ((int32_t)(pics - need) >= 0) return 1;
		}
		if (acc_now() - t0 > 0.5) return 0;
		usleep(200);
	}
}

// Starts the link for a file (the decoder is not running yet). 0 when the
// core answered.
static int al_open(char *why, size_t why_len)
{
	why[0] = 0;
	if (al.fd < 0)
	{
#ifndef ACC_FAKE
		al.fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
		if (al.fd < 0)
		{
			snprintf(why, why_len, "/dev/mem cannot be opened");
			return -1;
		}
#else
		al.fd = 0;
#endif
	}
	if (!al.mb)
	{
		al.mb = (volatile uint32_t *)al_map(ACC_MBOX, ACC_MBOX_LEN, NULL);
		if (!al.mb)
		{
			snprintf(why, why_len, "the mailbox cannot be mapped (/dev/mem)");
			return -1;
		}
	}
#if defined(__arm__) && !defined(ACC_FAKE)
	if (!al.l2) al.l2 = (volatile uint32_t *)al_map(ACC_L2_REGS, 4096, NULL);
#endif
	const char *e = getenv("MC_ACC_RING");
	al.so = e && !strcmp(e, "so");
#ifdef ACC_FAKE
	al.so = 1;
#endif
	if (al.so ? al_ring_so(why, why_len) : al_ring_wc(why, why_len)) return -1;
	if (al_session())
	{
		snprintf(why, why_len, "the core does not answer (a core without the accelerator)");
		// the core may still look: the mailbox says nobody is there
		al.mb[0] = 0;
		al_ring_free();
		return -1;
	}
	al.fbase = acc.next_send;
	al.up = 1;
	memset(&al.w, 0, sizeof(al.w));
	al.shown = al.late = al.lost = al.ring_waits = 0;
	al.ring_wait_t = al.wait_t = 0;
	al.fp_n = 0;
	al.fp_since = al.fp_starve = al.fp_mwait = 0;
	return 0;
}

// After a seek (the decoder flushed, its threads idle): pictures still on
// their way are dropped and the core starts again.
static void al_restart(void)
{
	acc_restart();
	if (!al.up) return;
	if (al_session()) acc_fail(NULL, "the core does not answer after a seek");
	al.fbase = acc.next_send;
}

// The file ends (the decoder and all its pictures are gone).
static void al_close(void)
{
	if (al.mb && al.up)
	{
		al.mb[0] = 0; // the core stops
		al_drain();
	}
	al.up = 0;
	al_ring_free();
}

#ifndef ACC_FAKE
// ---------------------------------------------------------------- the player

// Turns the accelerator on for a new video decoder when it can do the
// stream: H.264, 8-bit 4:2:0, 1920 wide, drawn with half its lines by the
// zero-copy block (light mode). MC_NOACC=1 turns it off.
static void acc_try(AVCodecContext *ctx, const AVStream *st)
{
	const char *e = getenv("MC_NOACC");
	if (ctx->codec_id != AV_CODEC_ID_H264 || st->codecpar->format != AV_PIX_FMT_YUV420P || st->codecpar->width != 1920 ||
		!half_lines || fb_fake || zc_get_buffer2 || acc_file_off || (e && atoi(e))) return;
	char why[96];
	if (zcp_map())
	{
		printf("mediacenter: diag acceleratore: spento (zero-copy non disponibile)\n");
		return;
	}
	zcp_pin();
	al.pinned = 1;
	double t0 = now_seconds();
	if (al_open(why, sizeof(why)))
	{
		printf("mediacenter: diag acceleratore: spento (%s)\n", why);
		al.pinned = 0;
		zcp_unpin();
		return;
	}
	// a page table for each slot, for the zero-copy block
	volatile uint32_t *t = zcp_mb + ACC_ZC_TABLES / 4;
	for (int s = 0; s < ACC_SLOTS; s++)
		for (unsigned p = 0; p < ACC_SLOT_PAGES; p++) t[s * 1024 + p] = (ACC_SLOT_BASE + (uint32_t)s * ACC_SLOT_SIZE) / 4096 + p;
	acc.send = al_send;
	acc_attach(ctx);
	memset(&zd, 0, sizeof(zd));
	printf("mediacenter: diag acceleratore: acceso (anello %s, svuotamento L2 %s, sessione %u, pronto in %.0f ms)\n",
		al.so ? "fuori da Linux" : "nella memoria del lettore", al.l2 ? "si" : "NO", (unsigned)al.fsess, 1000 * (now_seconds() - t0));
}

// After the decoder and all its pictures are gone.
static void acc_close(void)
{
	if (!acc.on && !al.up) return;
	acc_detach();
	al_close();
	if (al.pinned) zcp_unpin();
	al.pinned = 0;
}

static void acc_diag(double t)
{
	if (!al.w.start) al.w.start = t;
	if (t - al.w.start < 10) return;
	unsigned sent = acc.pics_sent - al.w.sent, fp_n = al.fp_n - al.w.fp_n, shown = al.shown - al.w.shown;
	if (dd.on)
		printf("mediacenter: diag acceleratore %.0f s: inviate %u immagini (%.0f KB l'una, invio %.1f ms, %u attese dell'anello), "
			"FPGA %.1f ms tra due immagini (senza comandi %.1f ms, attesa memoria %.1f ms), "
			"disegnate %u (attesa media %.1f ms), in ritardo %u, perse %u, slot al massimo %d\n",
			mc.clock, sent, sent ? (double)(acc.words_sent - al.w.words) * 4 / 1024 / sent : 0,
			sent ? 1000 * (acc.send_time - al.w.send) / sent : 0, al.ring_waits - al.w.ring_waits,
			fp_n ? (al.fp_since - al.w.fp_since) / fp_n / 108000.0 : 0, fp_n ? (al.fp_starve - al.w.fp_starve) / fp_n / 108000.0 : 0,
			fp_n ? (al.fp_mwait - al.w.fp_mwait) / fp_n / 108000.0 : 0,
			shown, shown ? 1000 * (al.wait_t - al.w.wait) / shown : 0, al.late - al.w.late, al.lost - al.w.lost, acc.slots_peak);
	al.w.sent = acc.pics_sent;
	al.w.words = acc.words_sent;
	al.w.send = acc.send_time;
	al.w.ring_waits = al.ring_waits;
	al.w.fp_n = al.fp_n;
	al.w.fp_since = al.fp_since;
	al.w.fp_starve = al.fp_starve;
	al.w.fp_mwait = al.fp_mwait;
	al.w.shown = al.shown;
	al.w.wait = al.wait_t;
	al.w.late = al.late;
	al.w.lost = al.lost;
	al.w.start = t;
}

static void acc_report(void)
{
	if (!acc.pics_sent && !al.shown && !al.lost) return;
	printf("mediacenter: diag acceleratore: inviate %u immagini (%.0f KB l'una, invio %.1f ms, %u attese dell'anello per %.1f s), "
		"FPGA %.1f ms tra due immagini (senza comandi %.1f ms, attesa memoria %.1f ms), disegnate %u (attesa media %.1f ms), "
		"in ritardo %u, perse %u, scartate dopo un salto %u, slot al massimo %d%s%s\n",
		acc.pics_sent, acc.pics_sent ? (double)acc.words_sent * 4 / 1024 / acc.pics_sent : 0,
		acc.pics_sent ? 1000 * acc.send_time / acc.pics_sent : 0, al.ring_waits, al.ring_wait_t,
		al.fp_n ? al.fp_since / al.fp_n / 108000.0 : 0, al.fp_n ? al.fp_starve / al.fp_n / 108000.0 : 0,
		al.fp_n ? al.fp_mwait / al.fp_n / 108000.0 : 0, al.shown, al.shown ? 1000 * al.wait_t / al.shown : 0,
		al.late, al.lost, acc.pics_dropped, acc.slots_peak, acc.failed ? "; spento: " : "", acc.failed ? acc.why : "");
}

// zcp_draw for a picture of the accelerator: waits for the core to finish
// it, then has the zero-copy block copy its slot. 1 when drawn.
static int acc_zc_draw(AVFrame *f, acc_buf_t *b, int rows)
{
	double t0 = now_seconds();
	acc_diag(t0);
	if (!zcp_mb || zcp_broken || !fb_phys)
	{
		acc_fail(NULL, "the picture cannot be drawn by the core");
		return 0;
	}
	if (fb_w * 3 != f->width * 2 || (fb_w & 31) || rows <= 0 || f->height < 2 * rows)
	{
		acc_fail_layout("the picture format is not drawn by the core");
		return 0;
	}
	int r = al_wait(b);
	if (r < 0)
	{
		al.lost++;
		return 0;
	}
	if (!r)
	{
		al.late++;
		// the core is stuck: back to the normal decoder
		if (++al.late_row >= 3) acc_fail(NULL, "the FPGA does not finish the pictures");
	}
	else al.late_row = 0;
	double t1 = now_seconds();
	// where the picture's planes are in the slot (the decoder may crop it)
	size_t yo = (size_t)(f->data[0] - acc.dummy);
	uint8_t *u0 = acc.dummy + (size_t)acc.dummy_ls * acc.dummy_h;
	uint8_t *v0 = u0 + (size_t)acc.dummy_cs * ((acc.dummy_h + 1) / 2);
	size_t uo = (size_t)(f->data[1] - u0), vo = (size_t)(f->data[2] - v0);
	uint32_t y_off = (uint32_t)(yo / acc.dummy_ls * ACC_Y_STRIDE + yo % acc.dummy_ls);
	uint32_t u_off = ACC_U_OFF + (uint32_t)(uo / acc.dummy_cs * ACC_C_STRIDE + uo % acc.dummy_cs);
	uint32_t v_off = ACC_V_OFF + (uint32_t)(vo / acc.dummy_cs * ACC_C_STRIDE + vo % acc.dummy_cs);
	uint32_t page = (uint32_t)(fb_phys + (uintptr_t)(fb_mem - (fb_map ? fb_map : fb_mem)));
	volatile uint32_t *m = zcp_mb;
	m[2] = page;
	m[3] = (uint32_t)fb_stride | (uint32_t)rows << 16;
	m[4] = ZCP_MBOX + ACC_ZC_TABLES + (uint32_t)b->slot * 4096;
	m[5] = ACC_SLOT_PAGES | (uint32_t)(fb_w / 4) << 16;
	m[6] = y_off;
	m[7] = 2 * ACC_Y_STRIDE;
	m[8] = u_off;
	m[9] = v_off;
	m[10] = ACC_C_STRIDE;
	m[0] = ZCP_MAGIC;
	__sync_synchronize();
	unsigned seq = ++zcp_seq;
	if (!seq) seq = ++zcp_seq;
	m[1] = seq;
	__sync_synchronize();
	int ok = 0;
	for (int i = 0; i < 400; i++)
	{
		if (m[18] == seq) { ok = 1; break; }
		usleep(250);
	}
	if (!ok || m[17])
	{
		acc_fail(NULL, ok ? "the zero-copy block refused the picture" : "the zero-copy block does not answer");
		return 0;
	}
	al.shown++;
	al.wait_t += t1 - t0;
	zd.core++; zd.w_core++;
	zd.wait += now_seconds() - t1; zd.w_wait += now_seconds() - t1;
	zd.cycles += m[16]; zd.w_cycles += m[16];
	return 1;
}

// After a seek.
static void acc_seek(void)
{
	if (acc.on) al_restart();
}
#endif
