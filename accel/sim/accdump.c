// Test bench helper (host): decodes an H.264 file with the patched FFmpeg
// (ffmpeg-accel-dump.patch) the way the player does in light mode, but with
// one thread, and writes the FPGA commands and the expected pictures:
//   accdump <file> <prefix> [pictures]
#include <stdio.h>
#include <stdlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: accdump <file> <prefix> [pictures]\n"); return 1; }
	int maxpics = argc > 3 ? atoi(argv[3]) : 0;
	AVFormatContext *fmt = NULL;
	if (avformat_open_input(&fmt, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) return 1;
	setenv("MC_ACCDUMP", argv[2], 1); // not while probing the file
	int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (vs < 0) return 1;
	const AVCodec *c = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
	AVCodecContext *ctx = avcodec_alloc_context3(c);
	avcodec_parameters_to_context(ctx, fmt->streams[vs]->codecpar);
	ctx->thread_count = 1;
	ctx->skip_loop_filter = AVDISCARD_ALL; // light mode, as on the MiSTer
	ctx->flags2 |= AV_CODEC_FLAG2_FAST;
	if (avcodec_open2(ctx, c, NULL) < 0) return 1;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *f = av_frame_alloc();
	int pics = 0;
	while (av_read_frame(fmt, pkt) >= 0 && (!maxpics || pics < maxpics))
	{
		if (pkt->stream_index == vs)
		{
			avcodec_send_packet(ctx, pkt);
			while (avcodec_receive_frame(ctx, f) >= 0) { pics++; av_frame_unref(f); }
		}
		av_packet_unref(pkt);
	}
	avcodec_send_packet(ctx, NULL);
	while (avcodec_receive_frame(ctx, f) >= 0) pics++;
	fprintf(stderr, "accdump: %d pictures\n", pics);
	avcodec_free_context(&ctx);
	avformat_close_input(&fmt);
	return 0;
}
