// MiSTer MediaCenter FPGA accelerator, the ARM side (included by
// mediacenter.c, and by accel/sim/acctest.c on the PC).
//
// FFmpeg (our ffmpeg-mediacenter.patch, h264_mcacc.c) only reads the H.264
// stream and writes, for every picture, the commands of the FPGA engine
// (accel/doc/comandi.md) into a buffer from here. Pictures are numbered in
// decoding order when they start; with two decoding threads two pictures are
// written at the same time, so a finished picture waits here until all the
// earlier ones are sent (acc_send, the ring buffer to the FPGA).
//
// Every picture the decoder allocates gets a slot of the FPGA memory (4 MB,
// 64 slots): the engine rebuilds it there and the zero-copy block shows it.
// The slot stays taken while FFmpeg or the player hold the picture. The
// picture's own pixel memory is one shared dummy buffer: FFmpeg writes into
// it only to paint missing pictures grey, nothing reads it.

#define ACC_SLOTS 64
#define ACC_INFLIGHT 16                     // pictures being written at once (at most)

struct mc_acc_pic {
	uint32_t *p, *end;                       // FFmpeg's part (h264_mcacc.c)
	uint32_t *data;
	size_t cap;                              // words
	unsigned seq;                            // decoding order
	unsigned session;
	int done;
};

struct mc_acc_hooks {
	void *opaque;
	struct mc_acc_pic *(*begin)(void *opaque, const AVFrame *f, int mb_w, int mb_h);
	void (*grow)(void *opaque, struct mc_acc_pic *pic, int words);
	void (*end)(void *opaque, struct mc_acc_pic *pic);
	int (*slot)(void *opaque, const AVFrame *f);
	void (*fail)(void *opaque, const char *why);
};
extern struct mc_acc_hooks *mc_acc_hooks;    // in our FFmpeg

typedef struct {
	int slot;
	unsigned seq;                            // decoding order of the picture in it
	unsigned session;
} acc_buf_t;

static struct {
	pthread_mutex_t lock;
	struct mc_acc_hooks hooks;
	int on;
	volatile int failed;
	char why[96];
	unsigned session;
	unsigned next_seq;                       // the next picture to start
	unsigned next_send;                      // the next picture to send
	struct mc_acc_pic *inflight[ACC_INFLIGHT];
	struct mc_acc_pic *spare[ACC_INFLIGHT];
	int nspare;
	uint64_t slots_used;                     // bit per slot
	int slots_peak;
	acc_buf_t bufs[ACC_SLOTS];
	uint8_t *dummy;                          // the shared pixel memory
	size_t dummy_size;
	int dummy_ls;
	// sends a finished picture's commands (an even number of words)
	int (*send)(const uint32_t *w, size_t n, unsigned seq);
	// counters
	unsigned pics_sent, slot_waits, pics_dropped;
	uint64_t words_sent;
	double send_time;
} acc = { .lock = PTHREAD_MUTEX_INITIALIZER };

static double acc_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void acc_fail(void *o, const char *why)
{
	(void)o;
	if (!acc.failed) snprintf(acc.why, sizeof(acc.why), "%s", why);
	acc.failed = 1;
}

// ---------------------------------------------------------------- slots

static void acc_buf_free(void *opaque, uint8_t *data)
{
	(void)data;
	acc_buf_t *b = (acc_buf_t *)opaque;
	pthread_mutex_lock(&acc.lock);
	acc.slots_used &= ~(1ull << b->slot);
	pthread_mutex_unlock(&acc.lock);
}

// The decoder's pictures: a slot each, pixels in the shared dummy memory.
static int acc_get_buffer(AVCodecContext *s, AVFrame *f, int flags)
{
	(void)flags;
	if (f->format != AV_PIX_FMT_YUV420P)
	{
		acc_fail(NULL, "not 8-bit 4:2:0");
		return avcodec_default_get_buffer2(s, f, flags);
	}
	int w = f->width, h = f->height, align[AV_NUM_DATA_POINTERS];
	avcodec_align_dimensions2(s, &w, &h, align);
	int ys = (w + 63) & ~63, cs = ((w + 1) / 2 + 63) & ~63, ch = (h + 1) / 2;
	size_t need = (size_t)ys * h + 2 * (size_t)cs * ch + 4096;
	if (need > acc.dummy_size || ys != acc.dummy_ls)
	{
		// sized once for the stream (FFmpeg's threads are not running yet)
		free(acc.dummy);
		acc.dummy = (uint8_t *)aligned_alloc(64, (need + 63) & ~(size_t)63);
		acc.dummy_size = acc.dummy ? need : 0;
		acc.dummy_ls = ys;
		if (!acc.dummy) return AVERROR(ENOMEM);
	}
	pthread_mutex_lock(&acc.lock);
	int slot = -1;
	for (int i = 0; i < ACC_SLOTS; i++)
		if (!(acc.slots_used >> i & 1)) { slot = i; break; }
	if (slot >= 0)
	{
		acc.slots_used |= 1ull << slot;
		int n = __builtin_popcountll(acc.slots_used);
		if (n > acc.slots_peak) acc.slots_peak = n;
	}
	pthread_mutex_unlock(&acc.lock);
	if (slot < 0)
	{
		acc_fail(NULL, "no free picture slot in the FPGA memory");
		return AVERROR(ENOMEM);
	}
	acc_buf_t *b = &acc.bufs[slot];
	b->slot = slot;
	b->seq = 0;
	b->session = acc.session;
	f->buf[0] = av_buffer_create(acc.dummy, acc.dummy_size, acc_buf_free, b, 0);
	if (!f->buf[0])
	{
		acc_buf_free(b, NULL);
		return AVERROR(ENOMEM);
	}
	f->data[0] = acc.dummy;
	f->data[1] = acc.dummy + (size_t)ys * h;
	f->data[2] = f->data[1] + (size_t)cs * ch;
	f->linesize[0] = ys;
	f->linesize[1] = f->linesize[2] = cs;
	f->extended_data = f->data;
	return 0;
}

