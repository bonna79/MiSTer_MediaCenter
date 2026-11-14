#!/bin/bash
# MiSTer MediaCenter updater: downloads the latest release from GitHub and
# replaces only the files that changed (Main, player, menu core, scripts,
# libdvdcss). MiSTer.ini and mediacenter.ini are never touched.
# Copy to /media/fat/Scripts and start it from the Scripts menu.
# It needs no input: it runs to the end and prints what it does.

REPO=bonna79/MiSTer_MediaCenter
API=https://api.github.com/repos/$REPO/releases/latest
FAT=/media/fat
WORK=$FAT/linux/.mmc_update
BACKUP=$FAT/linux/mmc_backup
LOG=/tmp/Update_Mister_Media_Center.log
SELF=Scripts/Update_Mister_Media_Center.sh

# Language of the messages: the same choice as the player (MC_LANG=en).
LANG_EN=0
if grep -qiE '^[[:space:]]*MC_LANG[[:space:]]*=[[:space:]]*en' "$FAT/linux/mediacenter.ini" 2>/dev/null; then
  LANG_EN=1
fi
say() { # say "italiano" "english"
  if [ "$LANG_EN" = 1 ]; then echo "$2"; else echo "$1"; fi
  echo "$(date '+%F %T') $2" >>"$LOG"
}
# MiSTer.ini is never changed: remind the lines to copy when the [menu]
# section does not start our Main yet (same reading as Mister_Media_Center.sh).
ini_note() {
  m=$(awk -F= '
    /^[ \t]*\[/ { s = tolower($0); gsub(/[ \t\r]/, "", s); menu = (s == "[menu]"); next }
    menu && tolower($1) ~ /^[ \t]*main[ \t]*$/ { v = $2; sub(/[;\r].*/, "", v); gsub(/[ \t]/, "", v); print v; exit }
  ' "$FAT/MiSTer.ini" 2>/dev/null)
  if [ "$m" != "MiSTer_MediaCenter" ]; then
    echo
    say "Da fare a mano: copia le righe di $FAT/MMC_MiSTer_ini.txt in $FAT/MiSTer.ini" \
        "To do by hand: copy the lines of $FAT/MMC_MiSTer_ini.txt into $FAT/MiSTer.ini"
    say "(main=MiSTer_MediaCenter va solo nella sezione [menu]), poi riavvia." \
        "(main=MiSTer_MediaCenter goes only in the [menu] section), then restart."
  fi
}
fail() {
  say "ERRORE: $1" "ERROR: $2"
  say "Nessun file e' stato cambiato." "No file was changed."
  rm -rf "$WORK"
  exit 1
}

: >"$LOG"
say "MiSTer MediaCenter: aggiornamento" "MiSTer MediaCenter: update"
echo

# Certificates for https, the same files the MiSTer Downloader uses (they come
# with the MiSTer Linux and with Update All). Never an insecure connection.
CACERT=""
for c in "$FAT/Scripts/.config/downloader/cacert.pem" /etc/ssl/certs/cacert.pem /etc/ssl/certs/ca-certificates.crt; do
  if [ -s "$c" ]; then CACERT=$c; break; fi
done
[ -n "$CACERT" ] || fail "certificati https non trovati: lancia una volta update_all o downloader dal menu Scripts, poi riprova." \
                         "https certificates not found: run update_all or downloader from the Scripts menu once, then try again."
command -v curl >/dev/null 2>&1 || fail "curl non trovato." "curl not found."
CURL="curl -sSfL --cacert $CACERT --connect-timeout 15 --retry 3"

# Latest release: tag, zip address and the sha256 that GitHub computed.
say "Controllo l'ultima versione su GitHub..." "Checking the latest version on GitHub..."
rm -rf "$WORK"
mkdir -p "$WORK" || fail "non riesco a scrivere in $FAT/linux." "cannot write to $FAT/linux."
$CURL -H "Accept: application/vnd.github+json" -o "$WORK/release.json" "$API" 2>>"$LOG" \
  || fail "GitHub non risponde (rete o internet?)." "GitHub does not answer (network or internet?)."

INFO=$(python3 - "$WORK/release.json" <<'EOF' 2>>"$LOG"
import json, sys
r = json.load(open(sys.argv[1]))
for a in r.get("assets", []):
    if a.get("name") == "MiSTer_MediaCenter.zip":
        d = a.get("digest") or ""
        print(r.get("tag_name", ""), a.get("browser_download_url", ""),
              a.get("size", 0), d[7:] if d.startswith("sha256:") else "-")
        break
EOF
)
read -r TAG URL SIZE SHA <<<"$INFO"
case "$URL" in
  https://github.com/$REPO/releases/download/*/MiSTer_MediaCenter.zip) ;;
  *) fail "risposta di GitHub non valida." "invalid answer from GitHub." ;;
esac
say "Ultima versione: $TAG" "Latest version: $TAG"

# Download and check the zip: size, sha256 (when GitHub gives it), zip test.
say "Scarico MiSTer_MediaCenter.zip..." "Downloading MiSTer_MediaCenter.zip..."
$CURL -o "$WORK/mmc.zip" "$URL" 2>>"$LOG" || fail "download non riuscito." "download failed."
GOT=$(wc -c <"$WORK/mmc.zip" | tr -d ' ')
[ "$GOT" = "$SIZE" ] || fail "file scaricato incompleto ($GOT di $SIZE byte)." "downloaded file incomplete ($GOT of $SIZE bytes)."
if [ "$SHA" != "-" ]; then
  if command -v sha256sum >/dev/null 2>&1; then
    HAVE=$(sha256sum "$WORK/mmc.zip" | cut -d' ' -f1)
  else
    HAVE=$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$WORK/mmc.zip")
  fi
  [ "$HAVE" = "$SHA" ] || fail "il file scaricato non corrisponde a quello di GitHub (sha256)." "the downloaded file does not match GitHub's (sha256)."
fi

mkdir -p "$WORK/new"
if command -v unzip >/dev/null 2>&1; then
  unzip -tq "$WORK/mmc.zip" >>"$LOG" 2>&1 || fail "zip danneggiato." "damaged zip."
  unzip -q "$WORK/mmc.zip" -d "$WORK/new" >>"$LOG" 2>&1 || fail "estrazione non riuscita." "extraction failed."
else
  python3 - "$WORK/mmc.zip" "$WORK/new" <<'EOF' >>"$LOG" 2>&1 || fail "estrazione non riuscita." "extraction failed."
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
if z.testzip() is not None:
    sys.exit(1)
z.extractall(sys.argv[2])
EOF
fi

# The files of the release, the updater itself last. Only plain relative
# paths, never a MiSTer.ini or mediacenter.ini.
LIST=$(cd "$WORK/new" && find . -type f | sed 's|^\./||' | sort)
FILES=""
for f in $LIST; do
  case "$f" in
    /*|*..*|*.ini|*' '*) say "Salto $f" "Skipping $f"; continue ;;
    "$SELF") ;;
    *) FILES="$FILES $f" ;;
  esac
done
[ -f "$WORK/new/$SELF" ] && FILES="$FILES $SELF"
case "$FILES" in
  *MiSTer_MediaCenter*linux/mediacenter*|*linux/mediacenter*MiSTer_MediaCenter*) ;;
  *) fail "lo zip non contiene i file di MiSTer MediaCenter." "the zip does not hold the MiSTer MediaCenter files." ;;
esac

# Which files changed.
CHANGED=""
for f in $FILES; do
  new=$(md5sum "$WORK/new/$f" | cut -d' ' -f1)
  old=$(md5sum "$FAT/$f" 2>/dev/null | cut -d' ' -f1)
  [ "$new" = "$old" ] || CHANGED="$CHANGED $f"
done
echo
if [ -z "$CHANGED" ]; then
  say "Hai gia' la versione $TAG: nessun file da aggiornare." "You already have version $TAG: no file to update."
  rm -rf "$WORK"
  ini_note
  exit 0
fi

# Room on the SD for the copies (twice the new files, in KB).
NEED=$(du -sk "$WORK/new" | cut -f1)
FREE=$(df -k "$FAT" | awk 'NR==2 {print $4}')
case "$FREE" in *[!0-9]*|"") FREE="" ;; esac
if [ -n "$FREE" ] && [ "$FREE" -lt $((NEED * 2 + 1024)) ]; then
  fail "spazio libero insufficiente sulla SD." "not enough free space on the SD card."
fi

# Backup of the files being replaced (only the last update is kept).
rm -rf "$BACKUP"
for f in $CHANGED; do
  if [ -f "$FAT/$f" ]; then
    mkdir -p "$BACKUP/$(dirname "$f")"
    cp -p "$FAT/$f" "$BACKUP/$f" || fail "copia di sicurezza di $f non riuscita." "backup of $f failed."
  fi
done
[ -d "$BACKUP" ] && say "Copia di sicurezza dei vecchi file in $BACKUP" "Backup of the old files in $BACKUP"

# Replace: copy next to the old file, check it, then rename over it (a running
# program keeps the old file until it exits).
REBOOT=0
DONE=""
for f in $CHANGED; do
  dst="$FAT/$f"
  mkdir -p "$(dirname "$dst")"
  cp "$WORK/new/$f" "$dst.mmcnew" 2>>"$LOG" && sync
  a=$(md5sum "$WORK/new/$f" | cut -d' ' -f1)
  b=$(md5sum "$dst.mmcnew" 2>/dev/null | cut -d' ' -f1)
  if [ "$a" != "$b" ] || ! mv -f "$dst.mmcnew" "$dst" 2>>"$LOG"; then
    rm -f "$dst.mmcnew"
    say "ERRORE: non riesco a scrivere $dst" "ERROR: cannot write $dst"
    [ -n "$DONE" ] && say "Gia' aggiornati:$DONE (vecchi file in $BACKUP)" "Already updated:$DONE (old files in $BACKUP)"
    rm -rf "$WORK"
    exit 1
  fi
  chmod +x "$dst" 2>/dev/null
  DONE="$DONE $f"
  say "Aggiornato: $f" "Updated: $f"
  case "$f" in
    MiSTer_MediaCenter|linux/menu_mediacenter.rbf) REBOOT=1 ;;
  esac
done
sync
rm -rf "$WORK"

echo
say "Aggiornamento a $TAG completato." "Update to $TAG done."
if [ -f "$FAT/Scripts/mediacenter.sh" ]; then
  say "Nota: c'e' ancora il vecchio script Scripts/mediacenter.sh (4.0): puoi cancellarlo." \
      "Note: the old script Scripts/mediacenter.sh (4.0) is still there: you can delete it."
fi
ini_note
if [ "$REBOOT" = 1 ]; then
  say "Riavvia il MiSTer per completare l'aggiornamento (Main o core cambiati)." "Restart the MiSTer to finish the update (Main or menu core changed)."
fi
exit 0
