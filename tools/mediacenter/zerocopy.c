// Zero-copy (1080p light mode): the menu core copies the decoded picture into
// the frame buffer instead of the ARM (the core's mc_zc block).
//
// The decoder writes its pictures into a pool of locked memory, with Linux
// told not to move locked pages (vm.compact_unevictable_allowed=0, while a
// file plays). The physical address of each 4 KB page of a pool block goes
// into a page table in DDR; for each picture the player writes a command in
// a mailbox next to it (/dev/mem, without the caches) and the core reads the
// even Y lines and the U and V lines, narrows them 1920 -> 1280 as
// pack_yuyv_32 does and writes the frame buffer page. The ARM only waits.
//
// The core reads DDR, not the ARM's caches: a picture is handed to it only
// once the decoder has handed over MC_ZC_AGE more pictures (4; measured with
// --zctest: after 3 or more no piece of a picture was still in the caches).
// Younger pictures, pictures outside the pool and the lines under the
// on-screen controls are drawn by the ARM as before. The first picture of
// each file is checked against the ARM's own copy: if they differ, or the
// core does not answer (an older core), the ARM draws everything.
// MC_NOZC=1 turns it off.

#include <sys/mman.h>

#define ZCP_MBOX   0x23F00000u   // mailbox and page tables (beyond the Main's frame buffers)
#define ZCP_MLEN   0x100000u
#define ZCP_TABLE  4096u         // first page table; one per pool block
#define ZCP_TSIZE  8192u         // 2048 pages: blocks up to 8 MB
#define ZCP_SLOTS  64
#define ZCP_PAGE   4096
#define ZCP_MAGIC  0x435A434Du   // "MCZC"

typedef struct
{
	uint8_t *mem;
	size_t size;
	int used;
	int npages;
	unsigned seq;       // the picture's number when the decoder handed it over
	uint64_t *pfn;      // physical page of each page, as in the page table
} zcp_slot_t;

static zcp_slot_t zcp_slot[ZCP_SLOTS];
static int zcp_nslots, zcp_on, zcp_broken, zcp_checked, zcp_pagemap = -1, zcp_devmem = -1;
static volatile uint32_t *zcp_mb;
static unsigned zcp_handed;         // pictures handed over by the decoder
static unsigned zcp_seq;
static int zcp_age = 4;
static char zcp_compact[16];        // vm.compact_unevictable_allowed before playing
static pthread_mutex_t zcp_lock = PTHREAD_MUTEX_INITIALIZER;
static int zcp_get_buffer(AVCodecContext *s, AVFrame *f, int flags);

// diag: pictures drawn by the core, by the ARM (too young / not in the pool /
// other), the wait for the core and the core's own time; page moves
static struct
{
	int core, young, other, moved, w_core, w_young, w_other;
	double wait, cycles, w_wait, w_cycles, w_start;
} zd;

static void zcp_sysctl(const char *val, char *old, size_t old_len)
{
	const char *p = "/proc/sys/vm/compact_unevictable_allowed";
	if (old)
	{
		old[0] = 0;
		FILE *f = fopen(p, "r");
		if (f)
		{
			if (!fgets(old, (int)old_len, f)) old[0] = 0;
			fclose(f);
		}
		old[strcspn(old, "\n")] = 0;
	}
	FILE *f = fopen(p, "w");
	if (f)
	{
		fputs(val, f);
		fclose(f);
	}
}

// Physical pages of n pages from v, into pfn (0: not present).
static int zcp_phys(const void *v, int n, uint64_t *pfn)
{
	off_t off = (off_t)((uintptr_t)v / ZCP_PAGE) * 8;
	if (zcp_pagemap < 0 || pread(zcp_pagemap, pfn, (size_t)n * 8, off) != (ssize_t)n * 8) return -1;
	for (int i = 0; i < n; i++) pfn[i] = (pfn[i] >> 63) ? (pfn[i] & ((1ULL << 55) - 1)) : 0;
	return 0;
}

static volatile uint32_t *zcp_table(int slot)
{
	return zcp_mb + (ZCP_TABLE + (size_t)slot * ZCP_TSIZE) / 4;
}

