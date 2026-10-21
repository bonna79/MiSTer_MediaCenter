// mediacenter --zctest <file>: feasibility test of "zero-copy" (the FPGA
// reading the decoded pictures straight from memory instead of the ARM
// copying them into the framebuffer). Nothing here is used when playing.
//
// The pictures are decoded into a pool of locked memory, and the test checks:
//  1. the decoding speed with the pool (to compare with --bench);
//  2. that Linux gives the physical address of each page of the pool, and
//     that the pages do not move while decoding;
//  3. what the FPGA would read: each tracked picture is read again through
//     /dev/mem without the caches (O_SYNC), as DDR holds it, and compared
//     with what the ARM sees. This is done when the decoder hands the
//     picture over and 1 and 2 pictures later, half of the pictures with the
//     ARM's cacheflush first and half without;
//  4. what the cacheflush costs.

#include <sys/mman.h>

#ifndef __ARM_NR_cacheflush
#define __ARM_NR_cacheflush 0x0f0002
#endif

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

// Bytes of the slot's first n bytes that differ between the ARM's view and
// DDR (read through /dev/mem without the caches). -1: /dev/mem refused.
static long long zc_compare(zc_slot_t *s, size_t n)
{
	long long diff = 0;
	int pages = (int)((n + ZC_PAGE - 1) / ZC_PAGE);
	for (int p = 0; p < pages;)
	{
		// a run of physically consecutive pages, mapped at once
		int run = 1;
		while (p + run < pages && s->pfn[p + run] == s->pfn[p] + (uint64_t)run) run++;
		size_t len = (size_t)run * ZC_PAGE;
		const uint8_t *u = (const uint8_t *)mmap(NULL, len, PROT_READ, MAP_SHARED, zc_devmem, (off_t)(s->pfn[p] * ZC_PAGE));
		if (u == MAP_FAILED) return -1;
		const uint8_t *c = s->mem + (size_t)p * ZC_PAGE;
		if (memcmp(u, c, len))
			for (size_t i = 0; i < len; i++) diff += u[i] != c[i];
		munmap((void *)u, len);
		p += run;
	}
	return diff;
}

typedef struct
{
	AVFrame *f;
	zc_slot_t *slot;
	size_t bytes;
	int age, flushed;
} zc_held_t;

