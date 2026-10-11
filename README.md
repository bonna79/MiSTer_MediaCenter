# MiSTer MediaCenter

MiSTer MediaCenter è un lettore multimediale per MiSTer FPGA (DE10-Nano). Usa l'ARM e l'FPGA insieme: l'ARM legge dischi e file, l'FPGA presenta audio e video.

Questo repository è il Main (il programma ARM) del lettore. Si attiva al posto del Main standard tramite `MiSTer.ini` e non modifica l'installazione esistente.

## Stato

Il lettore Video CD deriva dal ramo `video-cd-boot-launch` di Main_MiSTer_Physical_Disc. L'obiettivo è farne un'unica applicazione che legga anche DVD (compresi i commerciali), file video DivX/Xvid/MP4/MKV e audio compresso, con gli stessi comandi.

| Funzione | Stato | Come funziona |
| --- | --- | --- |
| DVD-Video, anche commerciali | In prova | Programma `mediacenter`: menu del disco con i pulsanti, capitoli, tracce audio, sottotitoli; i DVD protetti con libdvdcss. Senza `mediacenter` si apre il core DVD ([MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD)) |
| Video CD 1.0/1.1/2.0 | Sì | Lettore integrato `vcd_player` sull'ARM: menu PBC, avanti/indietro, pausa, stop, capitoli, canale audio |
| Super Video CD | Parziale | Senza `vcd_player` o per gli SVCD (MPEG-2) si apre il core FPGA DVD |
| CD audio | Sì | Apre il core scelto con `AUDIOCD=` oppure [MiSTer Hi-Fi](https://github.com/Anime0t4ku/MiSTer_Hi-Fi) |
| File video DivX/Xvid/MP4/MKV/MPEG/VOB da SD, USB e rete | In prova | Programma `mediacenter` sull'ARM, decodifica FFmpeg; SD e 720p MPEG-4, l'H.264 HD è troppo pesante |
| Audio MP1/MP2/MP3, Ogg, FLAC, WAV, AAC | In prova | Stesso programma; i file di una cartella si ascoltano come un album |

## Installazione

1. Scarica `MiSTer_MediaCenter` dall'ultima build nella pagina Actions o Releases.
2. Copialo in `/media/fat/` sulla SD.
3. Aggiungi a `/media/fat/MiSTer.ini`:

```ini
[menu]
main=MiSTer_MediaCenter

[A0CD-*]
main=MiSTer_MediaCenter
```

La sezione `[A0CD-*]` vale per i core di `_Physical Disc Cores` (PSX, Saturn, Mega CD, PC Engine CD…). Se in `MiSTer.ini` c'è ancora `main=MiSTer_Physical-CD`, sostituiscilo: altrimenti quei core ripartono con il vecchio Main.

### Programma `mediacenter`

1. Scarica l'artifact `mediacenter` dalla stessa build.
2. Copia `mediacenter` in `/media/fat/linux/mediacenter`.
3. Copia `mediacenter.sh` in `/media/fat/Scripts/`.

Da **Scripts → mediacenter** si apre l'elenco delle sorgenti: disco nel lettore, `/media/fat/Video`, `/media/fat/Music`, la SD, le chiavette USB e la cartella di rete `cifs`. Quando `mediacenter` è installato, anche i Video CD e i DVD inseriti si aprono con lui. Nell'elenco, le immagini `.iso` e le cartelle con dentro `VIDEO_TS` si aprono come DVD.

Per i DVD protetti (CSS) serve `libdvdcss.so.2` in `/media/fat/dvdcss/`, lo stesso percorso usato da MiSTer_DVD e dal suo script `install_dvdcss`. Senza, si leggono solo i DVD non protetti.

| Tasto | Elenco | Riproduzione |
| --- | --- | --- |
| A | Apri | |
| B | Indietro | Stop |
| Start | | Play |
| Select | | Pausa |
| ◄ ► | Pagina | Capitolo precedente/successivo, senza capitoli ±60 s; nell'album brano precedente/successivo |
| ▲ ▼ | Su/giù | Cambia traccia audio, oppure canale (stereo, sinistro, destro) |
| L / R | | Indietro / avanti veloce |
| X | Esci | Torna all'elenco (conferma con A) |

Durante un DVD i tasti sono gli stessi, con queste differenze: **A** cambia i sottotitoli (spenti, poi ogni lingua) e **Y** apre il menu del disco. Nel menu del disco la croce sceglie il pulsante, **A** o **Start** lo conferma e **Y** torna al film.

Per misurare le prestazioni di un file o di un DVD: `/media/fat/linux/mediacenter --bench <file>` oppure `--bench /dev/sr0`. Stampa la velocità di decodifica rispetto al tempo reale: sopra il 100% il file è riproducibile.

In alternativa a `mediacenter`, per i DVD e i Video CD si può usare il core [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) e segui la sua guida: aggiunge la sezione `[DVD]` con `main=MiSTer_DVDcss` e lo script `install_dvdcss`.

## Lettore Video CD (in inglese, dal progetto originale)


Video CDs (VCD, SVCD and HQ-VCD, from any region: PAL, NTSC-U or NTSC-J) can be played through Auto Disc Detection.

Video CDs contain MPEG-1 video, which none of the console cores can decode (the PlayStation core does not include the MPEG board of the Asian SCPH-5903 model). They are therefore played by a separate application running on the MiSTer Linux side, `vcd_player`, which decodes the video in software and shows it on the MiSTer video output.

To enable it, copy `vcd_player` (built with `make vcd_player`, or taken from the build artifacts) to:

```
/media/fat/linux/vcd_player
```

Then, with Auto Disc Detection enabled, insert a Video CD while the MiSTer menu is showing. Playback starts automatically and plays every MPEG track of the disc in order. At the end of the disc, or when the OSD/Menu button is pressed, the player closes and the menu returns.

### Video CD controls

Pressing any button of the gamepad shows the controls and a progress bar at the bottom of the screen. They disappear 10 seconds after the last button press. The buttons are those of the MiSTer menu mapping:

| Button | Action |
| --- | --- |
| Start | Play (also resumes normal speed after fast forward / rewind) |
| Select | Pause / resume |
| B | Stop (back to the beginning of the disc; Start plays again) |
| D-pad right / left | Next / previous chapter |
| R / L | Fast forward / rewind at 10x (press again, or Start, to play) |
| D-pad up / down | Audio channel: stereo, left only, right only (for discs with two languages on the two channels) |
| Y | Back to the disc menu (Video CD 2.0 discs with menus) |
| X, then A | Exit to the menu (any other button cancels) |

Chapters are the entry points stored on the disc (`ENTRIES.VCD`), or the MPEG tracks when the disc has none.

### Video CD 2.0 menus (PBC)

Video CD 2.0 discs with playback control (`PSD.VCD`) start from the disc's own menu, which is drawn by the disc as a picture with numbered choices. The controls stay on screen while a menu is shown:

| Button | Action |
| --- | --- |
| D-pad up / down | Choose the menu entry (the number is shown at the bottom) |
| A | Play the chosen entry |
| Start | Play the menu's default entry |
| D-pad right / left | Next / previous page of the menu |
| Y | Back (the disc's "return") |
| B | Stop |
| X, then A | Exit to the menu |

Menu timeouts, waits and loops are followed as stored on the disc. Discs without menus, or with menus the player cannot read, play their tracks in order as before. Subtitles on Video CDs are part of the picture and are always shown; Super Video CDs (MPEG-2) are not decoded.

When `/media/fat/linux/vcd_player` is not present, nothing changes: Video CDs are handled exactly as before.

Audio is sent to the MiSTer audio output through `aplay`. If `aplay` is not available, video plays without sound.


## Impostazioni in `MiSTer.ini`

| Voce | Valori | Effetto |
| --- | --- | --- |
| `DVD=` | `MEDIACENTER`, `HYBRID`, `FPGA` | Programma o core da aprire per i DVD-Video; senza voce usa `mediacenter` se c'è, altrimenti un core DVD |
| `PHYSICAL_DISC_BOOT=` | `0`, `1` | Avvia il disco già inserito all'accensione |
| `AUDIOCD=` | `PSX`, `A0CD-Saturn`, `MISTERHIFI`, … | Programma da aprire per i CD audio |

I Video CD usano `/media/fat/linux/mediacenter` se è installato, poi `/media/fat/linux/vcd_player`, altrimenti il core FPGA DVD.

## Compilazione

Il workflow `.github/workflows/build.yml` compila il Main a ogni push con GCC ARM 10.2 (`arm-none-linux-gnueabihf`) e pubblica il binario come artifact. In locale: `make` con quel compilatore nel `PATH`.

`mediacenter` richiede FFmpeg 6.1.2, libdvdread 6.1.3 e libdvdnav 6.1.1 compilati in statico per l'ARM:

```sh
git clone --depth 1 -b n6.1.2 https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
tools/mediacenter/build_ffmpeg.sh ffmpeg-src ffmpeg-arm
curl -fsSL https://download.videolan.org/pub/videolan/libdvdread/6.1.3/libdvdread-6.1.3.tar.bz2 | tar -xj
curl -fsSL https://download.videolan.org/pub/videolan/libdvdnav/6.1.1/libdvdnav-6.1.1.tar.bz2 | tar -xj
tools/mediacenter/build_dvdnav.sh libdvdread-6.1.3 libdvdnav-6.1.1 dvdnav-arm
make mediacenter FFMPEG=$PWD/ffmpeg-arm DVDNAV=$PWD/dvdnav-arm
```

## Karaoke CD+G (Mega CD)

I dischi karaoke CD+G mostrano testi e immagini con il core Mega CD, attraverso il lettore CD del BIOS, come sulla console vera. Apri l'MGL del Mega CD da **`_Physical Disc Cores`**, oppure metti `AUDIOCD=A0CD-MegaCD` perché il riconoscimento automatico apra i CD audio e CD+G con il core Mega CD. Il lettore USB deve saper leggere il subcanale grezzo.

Se la grafica non compare, aggiungi `MEGACD_SUBCODE_LOG=1` alla sezione del Mega CD in `MiSTer.ini`: scrive `/media/fat/logs/megacd_subcode.log` e `/media/fat/logs/megacd_subcode_raw.bin`. Se la grafica ha difetti, prova `MEGACD_CDDA_PACED=1` nella stessa sezione.

```ini
[A0CD-MegaCD]
main=MiSTer_MediaCenter
MEGACD_SUBCODE_LOG=1
```

## Provenienza e licenza

MiSTer MediaCenter deriva da [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) e dal supporto ai dischi fisici di Physical Disc. È distribuito sotto GPL-3.0, come i progetti da cui deriva: vedi [LICENSE](LICENSE) e [NOTICE](NOTICE).