// The mailbox (also for the FPGA accelerator's pictures, accel_link.c).
static int zcp_map(void)
{
	if (zcp_mb) return 0;
	if (zcp_broken) return -1;
	zcp_devmem = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	zcp_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	void *m = zcp_devmem < 0 ? MAP_FAILED :
		mmap(NULL, ZCP_MLEN, PROT_READ | PROT_WRITE, MAP_SHARED, zcp_devmem, ZCP_MBOX);
	if (m == MAP_FAILED || zcp_pagemap < 0)
	{
		fprintf(stderr, "mediacenter: zero-copy not available (/dev/mem or pagemap), the ARM draws\n");
		zcp_broken = 1;
		return -1;
	}
	zcp_mb = (volatile uint32_t *)m;
	zcp_mb[0] = ZCP_MAGIC;
	zcp_seq = zcp_mb[1];
	return 0;
}

// Linux must not move locked pages while the core reads them (zero-copy
// pool, accelerator ring): counted, the setting comes back with the last.
static int zcp_pins;
static void zcp_pin(void)
{
	if (!zcp_pins++) zcp_sysctl("0", zcp_compact, sizeof(zcp_compact));
}
static void zcp_unpin(void)
{
	if (zcp_pins && !--zcp_pins && zcp_compact[0]) zcp_sysctl(zcp_compact, NULL, 0);
}

// Called when a file opens its video decoder: the pool for a 1080p picture
// that is drawn narrowed and with half its lines.
static void zcp_open(AVCodecContext *ctx, int width, int half)
{
	zcp_on = 0;
	zcp_checked = 0;
	if (!half || width != 1920 || zcp_broken || fb_fake) return;
	const char *e = getenv("MC_NOZC");
	if (e && atoi(e)) return;
	e = getenv("MC_ZC_AGE");
	if (e && atoi(e) > 0) zcp_age = atoi(e);
	if (zcp_map()) return;
	zcp_pin();
	zcp_on = 1;
	ctx->get_buffer2 = zcp_get_buffer;
	memset(&zd, 0, sizeof(zd));
	fprintf(stderr, "mediacenter: zero-copy on (the core draws pictures handed over %d pictures before)\n", zcp_age);
}

// After the decoder and all its pictures are gone: the pool goes, the
// setting comes back.
static void zcp_close(void)
{
	pthread_mutex_lock(&zcp_lock);
	int kept = 0;
	for (int i = 0; i < zcp_nslots; i++)
	{
		if (zcp_slot[i].used) { kept++; continue; }
		munmap(zcp_slot[i].mem, zcp_slot[i].size);
		free(zcp_slot[i].pfn);
		zcp_slot[i].mem = NULL;
		zcp_slot[i].size = 0;
	}
	// keep the blocks still in use (none expected) at their places
	if (!kept) zcp_nslots = 0;
	pthread_mutex_unlock(&zcp_lock);
	if (zcp_on) zcp_unpin();
	zcp_on = 0;
}

static void zcp_free(void *opaque, uint8_t *data)
{
	(void)data;
	pthread_mutex_lock(&zcp_lock);
	((zcp_slot_t *)opaque)->used = 0;
	pthread_mutex_unlock(&zcp_lock);
}

static zcp_slot_t *zcp_take(size_t need)
{
	pthread_mutex_lock(&zcp_lock);
	zcp_slot_t *s = NULL;
	for (int i = 0; i < zcp_nslots && !s; i++)
		if (zcp_slot[i].mem && !zcp_slot[i].used && zcp_slot[i].size >= need) s = &zcp_slot[i];
	if (!s && zcp_nslots < ZCP_SLOTS && need <= (size_t)ZCP_TSIZE / 4 * ZCP_PAGE)
	{
		size_t size = (need + ZCP_PAGE - 1) & ~(size_t)(ZCP_PAGE - 1);
		void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED | MAP_POPULATE, -1, 0);
		if (m != MAP_FAILED)
		{
			zcp_slot_t *n = &zcp_slot[zcp_nslots];
			n->mem = (uint8_t *)m;
			n->size = size;
			n->npages = (int)(size / ZCP_PAGE);
			n->pfn = (uint64_t *)calloc((size_t)n->npages, sizeof(uint64_t));
			if (n->pfn && !zcp_phys(n->mem, n->npages, n->pfn))
			{
				volatile uint32_t *t = zcp_table(zcp_nslots);
				for (int p = 0; p < n->npages; p++) t[p] = (uint32_t)n->pfn[p];
				zcp_nslots++;
				s = n;
			}
			else
			{
				free(n->pfn);
				munmap(m, size);
			}
		}
	}
	if (s) s->used = 1;
	pthread_mutex_unlock(&zcp_lock);
	return s;
}

