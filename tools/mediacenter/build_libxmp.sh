#!/bin/sh
# Cross-compiles a static libxmp (MIT licence, https://xmp.sourceforge.net)
# for the DE10-Nano HPS: it plays the tracker modules (MOD, XM, S3M, IT, MED
# and the other formats libxmp loads) in MiSTer MediaCenter's audio screen.
# Built without its configure script: the sources of the main library listed
# in its Makefile.vc, without the depackers and ProWizard (packed modules).
#
# usage: build_libxmp.sh <libxmp source dir> <install prefix> [cross prefix]
#   cross prefix defaults to arm-none-linux-gnueabihf- (the CI toolchain)
set -e

SRC=$(cd "$1" && pwd)
PREFIX=$2
CROSS=${3:-arm-none-linux-gnueabihf-}
mkdir -p "$PREFIX/lib" "$PREFIX/include"
PREFIX=$(cd "$PREFIX" && pwd)

FILES=$(tr -d '\r' < "$SRC/Makefile.vc" | sed -n '/^OBJS[ 	]*=/,/^$/p' | sed 's/[ 	]*\\$//' | tr ' \t' '\n\n' | grep '\.obj$' | grep -v win32 | tr '\\' '/' | sed 's#\.obj$#.c#')
BUILD=$(mktemp -d)
for f in $FILES; do
	o="$BUILD/$(echo "$f" | tr '/' '_').o"
	"${CROSS}gcc" -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -I"$SRC/include" -I"$SRC/src" \
		-DLIBXMP_STATIC -DLIBXMP_NO_DEPACKERS -DLIBXMP_NO_PROWIZARD -DHAVE_POWF -DHAVE_MKSTEMP -DHAVE_UMASK \
		-c "$SRC/$f" -o "$o" &
	[ "$(jobs -p | wc -l)" -ge "$(nproc)" ] && wait
done
wait
rm -f "$PREFIX/lib/libxmp.a"
"${CROSS}ar" rcs "$PREFIX/lib/libxmp.a" "$BUILD"/*.o
cp "$SRC/include/xmp.h" "$PREFIX/include/"
echo "libxmp: $(ls "$BUILD"/*.o | wc -l) files -> $PREFIX/lib/libxmp.a"
rm -rf "$BUILD"
