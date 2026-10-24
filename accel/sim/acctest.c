// Test of the accelerator's ARM side on the PC (FFmpeg built with
// tools/mediacenter/ffmpeg-mediacenter.patch): decodes an H.264 file twice,
// once normally (the expected pictures) and once with the accelerator hooks
// of tools/mediacenter/accel.c (only the commands, as on the MiSTer, with the
// player's two threads), and writes <prefix>.cmd (the commands, in the order
// the FPGA gets them) and <prefix>.rec (FFmpeg's pictures in the same order),
// for accel/sim/model.py and the test benches.
//   acctest <file> <prefix> [threads] [pictures held, like the player's queue]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "../../tools/mediacenter/accel.c"

static FILE *fcmd;
static int send_file(const uint32_t *w, size_t n, unsigned seq)
{
	(void)seq;
	return fwrite(w, 4, n, fcmd) == n ? 0 : -1;
}

typedef struct { unsigned seq; int slot; uint8_t *pix; size_t n; } pic_t;
static pic_t *pics;
static int npics, cap;
static AVFrame *qa[256], *qn[256];
static int na, nn;
static AVFrame *held[64];
static int nheld, keep;
static int W, H;

static void pair(void)
{
	while (na && nn)
	{
		AVFrame *a = qa[0], *n = qn[0];
		memmove(qa, qa + 1, --na * sizeof(*qa));
		memmove(qn, qn + 1, --nn * sizeof(*qn));
		acc_buf_t *b = acc_buf_of(a);
		if (npics == cap) { cap = cap ? cap * 2 : 64; pics = realloc(pics, cap * sizeof(*pics)); }
		pic_t *p = &pics[npics++];
		p->seq = b ? b->seq : ~0u;
		p->slot = b ? b->slot : -1;
		p->n = (size_t)W * H * 3 / 2;
		p->pix = malloc(p->n);
		uint8_t *o = p->pix;
		for (int pl = 0; pl < 3; pl++)
			for (int y = 0; y < (pl ? H / 2 : H); y++, o += pl ? W / 2 : W)
				memcpy(o, n->data[pl] + (size_t)y * n->linesize[pl], pl ? W / 2 : W);
		av_frame_free(&n);
		// held for a while, like the player's queue
		held[nheld++] = a;
		if (nheld > keep) { av_frame_free(&held[0]); memmove(held, held + 1, --nheld * sizeof(*held)); }
	}
}

static int cmp_seq(const void *x, const void *y)
{
	const pic_t *a = x, *b = y;
	return a->seq < b->seq ? -1 : a->seq > b->seq;
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: acctest <file> <prefix> [threads] [held]\n"); return 1; }
	int threads = argc > 3 ? atoi(argv[3]) : 2;
	keep = argc > 4 ? atoi(argv[4]) : 12;
	AVFormatContext *fmt = NULL;
	if (avformat_open_input(&fmt, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) return 1;
	int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (vs < 0) return 1;
	const AVCodec *c = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
	AVCodecContext *cn = avcodec_alloc_context3(c), *ca = avcodec_alloc_context3(c);
	avcodec_parameters_to_context(cn, fmt->streams[vs]->codecpar);
	avcodec_parameters_to_context(ca, fmt->streams[vs]->codecpar);
	cn->thread_count = 1;
	cn->skip_loop_filter = AVDISCARD_ALL;
	cn->flags2 |= AV_CODEC_FLAG2_FAST;
	ca->thread_count = threads;
	ca->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	ca->flags2 |= AV_CODEC_FLAG2_FAST;
	char name[1024];
	snprintf(name, sizeof(name), "%s.cmd", argv[2]);
	fcmd = fopen(name, "wb");
	acc.send = send_file;
	acc_attach(ca);
	if (avcodec_open2(cn, c, NULL) < 0 || avcodec_open2(ca, c, NULL) < 0) return 1;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *f = av_frame_alloc();
	double t0 = acc_now();
	for (int eof = 0; !eof;)
	{
		if (av_read_frame(fmt, pkt) < 0) { eof = 1; avcodec_send_packet(cn, NULL); avcodec_send_packet(ca, NULL); }
		else if (pkt->stream_index == vs) { avcodec_send_packet(cn, pkt); avcodec_send_packet(ca, pkt); }
		av_packet_unref(pkt);
		while (avcodec_receive_frame(cn, f) >= 0) { qn[nn++] = av_frame_clone(f); av_frame_unref(f); }
		while (avcodec_receive_frame(ca, f) >= 0) { qa[na++] = av_frame_clone(f); av_frame_unref(f); }
		W = cn->coded_width; H = cn->coded_height;
		pair();
	}
	fclose(fcmd);
	while (nheld) av_frame_free(&held[--nheld]);
	qsort(pics, npics, sizeof(*pics), cmp_seq);
	snprintf(name, sizeof(name), "%s.rec", argv[2]);
	FILE *fr = fopen(name, "wb");
	for (int i = 0; i < npics; i++)
	{
		uint32_t hdr[3] = { (uint32_t)pics[i].slot, (uint32_t)W, (uint32_t)H };
		fwrite(hdr, 4, 3, fr);
		fwrite(pics[i].pix, 1, pics[i].n, fr);
	}
	fclose(fr);
	printf("acctest: %d pictures (%u sent, %u dropped), %.1f words a picture, slots at most %d, %s%s, %.2f s\n",
		npics, acc.pics_sent, acc.pics_dropped, acc.pics_sent ? (double)acc.words_sent / acc.pics_sent : 0.0,
		acc.slots_peak, acc.failed ? "FAILED: " : "ok", acc.why, acc_now() - t0);
	for (int i = 0; i < npics; i++)
		if (pics[i].seq != (unsigned)i) { printf("acctest: picture %d has number %u\n", i, pics[i].seq); return 1; }
	return acc.failed;
}