static zcp_slot_t *zcp_slot_of(const uint8_t *p)
{
	for (int i = 0; i < zcp_nslots; i++)
		if (zcp_slot[i].mem && p >= zcp_slot[i].mem && p < zcp_slot[i].mem + zcp_slot[i].size) return &zcp_slot[i];
	return NULL;
}

// The decoder's pictures in the pool (8-bit 4:2:0 only; the rest as usual).
static int zcp_get_buffer(AVCodecContext *s, AVFrame *f, int flags)
{
	if (f->format != AV_PIX_FMT_YUV420P || !zcp_on) return avcodec_default_get_buffer2(s, f, flags);
	int w = f->width, h = f->height, align[AV_NUM_DATA_POINTERS];
	avcodec_align_dimensions2(s, &w, &h, align);
	int ys = (w + 63) & ~63, cs = ((w + 1) / 2 + 63) & ~63, ch = (h + 1) / 2;
	size_t ysz = (size_t)ys * h, csz = (size_t)cs * ch;
	size_t need = ysz + 2 * csz + 2 * ZCP_PAGE; // room for reads past the end
	zcp_slot_t *slot = zcp_take(need);
	if (!slot) return avcodec_default_get_buffer2(s, f, flags);
	f->buf[0] = av_buffer_create(slot->mem, slot->size, zcp_free, slot, 0);
	if (!f->buf[0])
	{
		zcp_free(slot, NULL);
		return AVERROR(ENOMEM);
	}
	f->data[0] = slot->mem;
	f->data[1] = slot->mem + ysz;
	f->data[2] = slot->mem + ysz + csz;
	f->linesize[0] = ys;
	f->linesize[1] = f->linesize[2] = cs;
	f->extended_data = f->data;
	return 0;
}

// A picture the decoder hands over (its age counts from here).
static void zcp_mark(const AVFrame *f)
{
	if (!zcp_on || !f->data[0]) return;
	zcp_handed++;
	pthread_mutex_lock(&zcp_lock);
	zcp_slot_t *s = zcp_slot_of(f->data[0]);
	if (s) s->seq = zcp_handed;
	pthread_mutex_unlock(&zcp_lock);
}

static void zcp_diag(double t)
{
	if (!zd.w_start) zd.w_start = t;
	if (t - zd.w_start < 10) return;
	if (dd.on)
		printf("mediacenter: diag zero-copy %.0f s: %d immagini disegnate dal core (attesa media %.1f ms, lavoro del core %.1f ms), "
			"dal ARM %d troppo recenti e %d per altri motivi\n",
			mc.clock, zd.w_core, zd.w_core ? 1000 * zd.w_wait / zd.w_core : 0, zd.w_core ? zd.w_cycles / zd.w_core / 108000.0 : 0,
			zd.w_young, zd.w_other);
	zd.w_core = zd.w_young = zd.w_other = 0;
	zd.w_wait = zd.w_cycles = 0;
	zd.w_start = t;
}

static void zcp_report(void)
{
	if (!zd.core && !zd.young && !zd.other) return;
	printf("mediacenter: diag zero-copy: %d immagini disegnate dal core (attesa media %.1f ms, lavoro del core %.1f ms), "
		"dal ARM %d troppo recenti e %d per altri motivi, pagine spostate %d\n",
		zd.core, zd.core ? 1000 * zd.wait / zd.core : 0, zd.core ? zd.cycles / zd.core / 108000.0 : 0, zd.young, zd.other, zd.moved);
}

// Has the core draw lines 0..rows-1 of the frame buffer page at fb_mem from
// f (1080p, half the lines, 1920 -> fb_w = 1280). Returns 1 when done, 0 when
// the ARM must draw them.
static int acc_zc_draw(AVFrame *f, acc_buf_t *b, int rows);

