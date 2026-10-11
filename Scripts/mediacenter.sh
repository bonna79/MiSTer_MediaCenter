#!/bin/bash
# MiSTer MediaCenter: opens the file browser (SD, USB, network share, disc).
# Copy to /media/fat/Scripts and the binary to /media/fat/linux/mediacenter.
BIN=/media/fat/linux/mediacenter

if [ ! -x "$BIN" ]; then
  echo "$BIN not found."
  echo "Copy the mediacenter build there first."
  sleep 5
  exit 1
fi

# The player needs the normal 32-bit framebuffer at the screen size. The
# MiSTer_MediaCenter Main sets it with fb_cmd3; with another Main fall back to
# a 640x480 framebuffer.
echo "fb_cmd3 default" >/dev/MiSTer_cmd 2>/dev/null
sleep 0.5
if [ "$(cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null)" != "32" ]; then
  command -v vmode >/dev/null 2>&1 && vmode -r 640 480 rgb32 >/dev/null 2>&1
fi

printf '\033[?25l'
"$BIN" "$@" >/tmp/mediacenter.log 2>&1
status=$?
printf '\033[?25h'
exit $status
