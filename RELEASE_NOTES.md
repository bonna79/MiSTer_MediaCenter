> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 2.0** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB e rete.

**Novità:**
- l'FPGA converte i colori del video: gli MP4 H.264 a 720p ora sono fluidi (immagini non mostrate dal 10% a circa il 2%);
- audio e video sincronizzati, cambio della traccia audio durante il film senza scatti;
- Super Video CD con i menu, anche da immagini `.bin` + `.cue` sulla SD;
- uscita VGA a 15 kHz per i CRT, insieme all'HDMI;
- i giochi su disco li gestisce il Main di [Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc), da installare a parte;
- file di impostazioni `/media/fat/linux/mediacenter.ini` (per esempio `MC_AV_OFFSET=0.05` per spostare l'audio).

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD. Contiene Main, lettore, core del menu e libreria per i DVD protetti: aggiornali sempre insieme. Poi in `MiSTer.ini`:

```ini
[menu]
main=MiSTer_MediaCenter

[A0CD-*]
main=MiSTer_Physical-CD
```

Riavvia il MiSTer. Per i DVD protetti lo zip include `libdvdcss.so.2` in `dvdcss/`, la stessa provata con i DVD protetti. Guida completa e risultati delle prove nel [README](https://github.com/bonna79/MiSTer_MediaCenter#readme).

**Provato su MiSTer reale (HDMI):** DVD protetti con menu, tracce audio e sottotitoli; Video CD; Super Video CD da immagine; DVD-R con DivX e MKV; MP4 H.264 720p; gioco PSX con LibCrypt tramite il Main di Physical Disc.
**Da provare:** Super Video CD da disco masterizzato, uscita a 15 kHz su CRT.

Problemi: apri una segnalazione in [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) con foto dello schermo e `/tmp/mediacenter.log`.

---

**English:** **MiSTer MediaCenter 2.0** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB and network.

**What's new:**
- the FPGA converts the video colours: 720p H.264 MP4 now plays smoothly (frames not shown from 10% to about 2%);
- audio and video in sync, seamless audio track changes during a film;
- Super Video CD with menus, also from `.bin` + `.cue` images on the SD card;
- 15 kHz VGA output for CRTs, alongside HDMI;
- game discs are handled by the [Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) Main, installed separately;
- settings file `/media/fat/linux/mediacenter.ini` (for example `MC_AV_OFFSET=0.05` to move the sound).

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card. It holds the Main, the player and the menu core: always update them together. Then set `main=MiSTer_MediaCenter` in `[menu]` and `main=MiSTer_Physical-CD` in `[A0CD-*]` of `MiSTer.ini`, and restart the MiSTer. For protected DVDs the zip includes `libdvdcss.so.2` in `dvdcss/`, the same one tested with protected DVDs. Full guide and test results in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md).

**Tested on a real MiSTer (HDMI):** protected DVDs with menus, audio tracks and subtitles; Video CDs; Super Video CD from an image; DVD-R with DivX and MKV; 720p H.264 MP4; PSX game with LibCrypt through the Physical Disc Main. **Still to test:** Super Video CD from a burned disc, 15 kHz output on a CRT.