static acc_buf_t *acc_buf_of(const AVFrame *f)
{
	if (!f || !f->buf[0] || av_buffer_get_opaque(f->buf[0]) < (void *)acc.bufs ||
	    av_buffer_get_opaque(f->buf[0]) >= (void *)(acc.bufs + ACC_SLOTS)) return NULL;
	return (acc_buf_t *)av_buffer_get_opaque(f->buf[0]);
}

static int acc_slot(void *o, const AVFrame *f)
{
	(void)o;
	acc_buf_t *b = acc_buf_of(f);
	return b ? b->slot : 255;
}

// ---------------------------------------------------------------- pictures

static struct mc_acc_pic *acc_begin(void *o, const AVFrame *f, int mb_w, int mb_h)
{
	(void)o;
	pthread_mutex_lock(&acc.lock);
	struct mc_acc_pic *p = acc.nspare ? acc.spare[--acc.nspare] : (struct mc_acc_pic *)calloc(1, sizeof(*p));
	if (p && !p->data)
	{
		p->cap = 1 << 18; // 1 MB, more when needed
		p->data = (uint32_t *)malloc(p->cap * 4);
		if (!p->data) { free(p); p = NULL; }
	}
	if (!p || acc.next_seq - acc.next_send >= ACC_INFLIGHT)
	{
		pthread_mutex_unlock(&acc.lock);
		acc_fail(NULL, "too many pictures at once");
		return NULL;
	}
	p->seq = acc.next_seq++;
	p->session = acc.session;
	p->done = 0;
	acc.inflight[p->seq % ACC_INFLIGHT] = p;
	acc_buf_t *b = acc_buf_of(f);
	if (b) b->seq = p->seq;
	pthread_mutex_unlock(&acc.lock);
	p->p = p->data;
	p->end = p->data + p->cap;
	*p->p++ = 0xA1000000u | (b ? b->slot : 255);
	*p->p++ = (uint32_t)mb_w | (uint32_t)mb_h << 8;
	return p;
}

static void acc_grow(void *o, struct mc_acc_pic *p, int words)
{
	(void)o;
	size_t used = p->p - p->data;
	size_t cap = p->cap;
	while (cap - used < (size_t)words + 2) cap *= 2;
	uint32_t *d = (uint32_t *)realloc(p->data, cap * 4);
	if (!d)
	{
		// nothing more fits: the rest of this picture is lost
		acc_fail(NULL, "out of memory for the commands");
		p->p = p->data + 2;
		return;
	}
	p->data = d;
	p->cap = cap;
	p->p = d + used;
	p->end = d + cap;
}

// A picture's commands are complete: send it and the ones after it that
// were waiting, in decoding order.
static void acc_end(void *o, struct mc_acc_pic *p)
{
	(void)o;
	if ((p->p - p->data) & 1) *p->p++ = 0xA0000000u; // even: the FPGA reads 64-bit words
	pthread_mutex_lock(&acc.lock);
	p->done = 1;
	if (p->session != acc.session)
	{
		// from before a seek: dropped
		if (acc.inflight[p->seq % ACC_INFLIGHT] == p) acc.inflight[p->seq % ACC_INFLIGHT] = NULL;
		acc.pics_dropped++;
		acc.spare[acc.nspare++] = p;
		pthread_mutex_unlock(&acc.lock);
		return;
	}
	for (;;)
	{
		struct mc_acc_pic *q = acc.inflight[acc.next_send % ACC_INFLIGHT];
		if (acc.next_send == acc.next_seq || !q || !q->done || q->seq != acc.next_send) break;
		double t = acc_now();
		if (acc.send && !acc.failed && acc.send(q->data, q->p - q->data, q->seq) < 0)
			acc_fail(NULL, "the FPGA does not answer");
		acc.send_time += acc_now() - t;
		acc.pics_sent++;
		acc.words_sent += q->p - q->data;
		acc.inflight[acc.next_send % ACC_INFLIGHT] = NULL;
		acc.spare[acc.nspare++] = q;
		acc.next_send++;
	}
	pthread_mutex_unlock(&acc.lock);
}

// Turns the accelerator on for this decoder (before avcodec_open2).
static void acc_attach(AVCodecContext *ctx)
{
	acc.hooks.opaque = &acc;
	acc.hooks.begin = acc_begin;
	acc.hooks.grow = acc_grow;
	acc.hooks.end = acc_end;
	acc.hooks.slot = acc_slot;
	acc.hooks.fail = acc_fail;
	ctx->opaque = &acc;
	ctx->get_buffer2 = acc_get_buffer;
	ctx->skip_loop_filter = AVDISCARD_ALL; // no deblocking in the FPGA yet
	acc.failed = 0;
	acc.why[0] = 0;
	acc.on = 1;
	mc_acc_hooks = &acc.hooks;
}

// A new start (file opened, seek): pictures still being written are dropped.
static void acc_restart(void)
{
	pthread_mutex_lock(&acc.lock);
	acc.session++;
	for (int i = 0; i < ACC_INFLIGHT; i++)
		if (acc.inflight[i] && acc.inflight[i]->done)
		{
			acc.spare[acc.nspare++] = acc.inflight[i];
			acc.inflight[i] = NULL;
		}
		else if (acc.inflight[i]) acc.inflight[i] = NULL; // its end() returns it
	acc.next_send = acc.next_seq;
	pthread_mutex_unlock(&acc.lock);
}

static void acc_detach(void)
{
	mc_acc_hooks = NULL;
	acc.on = 0;
}