static int zcp_draw(AVFrame *f, int rows)
{
	// a picture of the FPGA accelerator (accel_link.c)
	acc_buf_t *ab = acc.on ? acc_buf_of(f) : NULL;
	if (ab)
	{
		zcp_diag(now_seconds());
		return acc_zc_draw(f, ab, rows);
	}
	if (!zcp_on || zcp_broken || !zcp_mb || !fb_phys || !f->data[0]) return 0;
	double t0 = now_seconds();
	zcp_diag(t0);
	if (fb_w * 3 != f->width * 2 || (fb_w & 31) || rows <= 0 || f->height < 2 * rows)
	{
		zd.other++; zd.w_other++;
		return 0;
	}
	pthread_mutex_lock(&zcp_lock);
	zcp_slot_t *s = zcp_slot_of(f->data[0]);
	int idx = s ? (int)(s - zcp_slot) : -1;
	unsigned age = s ? zcp_handed - s->seq : 0;
	pthread_mutex_unlock(&zcp_lock);
	if (!s)
	{
		zd.other++; zd.w_other++;
		return 0;
	}
	if (age < (unsigned)zcp_age)
	{
		zd.young++; zd.w_young++;
		return 0;
	}
	// the pages are where the table says (Linux could still move one)
	static uint64_t *now;
	static int now_n;
	if (now_n < s->npages)
	{
		free(now);
		now = (uint64_t *)malloc((size_t)s->npages * sizeof(uint64_t));
		now_n = now ? s->npages : 0;
	}
	if (!now || zcp_phys(s->mem, s->npages, now))
	{
		zd.other++; zd.w_other++;
		return 0;
	}
	volatile uint32_t *t = zcp_table(idx);
	for (int p = 0; p < s->npages; p++)
		if (now[p] != s->pfn[p])
		{
			if (!now[p]) { zd.other++; zd.w_other++; return 0; }
			s->pfn[p] = now[p];
			t[p] = (uint32_t)now[p];
			zd.moved++;
		}
	uint32_t page = (uint32_t)(fb_phys + (uintptr_t)(fb_mem - (fb_map ? fb_map : fb_mem)));
	volatile uint32_t *m = zcp_mb;
	m[2] = page;
	m[3] = (uint32_t)fb_stride | (uint32_t)rows << 16;
	m[4] = ZCP_MBOX + ZCP_TABLE + (uint32_t)idx * ZCP_TSIZE;
	m[5] = (uint32_t)s->npages | (uint32_t)(fb_w / 4) << 16;  // output words of 8 bytes a line
	m[6] = (uint32_t)(f->data[0] - s->mem);
	m[7] = (uint32_t)(2 * f->linesize[0]);
	m[8] = (uint32_t)(f->data[1] - s->mem);
	m[9] = (uint32_t)(f->data[2] - s->mem);
	m[10] = (uint32_t)f->linesize[1];
	m[0] = ZCP_MAGIC;
	__sync_synchronize();
	unsigned seq = ++zcp_seq;
	if (!seq) seq = ++zcp_seq;
	m[1] = seq;
	__sync_synchronize();
	// the core answers in a few ms; a core without the block never does
	int ok = 0;
	for (int i = 0; i < 400; i++)
	{
		if (m[18] == seq) { ok = 1; break; }
		usleep(250);
	}
	double t1 = now_seconds();
	if (!ok || m[17])
	{
		fprintf(stderr, "mediacenter: zero-copy: the core %s, the ARM draws\n", ok ? "refused the picture" : "does not answer");
		zcp_broken = 1;
		zd.other++; zd.w_other++;
		return 0;
	}
	if (!zcp_checked)
	{
		// the first picture: two lines compared with the ARM's own copy
		zcp_checked = 1;
		static uint8_t *ref;
		ref = (uint8_t *)realloc(ref, (size_t)fb_w * 2);
		int bad = !ref;
		for (int k = 0; ref && k < 2 && !bad; k++)
		{
			int y = k ? rows - 1 : rows / 2;
			pack_yuyv_32(ref, f->data[0] + (size_t)(2 * y) * f->linesize[0], f->data[1] + (size_t)y * f->linesize[1],
				f->data[2] + (size_t)y * f->linesize[2], fb_w);
			bad = memcmp(ref, fb_mem + (size_t)y * fb_stride, (size_t)fb_w * 2) != 0;
		}
		printf("mediacenter: diag zero-copy: prova del core %s (%.1f ms)\n", bad ? "FALLITA, disegna il ARM" : "riuscita", 1000 * (t1 - t0));
		if (bad)
		{
			zcp_broken = 1;
			zd.other++; zd.w_other++;
			return 0;
		}
	}
	zd.core++; zd.w_core++;
	zd.wait += t1 - t0; zd.w_wait += t1 - t0;
	zd.cycles += m[16]; zd.w_cycles += m[16];
	return 1;
}
