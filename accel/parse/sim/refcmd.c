// Reference for acc_parse (step 2 of 4.0): decodes the H.264 video of a file
// with FFmpeg built with the player's patch (tools/mediacenter/
// ffmpeg-mediacenter.patch) plus ffmpeg-parse-dump.patch, on one thread, with
// the same hooks as the player (accel.c: a slot per picture, lowest free;
// deblocking on). Writes:
//   <prefix>.cmd  the commands the ARM writes today, per picture:
//                 'PIC1', word count, the words (A1 ... A4)
//   <prefix>.sl   for each CABAC slice, what the ARM would give acc_parse
//                 (slice descriptor + slice data, see mcpd_slice in the patch)
// Usage: refcmd file prefix [pictures]   (prefix - : one stream on stdout for pmodel -)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

struct mc_acc_pic { uint32_t *p, *end; };
struct mc_acc_hooks {
	void *opaque;
	struct mc_acc_pic *(*begin)(void *opaque, const AVFrame *f, int mb_w, int mb_h);
	void (*grow)(void *opaque, struct mc_acc_pic *pic, int words);
	void (*end)(void *opaque, struct mc_acc_pic *pic);
	int (*slot)(void *opaque, const AVFrame *f);
	void (*fail)(void *opaque, const char *why);
	int deblock;
};
extern struct mc_acc_hooks *mc_acc_hooks;
extern FILE *mc_pd_file;

#define SLOTS 64
static struct { int slot; } bufs[SLOTS];
static uint64_t used;
static uint8_t *dummy;
static size_t dummy_size;
static FILE *cmdf;
static long npics, nwords, nfail;

struct pic { struct mc_acc_pic h; uint32_t *data; size_t cap; };

static void buf_free(void *o, uint8_t *d) { (void)d; used &= ~(1ull << ((typeof(bufs[0]) *)o)->slot); }

static int get_buffer(AVCodecContext *s, AVFrame *f, int flags)
{
	if (f->format != AV_PIX_FMT_YUV420P) { nfail++; return avcodec_default_get_buffer2(s, f, flags); }
	int w = f->width, h = f->height, align[AV_NUM_DATA_POINTERS];
	avcodec_align_dimensions2(s, &w, &h, align);
	int ys = (w + 63) & ~63, cs = ((w + 1) / 2 + 63) & ~63, ch = (h + 1) / 2;
	size_t need = (size_t)ys * h + 2 * (size_t)cs * ch + 4096;
	if (!dummy) { dummy = aligned_alloc(64, (need + 63) & ~(size_t)63); dummy_size = need; }
	int slot = -1;
	for (int i = 0; i < SLOTS; i++) if (!(used >> i & 1)) { slot = i; break; }
	if (slot < 0) { fprintf(stderr, "refcmd: no free slot\n"); exit(1); }
	used |= 1ull << slot;
	bufs[slot].slot = slot;
	f->buf[0] = av_buffer_create(dummy, dummy_size, buf_free, &bufs[slot], 0);
	f->data[0] = dummy;
	f->data[1] = dummy + (size_t)ys * h;
	f->data[2] = f->data[1] + (size_t)cs * ch;
	f->linesize[0] = ys;
	f->linesize[1] = f->linesize[2] = cs;
	f->extended_data = f->data;
	return 0;
}

static int slot_of(void *o, const AVFrame *f)
{
	(void)o;
	if (!f || !f->buf[0]) return 255;
	void *b = av_buffer_get_opaque(f->buf[0]);
	if (b < (void *)bufs || b >= (void *)(bufs + SLOTS)) return 255;
	return ((typeof(bufs[0]) *)b)->slot;
}

static struct mc_acc_pic *begin(void *o, const AVFrame *f, int mb_w, int mb_h)
{
	struct pic *p = calloc(1, sizeof(*p));
	p->cap = 1 << 16;
	p->data = malloc(p->cap * 4);
	p->h.p = p->data;
	p->h.end = p->data + p->cap;
	*p->h.p++ = 0xA1000000u | slot_of(o, f);
	*p->h.p++ = (uint32_t)mb_w | (uint32_t)mb_h << 8;
	return &p->h;
}