static int zctest(const char *path)
{
	printf("mediacenter: zctest: prova zero-copy su %s\n", path);
	zc_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	zc_devmem = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
	if (zc_pagemap < 0) printf("mediacenter: zctest: /proc/self/pagemap non apribile (%s)\n", strerror(errno));
	if (zc_devmem < 0) printf("mediacenter: zctest: /dev/mem non apribile (%s)\n", strerror(errno));

	zc_get_buffer2 = zc_get_buffer;
	if (media_open(path)) return 1;
	if (mc.vstream < 0 || !mc.vdec)
	{
		printf("mediacenter: zctest: il file non ha video\n");
		media_close();
		return 1;
	}

	// [flushed][age]: pictures compared, pictures with differences, bytes
	long long cmp_n[2][3] = { { 0 } }, cmp_bad[2][3] = { { 0 } }, cmp_bytes[2][3] = { { 0 } };
	int devmem_refused = 0, no_phys = 0;
	double t_decode = 0, t_flush = 0;
	int flushes = 0, frames = 0;
	zc_held_t held[4];
	int nheld = 0;
	double t0 = now_seconds();

	for (int more = 1; more && !quit_requested;)
	{
		double media = !isnan(mc.last_vpts) ? mc.last_vpts : mc.audio_end;
		if (media >= 30.0) break;
		double a = now_seconds();
		more = decode_step();
		t_decode += now_seconds() - a;
		for (int i = 0; i < mc.vq_count; i++)
		{
			AVFrame *f = mc.vq[i].frame;
			// pictures held from before are now one picture older
			for (int k = 0; k < nheld;)
			{
				zc_held_t *h = &held[k];
				h->age++;
				long long d = devmem_refused ? -1 : zc_compare(h->slot, h->bytes);
				if (d < 0) devmem_refused = 1;
				else
				{
					cmp_n[h->flushed][h->age]++;
					if (d) { cmp_bad[h->flushed][h->age]++; cmp_bytes[h->flushed][h->age] += d; }
				}
				if (h->age >= 2)
				{
					av_frame_free(&h->f);
					held[k] = held[--nheld];
				}
				else k++;
			}
			// every 8th picture is tracked, alternately with and without cacheflush
			zc_slot_t *s = f->data[0] ? zc_slot_of(f->data[0]) : NULL;
			if (frames % 8 == 0 && s && nheld < 4)
			{
				if (!s->pfn || !s->pfn[0]) no_phys = 1;
				else
				{
					int flush = (frames / 8) & 1;
					size_t bytes = (size_t)(f->data[2] - f->data[0]) + (size_t)f->linesize[2] * ((f->height + 1) / 2);
					if (flush)
					{
						double b = now_seconds();
						syscall(__ARM_NR_cacheflush, (uintptr_t)s->mem, (uintptr_t)s->mem + bytes, 0);
						t_flush += now_seconds() - b;
						flushes++;
					}
					long long d = devmem_refused ? -1 : zc_compare(s, bytes);
					if (d < 0) devmem_refused = 1;
					else
					{
						cmp_n[flush][0]++;
						if (d) { cmp_bad[flush][0]++; cmp_bytes[flush][0] += d; }
					}
					AVFrame *keep = av_frame_clone(f);
					if (keep && !devmem_refused)
					{
						held[nheld].f = keep;
						held[nheld].slot = s;
						held[nheld].bytes = bytes;
						held[nheld].age = 0;
						held[nheld].flushed = flush;
						nheld++;
					}
					else av_frame_free(&keep);
				}
			}
			frames++;
		}
		vq_clear();
		mc.pend_len = 0;
	}
	for (int k = 0; k < nheld; k++) av_frame_free(&held[k].f);

	// did the pages move?
	long long pages = 0, moved = 0, runs = 0;
	for (int i = 0; i < zc_nslots; i++)
	{
		zc_slot_t *s = &zc_slot[i];
		for (int p = 0; s->pfn && p < s->npages; p++)
		{
			pages++;
			if (zc_phys_page(s->mem + (size_t)p * ZC_PAGE) != s->pfn[p]) moved++;
			if (!p || s->pfn[p] != s->pfn[p - 1] + 1) runs++;
		}
	}

	double secs = now_seconds() - t0;
	double media = !isnan(mc.last_vpts) ? mc.last_vpts : mc.audio_end;
	double fps = av_q2d(mc.fmt->streams[mc.vstream]->avg_frame_rate);
	printf("mediacenter: zctest: %d immagini, %.1f s di video in %.1f s (con i confronti)\n", frames, media, secs);
	if (frames)
		printf("mediacenter: zctest: 1) decodifica %.1f ms per immagine (con audio); tempo disponibile %.1f ms\n",
			1000 * t_decode / frames, fps > 0 ? 1000 / fps : 0);
	printf("mediacenter: zctest:    zona bloccata: %d blocchi, %.1f MB, %d non riusciti, %d immagini fuori zona\n",
		zc_nslots, pages * ZC_PAGE / 1048576.0, zc_pool_fail, zc_not_pool);
	if (no_phys || !pages || zc_slot[0].pfn == NULL || !zc_slot[0].pfn[0])
		printf("mediacenter: zctest: 2) indirizzi fisici NON disponibili\n");
	else
		printf("mediacenter: zctest: 2) indirizzi fisici ok: %lld pagine, %lld spostate, in media %.1f pagine consecutive\n",
			pages, moved, runs ? (double)pages / runs : 0);
	if (devmem_refused || zc_devmem < 0)
		printf("mediacenter: zctest: 3) lettura senza cache RIFIUTATA da Linux (/dev/mem): la prova la fara' l'FPGA\n");
	else
		for (int fl = 0; fl < 2; fl++)
			for (int age = 0; age < 3; age++)
				printf("mediacenter: zctest: 3) %s, %s: %lld immagini confrontate, %lld con differenze, %lld byte diversi\n",
					fl ? "con cacheflush" : "senza cacheflush",
					age == 0 ? "subito" : age == 1 ? "dopo 1 immagine" : "dopo 2 immagini",
					cmp_n[fl][age], cmp_bad[fl][age], cmp_bytes[fl][age]);
	if (flushes)
		printf("mediacenter: zctest: 4) cacheflush %.2f ms per immagine\n", 1000 * t_flush / flushes);
	media_close();
	return 0;
}
