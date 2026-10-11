#!/bin/sh
# Cross-compiles a minimal static FFmpeg for the DE10-Nano HPS (Cortex-A9,
# NEON) with only the decoders, demuxers and parsers MiSTer MediaCenter uses.
#
# usage: build_ffmpeg.sh <ffmpeg source dir> <install prefix> [cross prefix]
#   cross prefix defaults to arm-none-linux-gnueabihf- (the CI toolchain)
set -e

SRC=$(cd "$1" && pwd)
PREFIX=$2
CROSS=${3:-arm-none-linux-gnueabihf-}
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)

VIDEO_DECODERS=mpeg1video,mpeg2video,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,h263,h264,mjpeg,wmv1,wmv2,wmv3,vc1
AUDIO_DECODERS=mp1,mp1float,mp2,mp2float,mp3,mp3float,ac3,ac3_fixed,eac3,dca,aac,aac_latm,vorbis,flac,alac,wmav1,wmav2,pcm_s16le,pcm_s16be,pcm_s24le,pcm_u8,pcm_dvd,pcm_bluray
SUB_DECODERS=dvdsub,srt,subrip,ass,ssa,text
DEMUXERS=mpegps,mpegts,mpegvideo,avi,matroska,mov,ogg,asf,mp3,flac,wav,aac,ac3,dts,srt,ass
PARSERS=mpegvideo,mpeg4video,h264,mpegaudio,ac3,dca,aac,vorbis,flac,dvdsub

BUILD=$(mktemp -d)
cd "$BUILD"
"$SRC/configure" \
	--prefix="$PREFIX" \
	--enable-cross-compile --target-os=linux --arch=arm --cpu=cortex-a9 \
	--cross-prefix="$CROSS" \
	--extra-cflags="-mfpu=neon -mfloat-abi=hard -O2" \
	--enable-static --disable-shared --enable-pic \
	--disable-programs --disable-doc --disable-debug \
	--disable-network --disable-autodetect --disable-iconv \
	--disable-everything \
	--disable-avdevice --disable-avfilter --disable-postproc \
	--enable-avcodec --enable-avformat --enable-swscale --enable-swresample \
	--enable-decoder=$VIDEO_DECODERS,$AUDIO_DECODERS,$SUB_DECODERS \
	--enable-demuxer=$DEMUXERS \
	--enable-parser=$PARSERS \
	--enable-protocol=file \
	--enable-neon
make -j"$(nproc)"
make install
rm -rf "$BUILD"
