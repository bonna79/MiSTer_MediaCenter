// mediacenter --zctest <file>: feasibility test of "zero-copy" (the FPGA
// reading the decoded pictures straight from memory instead of the ARM
// copying them into the framebuffer). Nothing here is used when playing.
//
// The pictures are decoded into a pool of locked memory (with Linux told not
// to move locked pages, vm.compact_unevictable_allowed=0, restored at the
// end), and the test checks:
//  1. the decoding speed with the pool, over the first 10 s without checks;
//  2. that Linux gives the physical address of each page of the pool, and
//     whether the pages move while decoding;
//  3. what the FPGA would read: from 0:10 to 6:00, one picture in four is
//     read through /dev/mem without the caches (O_SYNC), as DDR holds it,
//     when the decoder hands it over and 2 pictures later. A checksum of
//     each 32-byte piece (a cache line) is taken from DDR first, then
//     compared with what the ARM sees, so the check itself disturbs the
//     caches as little as possible. Pieces that differ were still in the
//     cache: the FPGA would read them stale. A line every 10 s of video.

#include <sys/mman.h>

#define ZC_SLOTS 64
#define ZC_PAGE 4096

typedef struct
{
	uint8_t *mem;
	size_t size;
	int used;
	int npages;
	uint64_t *pfn; // physical page of each page when the slot was made
} zc_slot_t;

static zc_slot_t zc_slot[ZC_SLOTS];
static int zc_nslots, zc_pagemap = -1, zc_devmem = -1, zc_pool_fail, zc_not_pool;
static pthread_mutex_t zc_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t zc_phys_page(const void *v)
{
	uint64_t e = 0;
	off_t off = (off_t)((uintptr_t)v / ZC_PAGE) * 8;
	if (zc_pagemap < 0 || pread(zc_pagemap, &e, 8, off) != 8) return 0;
	if (!(e >> 63)) return 0; // not present
	return e & ((1ULL << 55) - 1);
}

static void zc_free(void *opaque, uint8_t *data)
{
	(void)data;
	pthread_mutex_lock(&zc_lock);
	((zc_slot_t *)opaque)->used = 0;
	pthread_mutex_unlock(&zc_lock);
}

static zc_slot_t *zc_take(size_t need)
{
	pthread_mutex_lock(&zc_lock);
	for (int i = 0; i < zc_nslots; i++)
		if (!zc_slot[i].used && zc_slot[i].size >= need)
		{
			zc_slot[i].used = 1;
			pthread_mutex_unlock(&zc_lock);
			return &zc_slot[i];
		}
	zc_slot_t *s = NULL;
	if (zc_nslots < ZC_SLOTS)
	{
		size_t size = (need + ZC_PAGE - 1) & ~(size_t)(ZC_PAGE - 1);
		void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED | MAP_POPULATE, -1, 0);
		if (m != MAP_FAILED)
		{
			s = &zc_slot[zc_nslots++];
			s->mem = (uint8_t *)m;
			s->size = size;
			s->npages = (int)(size / ZC_PAGE);
			s->pfn = (uint64_t *)calloc((size_t)s->npages, sizeof(uint64_t));
			for (int p = 0; s->pfn && p < s->npages; p++) s->pfn[p] = zc_phys_page(s->mem + (size_t)p * ZC_PAGE);
			s->used = 1;
		}
		else zc_pool_fail++;
	}
	pthread_mutex_unlock(&zc_lock);
	return s;
}

static zc_slot_t *zc_slot_of(const uint8_t *p)
{
	for (int i = 0; i < zc_nslots; i++)
		if (p >= zc_slot[i].mem && p < zc_slot[i].mem + zc_slot[i].size) return &zc_slot[i];
	return NULL;
}

