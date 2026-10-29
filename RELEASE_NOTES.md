<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 3.5** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB e rete, su HDMI e su CRT a 15 kHz.

**Novità della 3.5:** l'acceleratore H.264 della 3.0 è completo. L'immagine 1080p è più pulita, i video 1080p a 30 immagini al secondo sono fluidi e il menu sopra il video non li rallenta più.

- **Filtro anti-blocchi (deblocking) nell'FPGA:** il core del menu fa anche il filtro che nasconde i bordi dei blocchi dell'H.264, senza costare niente all'ARM. Spariti i punti viola, verdi e gialli nelle scene veloci con la neve.
- **1080p a 30 immagini al secondo fluido:** HTC.mp4 (1080p 29,97) passa dal 12,4% di immagini non mostrate della 3.0 allo 0,1%.
- **Menu sopra il 1080p senza scatti:** barra, menu e scritte del lettore li mescola il core del menu mentre copia l'immagine; all'ARM restano 0,3 ms per immagine invece di circa 5.
- **Subito dopo l'accensione il video aspetta la fine dell'avvio della MiSTer** (al massimo 60 secondi, con i secondi sullo schermo; B torna indietro), invece di scattare nel primo minuto. `MC_NOBOOTWAIT=1` toglie l'attesa.
- **Il video ha la precedenza sugli altri programmi** mentre suona, e **MiSTer Remote** viene messo in pausa fino alla fine del video (`MC_NOPRIO=1` e `MC_NOPAUSE=1` li tolgono).
- **Diagnosi nel log:** bitrate del video, picchi del decoder, tempo per disegnare le immagini, altri programmi che usano la CPU.
- **Nuova immagine** Ver. 3.5, con il numero della build in basso.

**Fin dove arriva il 1080p:** fluido fino a circa 6 Mbit/s a 30 immagini al secondo e 9 Mbit/s a 24 (stime dalle prove). Oltre, l'ARM non fa in tempo a leggere il flusso H.264: leggerlo nell'FPGA è un obiettivo per la 4.0.

Provata su un MiSTer vero il 25–28 ottobre 2026 su CRT Sony Trinitron (SCART RGB, 480i) e HDMI insieme: HTC.mp4 1080p 29,97 con il menu aperto più volte 0,0–0,1% di immagini non mostrate e menu pulito su HDMI e CRT; HTC.mp4 subito dopo l'accensione 0,3% dopo 18 s di attesa (prima 6,8%); One Piece 1080p 23,976 0,3%. Dettagli nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-35) e nella [scheda tecnica](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/docs/SCHEDA_TECNICA_ACCELERATORE_IT.md).

Le novità della 3.0 (acceleratore H.264 nell'FPGA) e delle versioni precedenti sono nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-30).

**Ringraziamenti:**

- **Umberto Parisi** (rmonic79) dei **Beceri Videoludici**: la funzione Regola CRT è stata possibile grazie al suo lavoro su [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust). Vi consiglio di seguire il loro canale YouTube: [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).
- **Max Riptide**, per le numerose prove fatte e i suggerimenti.
- **mad4gith**, per le idee suggerite per migliorare MMC.

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD: contiene tutto quello che serve a MMC. Aggiorna sempre tutti i file insieme. **Dalla 3.0:** estrai lo zip sopra i file vecchi e riavvia; servono insieme il lettore `mediacenter` e il core `menu_mediacenter.rbf` della 3.5 (il filtro anti-blocchi e il menu sopra il video sono nel core). `MiSTer.ini` e `mediacenter.ini` restano come sono.

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

**English:** **MiSTer MediaCenter 3.5** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB and network, over HDMI and on a 15 kHz CRT.

**What's new in 3.5:** 3.0's H.264 accelerator is complete. The 1080p picture is cleaner, 1080p video at 30 pictures per second plays smoothly and the menu over the video no longer slows it down.

- **Deblocking filter in the FPGA:** the menu core also runs the filter that hides the edges of H.264's blocks, at no cost to the ARM. The purple, green and yellow dots in fast scenes with snow are gone.
- **Smooth 1080p at 30 pictures per second:** HTC.mp4 (1080p 29.97) goes from 12.4% of pictures not shown in 3.0 to 0.1%.
- **Menu over 1080p without stutter:** the player's bar, menu and text are blended by the menu core while it copies the picture; the ARM spends 0.3 ms per picture instead of about 5.
- **Right after power-on the video waits for the MiSTer's start-up to end** (60 seconds at most, with the seconds on screen; B goes back), instead of stuttering for the first minute. `MC_NOBOOTWAIT=1` removes the wait.
- **The video comes before the other programs** while it plays, and **MiSTer Remote** is paused until the video ends (`MC_NOPRIO=1` and `MC_NOPAUSE=1` remove them).
- **Diagnostics in the log:** the video's bitrate, the decoder's peaks, the time to draw the pictures, the other programs using the CPU.
- **New picture** Ver. 3.5, with the build number at the bottom.

**How far 1080p goes:** smooth up to about 6 Mbit/s at 30 pictures per second and 9 Mbit/s at 24 (estimates from the tests). Beyond that the ARM cannot parse the H.264 stream in time: parsing it in the FPGA is a goal for 4.0.

Tested on a real MiSTer on 25–28 October 2026 on a Sony Trinitron CRT (SCART RGB, 480i) and HDMI together: HTC.mp4 1080p 29.97 with the menu opened several times 0.0–0.1% of pictures not shown and the menu clean on HDMI and CRT; HTC.mp4 right after power-on 0.3% after 18 s of waiting (before 6.8%); One Piece 1080p 23.976 0.3%. Details in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-35) and the [technical sheet](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/docs/TECHNICAL_SHEET_ACCELERATOR_EN.md).

What's new in 3.0 (the H.264 accelerator in the FPGA) and in earlier versions is in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-30).

**Thanks:** Umberto Parisi (rmonic79) of the Beceri Videoludici, whose work on [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust) made CRT Adjust possible ([YouTube channel](https://youtube.com/@rmonic79_ibecerivideoludici)); Max Riptide for the many tests and suggestions; mad4gith for the ideas to improve MMC.

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card: it holds everything MMC needs. Always update all files together (**from 3.0:** extract the zip over the old files and restart; the 3.5 `mediacenter` player and `menu_mediacenter.rbf` core are needed together, the deblocking filter and the menu over the video are in the core; `MiSTer.ini` and `mediacenter.ini` stay as they are): Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, script `mediacenter.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`, `MMC_MiSTer_ini.txt` to `/media/fat/`. Copy the lines of `MMC_MiSTer_ini.txt` into `/media/fat/MiSTer.ini`. The [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) Main, for game discs, is downloaded from its own project. Restart the MiSTer. Full guide in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installation).

**Your feedback is very valuable:** write to me, even when everything works. If there are problems, open a report on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) with photos of the screen and the `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` and `/tmp/mediacenter_crt` files.
