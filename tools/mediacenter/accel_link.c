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
// The picture whose turn it is writes its commands straight into the ring
// (accel.c): for that the ring is mapped twice, one copy right after the
// other, so a run of commands that goes past the ring's end lands at its
// start. MC_ACC_COPY=1 copies every picture into the ring, as before.
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
#define ACC_OVL_BASE   0x2F800000u   // the controls for mc_zc to blend (outside Linux, unused)
#define ACC_OVL_LEN    0x80000u
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
	size_t ring_map;           // bytes of address space behind ring
	int mirror;                // the ring is there twice, one copy after the other
	uint32_t dbase;            // written straight: the count of the ring's first byte
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
	struct { unsigned shown, late, lost, ring_waits, sent, fp_n; uint64_t words, direct; double send, wait, fp_since, fp_starve, fp_mwait, start; } w;
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
	if (al.ring) munmap(al.ring, al.ring_map ? al.ring_map : al.ring_len);
	if (al.pool) munmap(al.pool, al.ring_len);
#endif
	al.ring = NULL;
	al.pool = NULL;
	al.ring_map = 0;
	al.mirror = 0;
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
	// the same pages again, one after the other and without the caches,
	// twice (the second copy for the commands that go past the end)
	void *va = mmap(NULL, 2 * len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (va == MAP_FAILED)
	{
		snprintf(why, why_len, "no address space for the ring");
		goto fail;
	}
	al.ring = (uint8_t *)va;
	al.ring_map = 2 * len;
	al.mirror = 1;
	for (int i = 0; i < ACC_RING_PAGES;)
	{
		int j = i + 1;
		while (j < ACC_RING_PAGES && pfn[j] == pfn[j - 1] + 1) j++;
		if (!al_map((uint32_t)(pfn[i] * 4096), (size_t)(j - i) * 4096, al.ring + (size_t)i * 4096))
		{
			snprintf(why, why_len, "the ring cannot be mapped (/dev/mem)");
			goto fail;
		}
		// the second copy: without it every picture is copied, as before
		if (al.mirror && !al_map((uint32_t)(pfn[i] * 4096), (size_t)(j - i) * 4096, al.ring + len + (size_t)i * 4096))
			al.mirror = 0;
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
	// the second copy shows the same memory (a word of each page)
	for (size_t i = 0; al.mirror && i < len / 4; i += 1024 + 1)
		if (((volatile uint32_t *)r)[len / 4 + i] != (uint32_t)i * 2654435761u) al.mirror = 0;
	for (int i = 0; i < ACC_RING_PAGES; i++) al.mb[ACC_MTABLE + i] = (uint32_t)pfn[i];
	free(pfn);
	return 0;
fail:
	free(pfn);
	al_ring_free();
	return -1;
}

#ifdef ACC_FAKE
uint8_t *acc_fake_ring(size_t len);
// The pretend core's ring, mapped twice like the player's own (acctest.c).
static int al_ring_fake(char *why, size_t why_len)
{
	al.ring_len = (size_t)ACC_RING_PAGES * 4096;
	al.ring = acc_fake_ring(al.ring_len);
	if (!al.ring)
	{
		snprintf(why, why_len, "no pretend ring");
		return -1;
	}
	al.ring_map = 0;
	al.mirror = 1;
	return 0;
}
#endif

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

// ---------------------------------------------------------------- written straight

// The ring's count of a place in the ring (either copy).
static uint32_t al_count(const uint32_t *p)
{
	return al.dbase + (uint32_t)((const uint8_t *)p - al.ring);
}

// acc.ring_ok: pictures can be written straight into the ring.
static int al_ring_ok(void)
{
	return al.up && al.mirror && !al.so;
}

// Waits until the core has read the ring far enough for `bytes` from the
// count pos; the bytes free from there. -1: it does not (a second, a core
// error, the accelerator off).
static long al_room_wait(uint32_t pos, size_t bytes)
{
	double t0 = 0;
	size_t room;
	for (;;)
	{
		uint32_t used = pos - al_read_done();
		room = used <= al.ring_len ? al.ring_len - used : 0;
		if (room >= bytes) break;
		double t = acc_now();
		if (!t0)
		{
			t0 = t;
			al.ring_waits++;
		}
		if (t - t0 > 1.0 || (al.mb[20] & 3) || acc.failed) return -1;
		usleep(200);
	}
	if (t0) al.ring_wait_t += acc_now() - t0;
	return (long)room;
}

// The words before the count pos are in the ring: the core may read them
// (whole 64-bit words only).
static void al_publish(uint32_t pos)
{
	pos &= ~7u;
	if (pos == al.wp) return;
	al_drain();
	al.wp = pos;
	al.mb[4] = pos;
}

// acc.ring_room: the words before *p are done; room for `words` more, at
// most ACC_CHUNK bytes so that the write pointer moves that often.
static int al_ring_room(uint32_t **pp, uint32_t **endp, size_t words)
{
	uint32_t *p = *pp;
	uint32_t pos = al_count(p);
	al_publish(pos);
	// back into the first copy
	if ((uint8_t *)p >= al.ring + al.ring_len)
	{
		p = (uint32_t *)((uint8_t *)p - al.ring_len);
		al.dbase += (uint32_t)al.ring_len;
	}
	long room = al_room_wait(pos, words * 4);
	if (room < 0) return -1;
	if (room > ACC_CHUNK) room = ACC_CHUNK;
	*pp = p;
	*endp = p + room / 4;
	return 0;
}

// acc.ring_take: the picture whose turn it is goes on in the ring; the n
// words it wrote before are copied first. Everything before it is sent.
static int al_ring_take(const uint32_t *w, size_t n, size_t words, uint32_t **pp, uint32_t **endp, uint32_t *start)
{
	*start = al.wp;
	if ((n & ~(size_t)1) && al_send(w, n & ~(size_t)1, 0) < 0) return -1;
	size_t off = al.wp % al.ring_len;
	al.dbase = al.wp - (uint32_t)off;
	uint32_t *p = (uint32_t *)(al.ring + off);
	if (n & 1)
	{
		// the odd last word: published with the next ones
		if (al_room_wait(al.wp, 4) < 0) return -1;
		*p++ = w[n - 1];
	}
	*pp = p;
	return al_ring_room(pp, endp, words);
}

// acc.ring_end: the picture's last word is before p; the count after it.
static uint32_t al_ring_end(uint32_t *p)
{
	uint32_t pos = al_count(p);
	al_publish(pos);
	return pos;
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

// The controls blended by the core: see acc_ovl_prepare.
static struct
{
	volatile uint64_t *map;
	uint64_t *shadow, *line;
	int valid, failed, y, w, rows;
	unsigned gen;
} ao;
static int acc_ovl_on; // this picture: the core blends the controls

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
	if (al.so ? al_ring_so(why, why_len) : al_ring_fake(why, why_len)) return -1;
#else
	if (al.so ? al_ring_so(why, why_len) : al_ring_wc(why, why_len)) return -1;
#endif
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
	ao.valid = 0;
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

// The core's zero-copy block can also resample any width and height (from
// up to twice the frame buffer's pixels and lines, 2048 wide at most, the
// same pixels as the player's pack_yuyv_map): said by mc_acc in its answer
// (word 20, bit 20); kept once seen.
static int acc_core_rs;
static int acc_can_rs(void)
{
	if (acc.on && al.mb && (al.mb[20] & 0x100000)) acc_core_rs = 1;
	return acc_core_rs;
}

// A picture sw x sh (source lines, half of them in light mode) drawn by the
// core's resampling on a frame buffer w x rows.
static int acc_rs_fits(int sw, int sh, int w, int rows)
{
	return acc_can_rs() && w > 0 && !(w & 31) && rows > 0 && sw <= 2048 && w <= sw && sw < 2 * w && rows <= sh && sh < 2 * rows;
}

// Turns the accelerator on for a new video decoder when it can do the
// stream: H.264, 8-bit 4:2:0, 1920 wide (or 1440: 810p, drawn on a 960
// wide frame buffer), drawn with half its lines by the zero-copy block
// (light mode). With a core that resamples (acc_can_rs), also any other
// H.264 from 1024 x 400 to 2048 x 1088 (720p, 1728 x 720...): when the core
// turns out not to resample, it is turned off again. MC_NOACC=1 turns it off.
static void acc_try(AVCodecContext *ctx, const AVStream *st)
{
	const char *e = getenv("MC_NOACC");
	int w = st->codecpar->width, h = st->codecpar->height;
	int old = (w == 1920 || w == 1440) && half_lines;
	int rs = !old && w >= 1024 && w <= 2048 && !(w & 1) && h >= 400 && h <= 1088;
	if (ctx->codec_id != AV_CODEC_ID_H264 || st->codecpar->format != AV_PIX_FMT_YUV420P || (!old && !rs) ||
		fb_fake || zc_get_buffer2 || acc_file_off || (e && atoi(e))) return;
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
	acc.ring_ok = al_ring_ok;
	acc.ring_take = al_ring_take;
	acc.ring_room = al_ring_room;
	acc.ring_end = al_ring_end;
	// deblocking in the core, which works out the edge strengths (answer
	// word 20, bit 18: command A5 of 3 words), unless MC_NODEBLOCK=1. The
	// phase 4 test core (bit 17, A5 of 5 words) is not used.
	// Word 20 comes just after the words al_open waited for: every core
	// since 3.0 sets bit 16 in it, so once bit 16 is there the word is new.
	for (int i = 0; i < 40 && !(al.mb[20] & 0x10000); i++) usleep(500);
	if (rs && !(al.mb[20] & 0x100000))
	{
		printf("mediacenter: diag acceleratore: spento (%dx%d: il core non sa ridimensionare, serve il core nuovo)\n", w, h);
		al_close();
		al.pinned = 0;
		zcp_unpin();
		return;
	}
	if (rs) acc_core_rs = 1;
	e = getenv("MC_NODEBLOCK");
	int dbk_core = (al.mb[20] & 0x40000) != 0, dbk_off = e && atoi(e);
	acc.hooks.deblock = dbk_core && !dbk_off;
	acc_attach(ctx);
	memset(&zd, 0, sizeof(zd));
	printf("mediacenter: diag acceleratore: acceso (anello %s, scritto direttamente %s, svuotamento L2 %s, sessione %u, pronto in %.0f ms, deblocking %s)\n",
		al.so ? "fuori da Linux" : "nella memoria del lettore",
		acc.copy_only ? "no (MC_ACC_COPY)" : al.so ? "no" : al_ring_ok() ? "si" : "no (seconda mappatura non riuscita)",
		al.l2 ? "si" : "NO", (unsigned)al.fsess, 1000 * (now_seconds() - t0),
		acc.hooks.deblock ? "nell'FPGA" : (dbk_core ? "spento (MC_NODEBLOCK)" : "no (core senza filtro)"));
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
		printf("mediacenter: diag acceleratore %.0f s: inviate %u immagini (%.0f KB l'una, scritte direttamente nell'anello %.0f%%, "
			"invio %.1f ms, %u attese dell'anello), "
			"FPGA %.1f ms tra due immagini (senza comandi %.1f ms, attesa memoria %.1f ms), "
			"disegnate %u (attesa media %.1f ms), in ritardo %u, perse %u, slot al massimo %d\n",
			mc.clock, sent, sent ? (double)(acc.words_sent - al.w.words) * 4 / 1024 / sent : 0,
			acc.words_sent > al.w.words ? 100.0 * (acc.words_direct - al.w.direct) / (acc.words_sent - al.w.words) : 0,
			sent ? 1000 * (acc.send_time - al.w.send) / sent : 0, al.ring_waits - al.w.ring_waits,
			fp_n ? (al.fp_since - al.w.fp_since) / fp_n / 108000.0 : 0, fp_n ? (al.fp_starve - al.w.fp_starve) / fp_n / 108000.0 : 0,
			fp_n ? (al.fp_mwait - al.w.fp_mwait) / fp_n / 108000.0 : 0,
			shown, shown ? 1000 * (al.wait_t - al.w.wait) / shown : 0, al.late - al.w.late, al.lost - al.w.lost, acc.slots_peak);
	al.w.sent = acc.pics_sent;
	al.w.words = acc.words_sent;
	al.w.direct = acc.words_direct;
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
	printf("mediacenter: diag acceleratore: inviate %u immagini (%.0f KB l'una, scritte direttamente nell'anello %.0f%%, "
		"invio %.1f ms, %u attese dell'anello per %.1f s), "
		"FPGA %.1f ms tra due immagini (senza comandi %.1f ms, attesa memoria %.1f ms), disegnate %u (attesa media %.1f ms), "
		"in ritardo %u, perse %u, scartate dopo un salto %u, slot al massimo %d%s%s\n",
		acc.pics_sent, acc.pics_sent ? (double)acc.words_sent * 4 / 1024 / acc.pics_sent : 0,
		acc.words_sent ? 100.0 * acc.words_direct / acc.words_sent : 0,
		acc.pics_sent ? 1000 * acc.send_time / acc.pics_sent : 0, al.ring_waits, al.ring_wait_t,
		al.fp_n ? al.fp_since / al.fp_n / 108000.0 : 0, al.fp_n ? al.fp_starve / al.fp_n / 108000.0 : 0,
		al.fp_n ? al.fp_mwait / al.fp_n / 108000.0 : 0, al.shown, al.shown ? 1000 * al.wait_t / al.shown : 0,
		al.late, al.lost, acc.pics_dropped, acc.slots_peak, acc.failed ? "; spento: " : "", acc.failed ? acc.why : "");
}

// The core's zero-copy block can also narrow by 2:1 (1920 on a 960 frame
// buffer): said by mc_acc in its answer (word 20, bit 16); kept once seen.
static int acc_core21;
static int acc_can21(void)
{
	if (acc.on && al.mb && (al.mb[20] & 0x10000)) acc_core21 = 1;
	return acc_core21;
}

// The controls blended by the core (mc_zc, when mc_acc's answer says so,
// word 20 bit 19): the player writes them at ACC_OVL_BASE, for each line of
// the band one 64-bit word per 4 pixels, bytes a0 g0 a1 g1 a2 g2 a3 g3
// (coverage, and grey 16 + v * 219 / 255 as yuv_band). Only the words that
// changed are written (a copy of what is there is kept), and only when the
// controls changed. MC_NOCOREOSD=1: the ARM blends them (yuv_band_fast).
// (ao and acc_ovl_on: before al_open)

#if defined(__arm__)
#pragma GCC push_options
#pragma GCC target("fpu=neon")
#endif
static int acc_ovl_prepare(void)
{
	if (!acc.on || !al.mb || !(al.mb[20] & 0x80000) || ao.failed) return 0;
	const char *e = getenv("MC_NOCOREOSD");
	if (e && atoi(e)) return 0;
	int rows = fb_h - ovl_y, ow = fb_w / 4;
	if (ovl_y < 0 || rows <= 0 || (fb_w & 3) || (size_t)ow * 8 * rows > ACC_OVL_LEN) return 0;
	if (!ao.map)
	{
		ao.map = (volatile uint64_t *)al_map(ACC_OVL_BASE, ACC_OVL_LEN, NULL);
		ao.shadow = (uint64_t *)malloc(ACC_OVL_LEN);
		ao.line = (uint64_t *)malloc((size_t)ACC_OVL_LEN / 64);
		if (!ao.map || !ao.shadow || !ao.line)
		{
			ao.failed = 1;
			printf("mediacenter: diag comandi sopra l'acceleratore: memoria per il core non disponibile, li mescola il ARM\n");
			return 0;
		}
	}
	if (ao.valid && ao.gen == ovl_gen && ao.y == ovl_y && ao.w == fb_w && ao.rows == rows) return 1;
	double t0 = now_seconds();
	int all = !ao.valid || ao.y != ovl_y || ao.w != fb_w || ao.rows != rows;
	for (int r = 0; r < rows; r++)
	{
		const uint8_t *a = ovl_a + (size_t)r * fb_w, *v = ovl_v + (size_t)r * fb_w;
		uint8_t *l = (uint8_t *)ao.line;
		int x = 0;
#if defined(__arm__)
		for (; x + 8 <= fb_w; x += 8)
		{
			uint8x8x2_t o;
			o.val[0] = vld1_u8(a + x);
			o.val[1] = grey_y(vld1_u8(v + x));
			vst2_u8(l + 2 * x, o);
		}
#endif
		for (; x < fb_w; x++)
		{
			l[2 * x] = a[x];
			l[2 * x + 1] = (uint8_t)(16 + v[x] * 219 / 255);
		}
		uint64_t *s = ao.shadow + (size_t)r * ow;
		volatile uint64_t *m = ao.map + (size_t)r * ow;
		for (int k = 0; k < ow; k++)
			if (all || s[k] != ao.line[k])
			{
				s[k] = ao.line[k];
				m[k] = ao.line[k];
			}
	}
	ao.valid = 1;
	ao.gen = ovl_gen;
	ao.y = ovl_y;
	ao.w = fb_w;
	ao.rows = rows;
	od.prep += now_seconds() - t0;
	od.w_prep += now_seconds() - t0;
	return 1;
}
#if defined(__arm__)
#pragma GCC pop_options
#endif

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
	int n21 = fb_w * 2 == f->width && acc_can21();
	int old = (fb_w * 3 == f->width * 2 || n21) && yuv_half && !(fb_w & 31) && rows > 0 && f->height >= 2 * rows;
	// any other format: resampled (the lines in light mode are every other one)
	int sh = yuv_half ? f->height / 2 : f->height;
	int rs = !old && acc_rs_fits(f->width, sh, fb_w, rows);
	if (!old && !rs)
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
	m[7] = (rs && !yuv_half ? 1 : 2) * ACC_Y_STRIDE;
	m[8] = u_off;
	m[9] = v_off;
	// resample: colour line = line / 2 when every line is used
	m[10] = ACC_C_STRIDE | (rs ? 1u << 19 | (yuv_half ? 0 : 1u << 16) : n21 ? 1u << 17 : 0) | (acc_ovl_on ? 1u << 18 : 0);
	m[11] = ACC_OVL_BASE;
	m[12] = (uint32_t)ovl_y;
	m[13] = rs ? (uint32_t)f->width | (uint32_t)sh << 16 : 0;
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
