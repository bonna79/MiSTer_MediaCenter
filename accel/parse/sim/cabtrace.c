// CABAC trace from FFmpeg (built with ffmpeg-cabac-trace.patch): decodes the
// H.264 video of a file on one thread and writes every CABAC bin outside the
// residual blocks, and every residual block's coefficients, to stdout.
// Format: 16-bit little endian words.
//   0x0000-0x07FF  decision: context (10 bits) | bin << 10
//   0x0800 | bin   bypass;  0x0802 | bin  terminate
//   0x2000 | cat << 7 | n, then n x (position, level low, level high)  (last coefficient first)
//   0x3000, type (0 P, 1 B, 2 I), cabac_init_idc, qp, flags, bytes low, bytes high, data (padded to even)
//   0x3100, offset low, offset high    I_PCM samples start (byte offset in the slice data)
//   0x3F00, picture number             end of a picture
// Usage: cabtrace file [max pictures] > trace
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

typedef struct MMCCabTrace { FILE *f; const uint8_t *base, *s0; int inres; } MMCCabTrace;
extern MMCCabTrace ff_mmc_cabtr;

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: cabtrace file [pictures]\n"); return 2; }
	long maxp = argc > 2 ? atol(argv[2]) : 0;
	AVFormatContext *fc = NULL;
	if (avformat_open_input(&fc, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) { fprintf(stderr, "cannot open\n"); return 1; }
	int vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (vs < 0) return 1;
	const AVCodec *dec = avcodec_find_decoder(fc->streams[vs]->codecpar->codec_id);
	AVCodecContext *ctx = avcodec_alloc_context3(dec);
	avcodec_parameters_to_context(ctx, fc->streams[vs]->codecpar);
	ctx->thread_count = 1;
	if (avcodec_open2(ctx, dec, NULL) < 0) return 1;
	static char buf[1 << 20];
	setvbuf(stdout, buf, _IOFBF, sizeof(buf));
	ff_mmc_cabtr.f = stdout;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *fr = av_frame_alloc();
	long pics = 0, pkts = 0;
	while (av_read_frame(fc, pkt) >= 0)
	{
		if (pkt->stream_index == vs)
		{
			avcodec_send_packet(ctx, pkt);
			uint16_t w[2] = { 0x3F00, (uint16_t)pkts++ };
			fwrite(w, 2, 2, stdout);
			while (avcodec_receive_frame(ctx, fr) == 0) pics++;
		}
		av_packet_unref(pkt);
		if (maxp && pkts >= maxp) break;
	}
	fflush(stdout);
	fprintf(stderr, "cabtrace: %ld packets, %ld pictures\n", pkts, pics);
	return 0;
}
