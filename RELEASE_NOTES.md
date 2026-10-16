<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 2.1** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB e rete.

**Novità della 2.1:**
- Main aggiornato al Main ufficiale del MiSTer del 16 ottobre 2026 (correzioni HDMI e novità per gli altri core).

**Correzioni:**
- **CRT senza immagine:** con la 2.0 sull'uscita VGA a 15 kHz si vedeva solo il fruscio del menu, perché il Main non diceva al core che c'era un'immagine da mostrare. Ora DVD e video arrivano al CRT (provato su un Trinitron con cavo SCART RGB, 576i interlacciato);
- **DivX tagliati a destra su HDMI:** un video 576x432 veniva raddoppiato a 1152 punti, più largo del riquadro 4:3 (960 punti a 720p) che lo scaler non sa rimpicciolire. Ora il lettore prepara l'immagine della misura giusta;
- con `vga_scaler=0` non compare più l'avviso "modify MiSTer.ini" sopra il lettore.

**Come funziona:** l'ARM legge il disco o il file, decodifica video e audio con FFmpeg e scrive le immagini nel framebuffer. L'FPGA converte i colori da YUV a RGB, ingrandisce l'immagine per l'HDMI e genera da solo il segnale a 15 kHz per il CRT, mandando la stessa immagine alle due uscite insieme. Dettagli nel README.

Provata sia con il Main ufficiale precedente sia con l'ultimo Main ufficiale installato come `/media/fat/MiSTer`: nessun problema.

**Dalla 2.0:**
- l'FPGA converte i colori del video: gli MP4 H.264 a 720p ora sono fluidi (immagini non mostrate dal 10% a circa il 2%);
- audio e video sincronizzati, cambio della traccia audio durante il film senza scatti;
- Super Video CD con i menu, anche da immagini `.bin` + `.cue` sulla SD;
- i giochi su disco li gestisce il Main di [Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc), da installare a parte;
- file di impostazioni `/media/fat/linux/mediacenter.ini` (per esempio `MC_AV_OFFSET=0.05` per spostare l'audio).

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD (aggiorna sempre tutti i file insieme):

| File | Cartella |
| --- | --- |
| `MiSTer_MediaCenter` (Main) | `/media/fat/` |
| `mediacenter` (lettore ARM) | `/media/fat/linux/` |
| `menu_mediacenter.rbf` (core del menu) | `/media/fat/linux/` |
| `mediacenter.sh` (script) | `/media/fat/Scripts/` |
| `libdvdcss.so.2` | `/media/fat/dvdcss/` |

Il vecchio `vcd_player` non serve più. Poi in `MiSTer.ini`, senza forzature dello scaler nella sezione `[menu]`:

```ini
[MiSTer]
vga_scaler=0          ; per il CRT a 15 kHz
forced_scandoubler=0
composite_sync=1      ; solo con cavo SCART

[menu]
main=MiSTer_MediaCenter

[A0CD-*]
main=MiSTer_Physical-CD
```

MMC lavora nel menu (dischi video e file); il Main di Physical Disc di Anime0t4ku, installato a parte, lavora nei core delle console per i giochi. Riavvia il MiSTer. Per i DVD protetti lo zip include `libdvdcss.so.2` in `dvdcss/`, la stessa provata con i DVD protetti. Guida completa e risultati delle prove nel [README](https://github.com/bonna79/MiSTer_MediaCenter#readme).

**Provato su MiSTer reale:** DVD protetti con menu, tracce audio e sottotitoli; Video CD; Super Video CD da immagine; DVD-R con DivX e MKV; MP4 H.264 720p; gioco PSX con LibCrypt tramite il Main di Physical Disc; DVD e DivX su CRT Trinitron via SCART RGB e su HDMI 720p.
**Da provare:** Super Video CD da disco masterizzato.

Problemi: apri una segnalazione in [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) con foto dello schermo e `/tmp/mediacenter.log`.

---

**English:** **MiSTer MediaCenter 2.1** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB and network.

**What's new in 2.1:**
- Main updated to the official MiSTer Main of 16 October 2026 (HDMI fixes and what's new for the other cores).

**Fixes:**
- **CRT with no picture:** with 2.0 the 15 kHz VGA output only showed the menu's noise, because the Main never told the core there was a picture to show. DVDs and video now reach the CRT (tested on a Trinitron with a SCART RGB cable, 576i interlaced);
- **DivX cut on the right over HDMI:** a 576x432 video was doubled to 1152 pixels, wider than the 4:3 window (960 pixels at 720p) that the scaler cannot shrink. The player now prepares the picture at the right size;
- with `vga_scaler=0` the "modify MiSTer.ini" warning no longer appears over the player.

**How it works:** the ARM reads the disc or file, decodes video and audio with FFmpeg and writes the pictures to the frame buffer. The FPGA converts colours from YUV to RGB, enlarges the picture for HDMI and generates the 15 kHz CRT signal by itself, sending the same picture to both outputs at once. Details in the README.

Tested with both the previous official Main and the latest official Main installed as `/media/fat/MiSTer`: no problems.

**From 2.0:**
- the FPGA converts the video colours: 720p H.264 MP4 now plays smoothly (frames not shown from 10% to about 2%);
- audio and video in sync, seamless audio track changes during a film;
- Super Video CD with menus, also from `.bin` + `.cue` images on the SD card;
- game discs are handled by the [Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) Main, installed separately;
- settings file `/media/fat/linux/mediacenter.ini` (for example `MC_AV_OFFSET=0.05` to move the sound).

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card (always update all files together): Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, script `mediacenter.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`. The old `vcd_player` is no longer needed. In `MiSTer.ini` put `vga_scaler=0`, `forced_scandoubler=0` (and `composite_sync=1` with SCART) in `[MiSTer]`, `main=MiSTer_MediaCenter` in `[menu]` with no scaler overrides, and `main=MiSTer_Physical-CD` in `[A0CD-*]`: MMC handles the menu (video discs and files), Anime0t4ku's Physical Disc Main, installed separately, handles games inside the console cores. Restart the MiSTer. Full guide and test results in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md).

**Tested on a real MiSTer:** protected DVDs with menus, audio tracks and subtitles; Video CDs; Super Video CD from an image; DVD-R with DivX and MKV; 720p H.264 MP4; PSX game with LibCrypt through the Physical Disc Main; DVD and DivX on a Trinitron CRT over SCART RGB and over HDMI 720p. **Still to test:** Super Video CD from a burned disc.
