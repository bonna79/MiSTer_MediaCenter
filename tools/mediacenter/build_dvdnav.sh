#!/bin/sh
# Cross-compiles static libdvdread and libdvdnav for the DE10-Nano HPS.
# libdvdcss is not linked: libdvdread loads libdvdcss.so.2 at run time, and
# mediacenter loads it from /media/fat/dvdcss first (see dvd.c).
#
# usage: build_dvdnav.sh <libdvdread src> <libdvdnav src> <install prefix> [cross prefix]
#   cross prefix defaults to arm-none-linux-gnueabihf- (the CI toolchain)
set -e

READ_SRC=$(cd "$1" && pwd)
NAV_SRC=$(cd "$2" && pwd)
PREFIX=$3
CROSS=${4:-arm-none-linux-gnueabihf-}
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)
HOST=${CROSS%-}

export CC="${CROSS}gcc"
export AR="${CROSS}ar"
export RANLIB="${CROSS}ranlib"
export CFLAGS="-O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH=

for src in "$READ_SRC" "$NAV_SRC"; do
	BUILD=$(mktemp -d)
	cd "$BUILD"
	"$src/configure" --host="$HOST" --prefix="$PREFIX" --disable-shared --enable-static --with-pic --disable-apidoc
	make -j"$(nproc)"
	make install
	cd /
	rm -rf "$BUILD"
done