static void grow(void *o, struct mc_acc_pic *h, int words)
{
	(void)o;
	struct pic *p = (struct pic *)h;
	size_t n = p->h.p - p->data;
	while (p->cap - n < (size_t)words + 16) p->cap *= 2;
	p->data = realloc(p->data, p->cap * 4);
	p->h.p = p->data + n;
	p->h.end = p->data + p->cap;
}

static void end(void *o, struct mc_acc_pic *h)
{
	(void)o;
	struct pic *p = (struct pic *)h;
	uint32_t n = p->h.p - p->data, hd[2] = { 0x50494331u, n };
	fwrite(hd, 4, 2, cmdf);
	fwrite(p->data, 4, n, cmdf);
	npics++;
	nwords += n;
	free(p->data);
	free(p);
}

static void fail(void *o, const char *why) { (void)o; nfail++; fprintf(stderr, "refcmd: not for the FPGA: %s\n", why); }

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: refcmd file prefix [pictures]\n"); return 2; }
	long maxp = argc > 3 ? atol(argv[3]) : 0;
	AVFormatContext *fc = NULL;
	if (avformat_open_input(&fc, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) { fprintf(stderr, "cannot open\n"); return 1; }
	int vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (vs < 0) return 1;
	char name[1024];
	static char b1[1 << 20], b2[1 << 20];
	if (!strcmp(argv[2], "-"))
	{
		// one stream on stdout, in decoding order: a picture's slices, then its commands
		cmdf = mc_pd_file = stdout;
		setvbuf(stdout, b1, _IOFBF, sizeof(b1));
	}
	else
	{
		snprintf(name, sizeof(name), "%s.cmd", argv[2]);
		cmdf = fopen(name, "wb");
		snprintf(name, sizeof(name), "%s.sl", argv[2]);
		mc_pd_file = fopen(name, "wb");
		if (!cmdf || !mc_pd_file) return 1;
		setvbuf(cmdf, b1, _IOFBF, sizeof(b1));
		setvbuf(mc_pd_file, b2, _IOFBF, sizeof(b2));
	}
	const AVCodec *dec = avcodec_find_decoder(fc->streams[vs]->codecpar->codec_id);
	AVCodecContext *ctx = avcodec_alloc_context3(dec);
	avcodec_parameters_to_context(ctx, fc->streams[vs]->codecpar);
	ctx->pkt_timebase = fc->streams[vs]->time_base;
	ctx->thread_count = 1;
	// as the player: light mode flags, then the accelerator with deblocking
	ctx->flags2 |= AV_CODEC_FLAG2_FAST;
	static struct mc_acc_hooks hooks;
	static int me;
	hooks.opaque = &me;
	hooks.begin = begin;
	hooks.grow = grow;
	hooks.end = end;
	hooks.slot = slot_of;
	hooks.fail = fail;
	hooks.deblock = 1;
	ctx->opaque = &me;
	ctx->get_buffer2 = get_buffer;
	ctx->skip_loop_filter = AVDISCARD_DEFAULT;
	mc_acc_hooks = &hooks;
	if (avcodec_open2(ctx, dec, NULL) < 0) return 1;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *fr = av_frame_alloc();
	long pkts = 0;
	while (av_read_frame(fc, pkt) >= 0)
	{
		if (pkt->stream_index == vs)
		{
			avcodec_send_packet(ctx, pkt);
			pkts++;
			while (avcodec_receive_frame(ctx, fr) == 0) av_frame_unref(fr);
		}
		av_packet_unref(pkt);
		if (maxp && pkts >= maxp) break;
	}
	avcodec_send_packet(ctx, NULL);
	while (avcodec_receive_frame(ctx, fr) == 0) av_frame_unref(fr);
	avcodec_free_context(&ctx);
	fclose(cmdf);
	if (mc_pd_file != cmdf) fclose(mc_pd_file);
	fprintf(stderr, "refcmd: %ld packets, %ld pictures, %ld words, %ld not for the FPGA\n", pkts, npics, nwords, nfail);
	return 0;
}