// The decoder's pictures in the pool (8-bit 4:2:0 only; the rest as usual).
static int zc_get_buffer(AVCodecContext *s, AVFrame *f, int flags)
{
	if (f->format != AV_PIX_FMT_YUV420P)
	{
		zc_not_pool++;
		return avcodec_default_get_buffer2(s, f, flags);
	}
	int w = f->width, h = f->height, align[AV_NUM_DATA_POINTERS];
	avcodec_align_dimensions2(s, &w, &h, align);
	int ys = (w + 63) & ~63, cs = ((w + 1) / 2 + 63) & ~63, ch = (h + 1) / 2;
	size_t ysz = (size_t)ys * h, csz = (size_t)cs * ch;
	size_t need = ysz + 2 * csz + 2 * ZC_PAGE; // slack for reads past the end
	zc_slot_t *slot = zc_take(need);
	if (!slot)
	{
		zc_not_pool++;
		return avcodec_default_get_buffer2(s, f, flags);
	}
	f->buf[0] = av_buffer_create(slot->mem, slot->size, zc_free, slot, 0);
	if (!f->buf[0])
	{
		zc_free(slot, NULL);
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


static uint16_t *zc_sums;  // DDR checksum of each 32-byte piece
static uint64_t *zc_fresh; // physical pages read just before a check
static int zc_sums_n, zc_fresh_n;

static uint16_t zc_sum32(const uint8_t *p)
{
	const uint32_t *w = (const uint32_t *)p;
	uint32_t s = 0;
	for (int i = 0; i < 8; i++) s = (s << 5 | s >> 27) ^ w[i];
	return (uint16_t)(s ^ s >> 16);
}

// Checks the slot's first n bytes: pieces of 32 bytes that DDR holds
// differently from what the ARM sees (*stale), pages that moved since the
// last check (*moved). Returns -1 when /dev/mem refuses.
static int zc_check(zc_slot_t *s, size_t n, long long *stale, long long *moved)
{
	int pages = (int)((n + ZC_PAGE - 1) / ZC_PAGE);
	int lines = pages * (ZC_PAGE / 32);
	if (lines > zc_sums_n)
	{
		free(zc_sums);
		zc_sums = (uint16_t *)malloc((size_t)lines * sizeof(uint16_t));
		zc_sums_n = zc_sums ? lines : 0;
	}
	if (pages > zc_fresh_n)
	{
		free(zc_fresh);
		zc_fresh = (uint64_t *)malloc((size_t)pages * sizeof(uint64_t));
		zc_fresh_n = zc_fresh ? pages : 0;
	}
	if (!zc_sums || !zc_fresh) return -1;
	*stale = *moved = 0;
	for (int p = 0; p < pages; p++)
	{
		zc_fresh[p] = zc_phys_page(s->mem + (size_t)p * ZC_PAGE);
		if (zc_fresh[p] != s->pfn[p])
		{
			(*moved)++;
			s->pfn[p] = zc_fresh[p];
		}
	}
	// first DDR, without the caches
	for (int p = 0; p < pages;)
	{
		int run = 1;
		while (p + run < pages && zc_fresh[p + run] == zc_fresh[p] + (uint64_t)run) run++;
		size_t len = (size_t)run * ZC_PAGE;
		const uint8_t *u = (const uint8_t *)mmap(NULL, len, PROT_READ, MAP_SHARED, zc_devmem, (off_t)(zc_fresh[p] * ZC_PAGE));
		if (u == MAP_FAILED) return -1;
		uint16_t *sum = zc_sums + (size_t)p * (ZC_PAGE / 32);
		for (size_t i = 0; i < len; i += 32) *sum++ = zc_sum32(u + i);
		munmap((void *)u, len);
		p += run;
	}
	// then what the ARM sees
	for (int l = 0; l < lines; l++)
		if (zc_sum32(s->mem + (size_t)l * 32) != zc_sums[l]) (*stale)++;
	return 0;
}

typedef struct
{
	AVFrame *f;
	zc_slot_t *slot;
	size_t bytes;
	int age;
} zc_held_t;

static const char *zc_time(double t, char *buf)
{
	int s = (int)(t + 0.5);
	sprintf(buf, "%d:%02d", s / 60, s % 60);
	return buf;
}

static int zctest(const char *path)
{
	printf("mediacenter: zctest v2: prova zero-copy su %s\n", path);
	zc_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	zc_devmem = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
	if (zc_pagemap < 0) printf("mediacenter: zctest: /proc/self/pagemap non apribile (%s)\n", strerror(errno));
	if (zc_devmem < 0) printf("mediacenter: zctest: /dev/mem non apribile (%s)\n", strerror(errno));

	// Linux told not to move locked pages while the test runs
	static const char *cua = "/proc/sys/vm/compact_unevictable_allowed";
	char cua_old[16] = "";
	int cua_set = 0;
	FILE *fp = fopen(cua, "r");
	if (fp)
	{
		if (!fgets(cua_old, sizeof(cua_old), fp)) cua_old[0] = 0;
		fclose(fp);
	}
	fp = cua_old[0] ? fopen(cua, "w") : NULL;
	if (fp)
	{
		cua_set = fputs("0\n", fp) >= 0;
		if (fclose(fp)) cua_set = 0;
	}
	printf("mediacenter: zctest: %s: era %s, %s\n", cua, cua_old[0] ? strtok(cua_old, "\n") : "(assente)",
		cua_set ? "messo a 0 per la prova" : "NON modificabile");

	zc_get_buffer2 = zc_get_buffer;
	int ret = 1;
	if (media_open(path)) goto restore;
	if (mc.vstream < 0 || !mc.vdec)
	{
		printf("mediacenter: zctest: il file non ha video\n");
		media_close();
		goto restore;
	}

	double fps = av_q2d(mc.fmt->streams[mc.vstream]->avg_frame_rate);
	double clean_decode = 0, t_decode = 0;
	int clean_frames = 0, frames = 0, refused = 0;
	// totals and the current 10 s window: [0] when handed over, [1] 2 pictures later
	long long n[2] = { 0 }, bad[2] = { 0 }, stale[2] = { 0 }, moved = 0;
	long long wn[2] = { 0 }, wbad[2] = { 0 }, wstale[2] = { 0 }, wmoved = 0;
	double w_decode = 0, w_start = 10;
	int w_frames = 0;
	zc_held_t held[4];
	int nheld = 0;
	double t0 = now_seconds();
	char ta[16], tb[16];

	for (int more = 1; more && !quit_requested;)
	{
		double media = !isnan(mc.last_vpts) ? mc.last_vpts : mc.audio_end;
		if (media >= 360.0) break;
		double a = now_seconds();
		more = decode_step();
		double d = now_seconds() - a;
		if (media < 10.0)
		{
			clean_decode += d;
			clean_frames += mc.vq_count;
		}
		else
		{
			t_decode += d;
			w_decode += d;
			w_frames += mc.vq_count;
		}
		for (int i = 0; i < mc.vq_count && !refused; i++)
		{
			AVFrame *f = mc.vq[i].frame;
			double pts = mc.vq[i].pts;
			// held pictures are now one picture older: checked at 2
			for (int k = 0; k < nheld;)
			{
				zc_held_t *h = &held[k];
				if (++h->age < 2) { k++; continue; }
				long long st, mv;
				if (zc_check(h->slot, h->bytes, &st, &mv)) refused = 1;
				else
				{
					n[1]++; wn[1]++;
					if (st) { bad[1]++; wbad[1]++; stale[1] += st; wstale[1] += st; }
					moved += mv; wmoved += mv;
				}
				av_frame_free(&h->f);
				held[k] = held[--nheld];
			}
			zc_slot_t *s = f->data[0] ? zc_slot_of(f->data[0]) : NULL;
			if (pts >= 10.0 && frames % 4 == 0 && s && s->pfn && nheld < 4 && !refused)
			{
				size_t bytes = (size_t)(f->data[2] - f->data[0]) + (size_t)f->linesize[2] * ((f->height + 1) / 2);
				long long st, mv;
				if (zc_check(s, bytes, &st, &mv)) refused = 1;
				else
				{
					n[0]++; wn[0]++;
					if (st) { bad[0]++; wbad[0]++; stale[0] += st; wstale[0] += st; }
					moved += mv; wmoved += mv;
					AVFrame *keep = av_frame_clone(f);
					if (keep)
					{
						held[nheld].f = keep;
						held[nheld].slot = s;
						held[nheld].bytes = bytes;
						held[nheld].age = 0;
						nheld++;
					}
				}
			}
			frames++;
			if (pts >= w_start + 10)
			{
				printf("mediacenter: zctest: %s-%s: decodifica %.1f ms (rallentata dai confronti); controllate %lld+%lld; "
					"pezzi vecchi subito %lld (in %lld immagini), dopo 2 immagini %lld (in %lld); pagine spostate %lld\n",
					zc_time(w_start, ta), zc_time(w_start + 10, tb), w_frames ? 1000 * w_decode / w_frames : 0,
					wn[0], wn[1], wstale[0], wbad[0], wstale[1], wbad[1], wmoved);
				w_start += 10;
				w_decode = 0;
				w_frames = 0;
				wn[0] = wn[1] = wbad[0] = wbad[1] = wstale[0] = wstale[1] = wmoved = 0;
			}
		}
		vq_clear();
		mc.pend_len = 0;
		if (refused) break;
	}
	for (int k = 0; k < nheld; k++) av_frame_free(&held[k].f);

	long long pages = 0, runs = 0;
	for (int i = 0; i < zc_nslots; i++)
	{
		zc_slot_t *s = &zc_slot[i];
		for (int p = 0; s->pfn && p < s->npages; p++)
		{
			uint64_t now = zc_phys_page(s->mem + (size_t)p * ZC_PAGE);
			pages++;
			if (now != s->pfn[p]) moved++;
			if (!p || now != zc_phys_page(s->mem + (size_t)(p - 1) * ZC_PAGE) + 1) runs++;
		}
	}

	printf("mediacenter: zctest: %d immagini in %.0f s\n", frames, now_seconds() - t0);
	if (clean_frames)
		printf("mediacenter: zctest: 1) decodifica senza confronti (0:00-0:10) %.1f ms per immagine (con audio); tempo disponibile %.1f ms\n",
			1000 * clean_decode / clean_frames, fps > 0 ? 1000 / fps : 0);
	printf("mediacenter: zctest:    zona bloccata: %d blocchi, %.1f MB, %d non riusciti, %d immagini fuori zona\n",
		zc_nslots, pages * ZC_PAGE / 1048576.0, zc_pool_fail, zc_not_pool);
	if (!pages || !zc_slot[0].pfn || !zc_slot[0].pfn[0])
		printf("mediacenter: zctest: 2) indirizzi fisici NON disponibili\n");
	else
		printf("mediacenter: zctest: 2) indirizzi fisici ok: %lld pagine, %lld spostamenti in tutto, in media %.1f pagine consecutive\n",
			pages, moved, runs ? (double)pages / runs : 0);
	if (refused || zc_devmem < 0)
		printf("mediacenter: zctest: 3) lettura senza cache RIFIUTATA da Linux (/dev/mem)\n");
	else
	{
		printf("mediacenter: zctest: 3) subito: %lld immagini controllate, %lld con pezzi vecchi, %lld pezzi vecchi da 32 byte\n",
			n[0], bad[0], stale[0]);
		printf("mediacenter: zctest: 3) dopo 2 immagini: %lld immagini controllate, %lld con pezzi vecchi, %lld pezzi vecchi da 32 byte\n",
			n[1], bad[1], stale[1]);
	}
	media_close();
	ret = 0;

restore:
	if (cua_set && (fp = fopen(cua, "w")))
	{
		fprintf(fp, "%s\n", cua_old);
		fclose(fp);
		printf("mediacenter: zctest: %s rimesso a %s\n", cua, cua_old);
	}
	return ret;
}
