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

# The menu belongs to the MiSTer_MediaCenter Main ([menu] main= in
# MiSTer.ini). Another Main can stay in charge of the menu after a game (seen
# with MiSTer_Physical-CD after a PSX game): it keeps the stock menu core, so
# the CRT shows the "modify MiSTer.ini" notice instead of the player. Then our
# Main is started in its place; it loads the MediaCenter menu core, and the
# script is started again from the Scripts menu.
OURS=/media/fat/MiSTer_MediaCenter
INI_MAIN=$(awk -F= '
  /^[ \t]*\[/ { s = tolower($0); gsub(/[ \t\r]/, "", s); menu = (s == "[menu]"); next }
  menu && tolower($1) ~ /^[ \t]*main[ \t]*$/ { v = $2; sub(/[;\r].*/, "", v); gsub(/[ \t]/, "", v); print v; exit }
' /media/fat/MiSTer.ini 2>/dev/null)
if [ -x "$OURS" ] && [ "/media/fat/$INI_MAIN" = "$OURS" ]; then
  for p in /proc/[0-9]*; do
    exe=$(readlink "$p/exe" 2>/dev/null)
    case "$exe" in
      /media/fat/MiSTer*) ;;
      *) continue ;;
    esac
    [ "$exe" = "$OURS" ] && break
    pid=${p#/proc/}
    echo "$(date +%s) script: menu run by $exe (pid $pid), not by $OURS: starting $OURS" >>/tmp/mediacenter_main.log
    echo "Il menu e' gestito da $exe."
    echo "Riavvio il Main di MiSTer MediaCenter: poi lancia di nuovo lo script."
    RESTART="sleep 1; kill $pid; sleep 1; kill -9 $pid 2>/dev/null; cd /media/fat; exec $OURS menu.rbf"
    if command -v setsid >/dev/null 2>&1; then
      setsid sh -c "$RESTART" </dev/null >/dev/null 2>&1 &
    else
      nohup sh -c "$RESTART" </dev/null >/dev/null 2>&1 &
    fi
    sleep 3
    exit 0
  done
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
# the console back in text mode, also if the player did not end normally
"$BIN" --console-text >/dev/null 2>&1
printf '\033[?25h'
exit $status
