// Test of the accelerator's ARM side on the PC (FFmpeg built with
// tools/mediacenter/ffmpeg-mediacenter.patch): decodes an H.264 file twice,
// once normally (the expected pictures) and once with the accelerator hooks
// of tools/mediacenter/accel.c (only the commands, as on the MiSTer, with the
// player's two threads), and writes <prefix>.cmd (the commands, in the order
// the FPGA gets them) and <prefix>.rec (FFmpeg's pictures in the same order),
// for accel/sim/model.py and the test benches. With deblocking (command A5),
// as the player with a core that filters; MC_NODEBLOCK=1: without.
//   acctest <file> <prefix> [threads] [pictures held, like the player's queue]
// Built with -DACC_FAKE, the commands go through the player's link with the
// core (tools/mediacenter/accel_link.c: ring, sessions, waiting for each
// picture) to a pretend core, a thread that reads the ring as mc_acc does,
// at random speeds, and writes what it read into <prefix>.cmd.
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
#ifdef ACC_FAKE
#include <unistd.h>
#include <fcntl.h>
#include "../../tools/mediacenter/accel_link.c"

static uint32_t fake_mb[2048] __attribute__((aligned(4096)));
static uint8_t fake_ring[ACC_RING_PAGES * 4096] __attribute__((aligned(4096)));
static volatile int fake_stop;
static unsigned fake_pics, fake_sessions, fake_bad;

void *acc_fake_map(uint32_t phys, size_t len, void *at)
{
	(void)at;
	if (phys == ACC_MBOX && len <= sizeof(fake_mb)) return fake_mb;
	if (phys == ACC_RING_SO && len <= sizeof(fake_ring)) return fake_ring;
	return NULL;
}

// mc_acc, as the player sees it: the mailbox, the ring, the answer
static void *fake_core(void *arg)
{
	(void)arg;
	volatile uint32_t *m = fake_mb;
	uint32_t session = m[1], rd = 0, pics = 0;
	int live = 0;
	// command parser: words left in the current command
	int left = 0, stage = 0, type = 0;
	unsigned seed = 1;
	while (!fake_stop)
	{
		usleep(rand_r(&seed) % 300);
		if (m[0] == ACC_MAGIC && m[1] != session)
		{
			session = m[1];
			rd = pics = 0;
			left = stage = 0;
			live = 1;
			fake_sessions++;
			m[18] = 0; m[19] = 0; m[20] = 0;
			__sync_synchronize();
			m[17] = session;
			m[16] = ACC_OKAY;
			continue;
		}
		if (!live || m[0] != ACC_MAGIC) { live = 0; continue; }
		uint32_t wp = m[4];
		uint32_t n = (wp - rd) / 8 * 8;
		uint32_t take = n ? (rand_r(&seed) % 4 ? n : (rand_r(&seed) % (n / 8 + 1)) * 8) : 0;
		for (uint32_t i = 0; i < take; i += 4)
		{
			uint32_t w;
			memcpy(&w, fake_ring + (rd + i) % sizeof(fake_ring), 4);
			fwrite(&w, 4, 1, fcmd);
			if (left) { left--; if (!left && stage == 1) stage = 2; continue; }
			if (stage == 2) { left = (int)w; stage = 0; continue; } // coefficient count
			uint32_t tag = w >> 24;
			if (tag == 0xA1) left = 1;
			else if (tag == 0xA2) left = 3 * ((w & 63) + ((w >> 12 & 1) ? (w >> 6 & 63) : 0));
			else if (tag == 0xA3)
			{
				type = w >> 16 & 0xFF;
				if (type == 4) left = 96;
				else { left = type ? 5 : 37; stage = 1; }
			}
			else if (tag == 0xA4) pics++;
			else if (tag == 0xA5) left = 2;
			else if (tag != 0xA0) { fake_bad++; m[20] = 1; }
		}
		rd += take;
		m[19] = pics;
		m[18] = rd;
		fake_pics = pics;
	}
	return NULL;
}
#endif
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
#ifdef ACC_FAKE
		if (b && al_wait(b) != 1) printf("acctest: picture %u not finished by the core\n", b->seq);
#endif
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
	int seek_at = argc > 5 ? atoi(argv[5]) : 0; // ACC_FAKE: a seek back to the start after this many pictures
	AVFormatContext *fmt = NULL;
	if (avformat_open_input(&fmt, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) return 1;
	int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (vs < 0) return 1;
	const AVCodec *c = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
	AVCodecContext *cn = avcodec_alloc_context3(c), *ca = avcodec_alloc_context3(c);
	avcodec_parameters_to_context(cn, fmt->streams[vs]->codecpar);
	avcodec_parameters_to_context(ca, fmt->streams[vs]->codecpar);
	cn->thread_count = 1;
	// deblocking in the FPGA (command A5) as the player does, unless MC_NODEBLOCK=1
	const char *nd = getenv("MC_NODEBLOCK");
	acc.hooks.deblock = !(nd && atoi(nd));
	cn->skip_loop_filter = acc.hooks.deblock ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
	cn->flags2 |= AV_CODEC_FLAG2_FAST;
	ca->thread_count = threads;
	ca->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	ca->flags2 |= AV_CODEC_FLAG2_FAST;
	char name[1024];
	snprintf(name, sizeof(name), "%s.cmd", argv[2]);
	fcmd = fopen(name, "wb");
	acc.send = send_file;
#ifdef ACC_FAKE
	pthread_t fc;
	pthread_create(&fc, NULL, fake_core, NULL);
	char why[96];
	if (al_open(why, sizeof(why))) { printf("acctest: link: %s\n", why); return 1; }
	acc.send = al_send;
#endif
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
#ifdef ACC_FAKE
		if (seek_at && npics >= seek_at)
		{
			seek_at = 0;
			av_seek_frame(fmt, vs, 0, AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers(cn);
			avcodec_flush_buffers(ca);
			while (na) av_frame_free(&qa[--na]);
			while (nn) av_frame_free(&qn[--nn]);
			al_restart();
			printf("acctest: seek after %d pictures\n", npics);
			npics = 0; // the check of the numbers starts again
		}
#endif
	}
#ifdef ACC_FAKE
	// everything sent has been read
	for (int i = 0; i < 2000 && al_read_done() != al.wp; i++) usleep(1000);
	fake_stop = 1;
	pthread_join(fc, NULL);
	printf("acctest: pretend core: %u sessions, %u pictures, %u bad words, ring waits %u (%.2f s)\n",
		fake_sessions, fake_pics, fake_bad, al.ring_waits, al.ring_wait_t);
#endif
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
