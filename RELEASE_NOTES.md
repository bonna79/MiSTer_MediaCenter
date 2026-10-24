<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 2.8** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB e rete, su HDMI e su CRT a 15 kHz.

**Novità della 2.8:** ogni film ha il suo formato giusto sul CRT, e si cambia durante la visione.

- **Formato giusto per ogni film sul CRT:** bande nere alla proporzione vera dell'immagine (4:3, 16:9, 1,85:1, 2,35:1 e le altre), non più solo al 16:9; bande ai lati per le immagini più strette del 4:3.
- **Formato e Pan & scan in Regola CRT:** Auto, Pieno, 4:3, 16:9, Anamorfico 2,35:1, Cinema 1,85:1 (anche per l'HDMI) e Pan & scan per il CRT, salvati in `mediacenter.ini`.
- **Riquadro Formato con Start durante il film:** ◄ ► cambiano formato e l'immagine cambia subito su CRT e HDMI; A, Start o B lo chiudono, si chiude da solo dopo 6 secondi; la scelta resta salvata.
- **Elenchi a 720x576 (PAL) e 720x480 (NTSC)**, la risoluzione giusta per il CRT, con le scritte al centro: le regolazioni del CRT non cambiano.
- **Corretto:** una banda azzurra o gialla in alto sul CRT quando il Main rispondeva in ritardo, per esempio dopo un cambio di Modo 15 kHz. Ora il lettore riprova al video successivo e fuori dal film dipinge sempre il nero.
- **1080p:** il Main resta fermo durante la riproduzione e il core del menu copia l'immagine nel framebuffer (zero-copy, `MC_NOZC=1` lo spegne). Il 1080p resta sconsigliato (circa il 14% di immagini non mostrate): è in corso il lavoro per un acceleratore H.264 nell'FPGA.

Provata su un MiSTer vero il 23 e 24 ottobre 2026 su CRT Sony Trinitron e HDMI insieme: DVD di prova della geometria con i cerchi rotondi in 4:3 e in 16:9, bande nere corrette anche dopo i cambi di modo, riquadro Formato ed elenchi a 720x480.

Le novità della 2.7 (CRT a 720 punti come un lettore DVD, modo Auto dagli fps, DVD in YUV, 720p a piena risoluzione) e delle versioni precedenti sono nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-27).

**Ringraziamenti:**

- **Umberto Parisi** (rmonic79) dei **Beceri Videoludici**: la funzione Regola CRT è stata possibile grazie al suo lavoro su [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust). Vi consiglio di seguire il loro canale YouTube: [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).
- **Max Riptide**, per le numerose prove fatte e i suggerimenti.
- **mad4gith**, per le idee suggerite per migliorare MMC.

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD: contiene tutto quello che serve a MMC. Aggiorna sempre tutti i file insieme.

| File | Cartella |
| --- | --- |
| `MiSTer_MediaCenter` (Main) | `/media/fat/` |
| `mediacenter` (lettore ARM) | `/media/fat/linux/` |
| `menu_mediacenter.rbf` (core del menu) | `/media/fat/linux/` |
| `mediacenter.sh` (script) | `/media/fat/Scripts/` |
| `libdvdcss.so.2` | `/media/fat/dvdcss/` |
| `MMC_MiSTer_ini.txt` (righe per `MiSTer.ini`) | `/media/fat/` |

Copia in `/media/fat/MiSTer.ini` le righe di `MMC_MiSTer_ini.txt`: `vga_scaler=0`, `forced_scandoubler=0` (e `composite_sync=1` con la SCART) in `[MiSTer]`, `main=MiSTer_MediaCenter` in `[menu]` e `main=MiSTer_Physical-CD` in `[A0CD-*]`. Il Main di [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases), per i giochi su disco, si scarica dal suo progetto. Riavvia il MiSTer. Guida completa nel [README](https://github.com/bonna79/MiSTer_MediaCenter#installazione).

**I vostri commenti sono molto preziosi:** scrivetemi, anche quando funziona tutto. In caso di problemi apri una segnalazione in [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) con foto dello schermo e i file `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` e `/tmp/mediacenter_crt`.

---

**English:** **MiSTer MediaCenter 2.8** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB and network, over HDMI and on a 15 kHz CRT.

**What's new in 2.8:** every film gets its right shape on the CRT, and you can change it while watching.

- **The right shape for every film on the CRT:** black bars at the picture's real aspect (4:3, 16:9, 1.85:1, 2.35:1 and others), no longer only 16:9; bars at the sides for pictures narrower than 4:3.
- **Formato and Pan & scan in CRT Adjust:** Auto, Pieno (stretched), 4:3, 16:9, Anamorfico 2.35:1, Cinema 1.85:1 (HDMI too) and Pan & scan for the CRT, saved in `mediacenter.ini`.
- **Format box with Start during the film:** ◄ ► change the format and the picture changes at once on CRT and HDMI; A, Start or B close it, it closes by itself after 6 seconds; the choice stays saved.
- **Lists at 720x576 (PAL) and 720x480 (NTSC)**, the right resolution for the CRT, with the text centred: the CRT adjustments do not change.
- **Fixed:** a cyan or yellow band at the top of the CRT when the Main answered late, for example after a 15 kHz mode change. The player now tries again at the next video and always paints black outside the film.
- **1080p:** the Main rests during playback and the menu core copies the picture into the frame buffer (zero-copy, `MC_NOZC=1` turns it off). 1080p is still not recommended (about 14% of pictures not shown): work is under way on an H.264 accelerator in the FPGA.

Tested on a real MiSTer on 23 and 24 October 2026 on a Sony Trinitron CRT and HDMI together: geometry test DVD with round circles in 4:3 and 16:9, correct black bars even after mode changes, format box and lists at 720x480.

What's new in 2.7 (720-pixel CRT output like a DVD player, Auto mode from the frame rate, DVDs in YUV, 720p at full resolution) and in earlier versions is in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-27).

**Thanks:** Umberto Parisi (rmonic79) of the Beceri Videoludici, whose work on [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust) made CRT Adjust possible ([YouTube channel](https://youtube.com/@rmonic79_ibecerivideoludici)); Max Riptide for the many tests and suggestions; mad4gith for the ideas to improve MMC.

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card: it holds everything MMC needs. Always update all files together: Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, script `mediacenter.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`, `MMC_MiSTer_ini.txt` to `/media/fat/`. Copy the lines of `MMC_MiSTer_ini.txt` into `/media/fat/MiSTer.ini`. The [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) Main, for game discs, is downloaded from its own project. Restart the MiSTer. Full guide in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installation).

**Your feedback is very valuable:** write to me, even when everything works. If there are problems, open a report on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) with photos of the screen and the `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` and `/tmp/mediacenter_crt` files.
