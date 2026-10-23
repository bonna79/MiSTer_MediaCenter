#!/bin/sh
# Test streams for the accelerator (phase 1: I and P, phase 2: B), made
# with the host ffmpeg + libx264, each one aimed at a part of the engine.
# usage: make_tests.sh <out dir> [1080p source file]
set -e
O=$1
mkdir -p "$O"
X="ffmpeg -v error -y"
P="-c:v libx264 -profile:v high -bf 0 -pix_fmt yuv420p"
# 1: CABAC, 8x8 transform, all intra modes on a test card
$X -f lavfi -i testsrc2=size=320x192:rate=25 -t 2 $P -x264-params weightp=0:keyint=25 "$O/t1.mp4"
# 2: fades: weighted prediction, 4 reference pictures
$X -f lavfi -i "mandelbrot=size=320x240:rate=25,fade=in:0:20,fade=out:30:20" -t 2 $P -x264-params weightp=2:ref=4:keyint=50 "$O/t2.mp4"
# 3: CAVLC, 4x4 transform only, 4 slices, fast motion
$X -f lavfi -i "testsrc=size=352x288:rate=25,rotate=a=t*2:c=black" -t 2 $P -profile:v main -x264-params cabac=0:8x8dct=0:slices=4:keyint=20 "$O/t3.mp4"
# 4: many coefficients (low qp) and long vectors (pan and zoom), all partitions
$X -f lavfi -i "cellauto=s=384x256:rate=25:rule=110,scale=384:256,zoompan=z='1+0.01*on':d=1:s=384x256" -t 2 $P -x264-params qp=12:partitions=all:me=umh:subme=9:ref=3:keyint=30 "$O/t4.mp4"
# 5: very low quality: skips, large DC blocks
$X -f lavfi -i "testsrc2=size=320x192:rate=25,hue=h=t*90" -t 2 $P -x264-params crf=45:keyint=50 "$O/t5.mp4"
# 6: 1080p (coded height 1088, cropped), from a real source if given
if [ -n "$2" ]; then
	$X -ss 1 -i "$2" -frames:v 6 -an $P -x264-params weightp=2:ref=3:keyint=50 "$O/t6.mp4"
fi

# phase 2: B pictures
B="-c:v libx264 -profile:v high -pix_fmt yuv420p"
# b1: B pyramid, spatial direct, plain average
$X -f lavfi -i testsrc2=size=320x192:rate=25 -t 2 $B -x264-params bframes=3:b-pyramid=normal:direct=spatial:weightb=0:keyint=25 "$O/b1.mp4"
# b2: fades, implicit weights, temporal direct, 4 reference pictures
$X -f lavfi -i "mandelbrot=size=320x240:rate=25,fade=in:0:20,fade=out:30:20" -t 2 $B -x264-params bframes=2:weightb=1:weightp=2:direct=temporal:ref=4:keyint=50 "$O/b2.mp4"
# b3: CAVLC, 4 slices, fast motion, all partitions, automatic direct
$X -f lavfi -i "testsrc=size=352x288:rate=25,rotate=a=t*2:c=black" -t 2 $B -profile:v main -x264-params cabac=0:8x8dct=0:slices=4:bframes=3:b-adapt=2:direct=auto:partitions=all:weightb=1:keyint=20 "$O/b3.mp4"
# b4: many coefficients, long vectors
$X -f lavfi -i "cellauto=s=384x256:rate=25:rule=110,scale=384:256,zoompan=z='1+0.01*on':d=1:s=384x256" -t 2 $B -x264-params qp=12:bframes=2:partitions=all:me=umh:subme=9:ref=3:weightb=1:keyint=30 "$O/b4.mp4"
# b6: 1080p with B pictures, from the 1080p test stream
if [ -f "$O/t6.mp4" ]; then
	$X -i "$O/t6.mp4" $B -x264-params bframes=3:weightb=1:ref=3:keyint=50 "$O/b6.mp4"
fi
