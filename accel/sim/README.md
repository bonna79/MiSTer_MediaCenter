# Accelerator test bench (phase 1)

1. Host FFmpeg 6.1.2 with `ffmpeg-parse-only.patch` (tools/mediacenter) and
   `ffmpeg-accel-dump.patch` applied, built for the host (C only, no asm):
   `configure --disable-everything --enable-decoder=h264 --enable-demuxer=mov,matroska,h264 --enable-parser=h264 --enable-protocol=file --disable-x86asm ...`
2. `gcc -O2 -o accdump accdump.c -I<ffmpeg>/include <ffmpeg>/lib/libavformat.a <ffmpeg>/lib/libavcodec.a <ffmpeg>/lib/libavutil.a -lm -lpthread`
3. `make_tests.sh <dir> [1080p file]` makes the test streams.
4. `accdump <dir>/t1.mp4 <dir>/t1` writes `t1.cmd` (FPGA commands, see
   `../doc/comandi.md`) and `t1.rec` (pictures FFmpeg rebuilt, no deblocking).
5. `model.py <dir>/t1` rebuilds the pictures from the commands and compares.
   The Verilog engine is checked the same way.
