<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 2.1** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB e rete.

**Novità della 2.1:**
- l'uscita a 15 kHz per i CRT ora mostra il video del lettore (provata su un Trinitron con cavo SCART RGB); con la 2.0 sul CRT si vedeva solo lo sfondo del menu;
- Main aggiornato al Main ufficiale del MiSTer del 16 ottobre 2026 (correzioni HDMI e novità per gli altri core);
- con `vga_scaler=0` non compare più l'avviso "modify MiSTer.ini" sopra il lettore.

**Dalla 2.0:**
- l'FPGA converte i colori del video: gli MP4 H.264 a 720p ora sono fluidi (immagini non mostrate dal 10% a circa il 2%);
- audio e video sincronizzati, cambio della traccia audio durante il film senza scatti;
- Super Video CD con i menu, anche da immagini `.bin` + `.cue` sulla SD;
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

**Provato su MiSTer reale:** DVD protetti con menu, tracce audio e sottotitoli; Video CD; Super Video CD da immagine; DVD-R con DivX e MKV; MP4 H.264 720p; gioco PSX con LibCrypt tramite il Main di Physical Disc; DVD su CRT Trinitron via SCART RGB.
**Da provare:** Super Video CD da disco masterizzato.

Problemi: apri una segnalazione in [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) con foto dello schermo e `/tmp/mediacenter.log`.

---

**English:** **MiSTer MediaCenter 2.1** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB and network.

**What's new in 2.1:**
- the 15 kHz output for CRTs now shows the player's video (tested on a Trinitron with a SCART RGB cable); with 2.0 the CRT only showed the menu background;
- Main updated to the official MiSTer Main of 16 October 2026 (HDMI fixes and what's new for the other cores);
- with `vga_scaler=0` the "modify MiSTer.ini" warning no longer appears over the player.

**From 2.0:**
- the FPGA converts the video colours: 720p H.264 MP4 now plays smoothly (frames not shown from 10% to about 2%);
- audio and video in sync, seamless audio track changes during a film;
- Super Video CD with menus, also from `.bin` + `.cue` images on the SD card;
- game discs are handled by the [Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) Main, installed separately;
- settings file `/media/fat/linux/mediacenter.ini` (for example `MC_AV_OFFSET=0.05` to move the sound).

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card. It holds the Main, the player and the menu core: always update them together. Then set `main=MiSTer_MediaCenter` in `[menu]` and `main=MiSTer_Physical-CD` in `[A0CD-*]` of `MiSTer.ini`, and restart the MiSTer. For protected DVDs the zip includes `libdvdcss.so.2` in `dvdcss/`, the same one tested with protected DVDs. Full guide and test results in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md).

**Tested on a real MiSTer:** protected DVDs with menus, audio tracks and subtitles; Video CDs; Super Video CD from an image; DVD-R with DivX and MKV; 720p H.264 MP4; PSX game with LibCrypt through the Physical Disc Main; DVD on a Trinitron CRT over SCART RGB. **Still to test:** Super Video CD from a burned disc.
