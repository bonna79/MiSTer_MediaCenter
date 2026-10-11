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

# The player needs a 32-bit framebuffer; the script terminal may be 16-bit.
command -v vmode >/dev/null 2>&1 && vmode -r 640 480 rgb32 >/dev/null 2>&1

printf '\033[?25l'
"$BIN" "$@" >/tmp/mediacenter.log 2>&1
status=$?
printf '\033[?25h'
exit $status
