> [!WARNING]
> **AVVERTENZA: HDMI e CRT collegati insieme.** Se usi insieme l'uscita HDMI verso un televisore LCD e l'uscita VGA→SCART verso un CRT, collega **MiSTer, televisore LCD e CRT alla stessa multipresa elettrica**. Con i due televisori su prese diverse, accendere il CRT mentre il MiSTer è acceso può far passare uno sbalzo elettrico attraverso il MiSTer, tramite le masse dei cavi. È successo in una prova: l'HDMI è diventato verde per qualche secondo, poi l'immagine è sparita su tutte e due le uscite (l'audio continuava) e il pad non rispondeva più; è servito riavviare. Se non puoi usare la stessa multipresa, **accendi il CRT prima del MiSTer**.

> [!WARNING]
> **WARNING: HDMI and CRT connected together.** If you use the HDMI output to an LCD TV and the VGA→SCART output to a CRT at the same time, plug **the MiSTer, the LCD TV and the CRT into the same power strip**. With the two TVs on different wall sockets, switching the CRT on while the MiSTer is running can send an electrical surge through the MiSTer via the cable grounds. It happened in a test: HDMI turned green for a few seconds, then the picture vanished on both outputs (the sound kept playing) and the pad stopped responding; a reboot was needed. If you cannot use the same power strip, **switch the CRT on before the MiSTer**.

<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 3.0** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB e rete, su HDMI e su CRT a 15 kHz.

**Novità della 3.0:** i video **1080p H.264** sono fluidi, su HDMI e su CRT, grazie a un acceleratore nell'FPGA.

- **Acceleratore H.264 nell'FPGA:** il core del menu contiene un motore che ricostruisce le immagini dei video H.264. L'ARM legge il file e decifra il video, poi manda all'FPGA i comandi di ogni immagine; l'FPGA ricostruisce l'immagine e la copia da sola nel framebuffer. Prima l'ARM impiegava circa 44 ms per immagine, più dei 41,7 ms disponibili a 24 immagini al secondo; ora gliene bastano in media 5–15.
- **1080p fluido:** nei tratti senza salti 240 immagini mostrate su 240 ogni 10 secondi, contro circa il 14% di immagini non mostrate della 2.8. Panoramiche fluide su HDMI e CRT.
- **Anche nel formato 4:3 con Start:** l'FPGA restringe l'immagine a 1280 punti (Auto e gli altri formati) o a 960 (4:3); il cambio di formato durante il film non spegne l'acceleratore.
- **Quando si usa:** solo per i video H.264 a 8 bit, 4:2:0, progressivi, larghi 1920 punti. DVD, Video CD, DivX, 720p e gli altri video vanno come nella 2.8. Se l'acceleratore trova un problema torna da solo al decoder normale dallo stesso punto del film. `MC_NOACC=1` in `mediacenter.ini` lo spegne.
- **Diagnosi nel log:** ogni 10 secondi una riga `diag acceleratore` con immagini mandate e disegnate, tempi dell'ARM e dell'FPGA, immagini in ritardo o perse.
- **Chiavetta USB elencata una volta sola** anche quando il sistema la monta su `/media/usb0` e `/media/usb1`.
- **Video CD giusto sul CRT:** immagine 4:3 a tutto schermo senza bande nere ai lati, 576i per i dischi PAL e riquadro Formato con Start (ora i Video CD li riproduce lo stesso lettore dei Super Video CD).
- **Nuova immagine** Ver. 3.0 1080 P, mostrata solo quando si avvia lo script mediacenter a mano (non quando parte un DVD, un Super Video CD o un Video CD), come nella 2.8.

Il motore non ha ancora il filtro anti-blocchi (deblocking) dell'H.264: è il prossimo passo.

Provata su un MiSTer vero il 24 ottobre 2026 su CRT Sony Trinitron (SCART RGB, 480i) e HDMI insieme, con One Piece Episodio 940 1080p H.264: 238 s di riproduzione continua con 5724 immagini disegnate dall'FPGA e nessuna persa; 449 s con salti avanti e indietro, 240 su 240 ogni 10 secondi nei tratti senza salti; cambi di formato Auto, 16:9, 4:3 fluidi con l'acceleratore sempre acceso; scene pesanti con l'FPGA al lavoro circa 30 ms su 41,7; DVD e DivX come nella 2.8; Video CD PAL a tutto schermo sul CRT. Dettagli nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-30).

Le novità della 2.8 (formato giusto per ogni film sul CRT, riquadro Formato con Start) e delle versioni precedenti sono nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-28).

**Ringraziamenti:**

- **Umberto Parisi** (rmonic79) dei **Beceri Videoludici**: la funzione Regola CRT è stata possibile grazie al suo lavoro su [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust). Vi consiglio di seguire il loro canale YouTube: [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).
- **Max Riptide**, per le numerose prove fatte e i suggerimenti.
- **mad4gith**, per le idee suggerite per migliorare MMC.

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD: contiene tutto quello che serve a MMC. Aggiorna sempre tutti i file insieme. **Dalla 2.8:** estrai lo zip sopra i file vecchi e riavvia; servono insieme il lettore `mediacenter` e il core `menu_mediacenter.rbf` della 3.0 (l'acceleratore è nel core). `MiSTer.ini` e `mediacenter.ini` restano come sono.

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

**English:** **MiSTer MediaCenter 3.0** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB and network, over HDMI and on a 15 kHz CRT.

**What's new in 3.0:** **1080p H.264** video plays smoothly, over HDMI and on the CRT, thanks to an accelerator in the FPGA.

- **H.264 accelerator in the FPGA:** the menu core holds an engine that rebuilds the pictures of H.264 video. The ARM reads the file and parses the video, then sends the FPGA the commands for each picture; the FPGA rebuilds the picture and copies it into the frame buffer by itself. Before, the ARM needed about 44 ms per picture, more than the 41.7 ms available at 24 pictures per second; now 5–15 ms on average are enough.
- **Smooth 1080p:** in stretches without seeks 240 of 240 pictures shown every 10 seconds, against about 14% not shown in 2.8. Smooth pans on HDMI and CRT.
- **Also in the 4:3 format chosen with Start:** the FPGA narrows the picture to 1280 pixels (Auto and the other formats) or 960 (4:3); changing the format during the film does not turn the accelerator off.
- **When it is used:** only for 8-bit, 4:2:0, progressive H.264 video 1920 pixels wide. DVD, Video CD, DivX, 720p and other video play as in 2.8. If the accelerator meets a problem it goes back to the normal decoder by itself from the same point of the film. `MC_NOACC=1` in `mediacenter.ini` turns it off.
- **Diagnostics in the log:** every 10 seconds a `diag acceleratore` line with pictures sent and drawn, ARM and FPGA times, pictures late or lost.
- **USB stick listed once** even when the system mounts it on both `/media/usb0` and `/media/usb1`.
- **Video CD right on the CRT:** full-screen 4:3 picture without black bars at the sides, 576i for PAL discs and the format box with Start (Video CDs are now played by the same player as Super Video CDs).
- **New picture** Ver. 3.0 1080 P, shown only when the mediacenter script is started by hand (not when a DVD, Super Video CD or Video CD starts it), as in 2.8.

The engine does not have H.264's deblocking filter yet: that is the next step.

Tested on a real MiSTer on 24 October 2026 on a Sony Trinitron CRT (SCART RGB, 480i) and HDMI together, with One Piece Episode 940 1080p H.264: 238 s of continuous playback with 5724 pictures drawn by the FPGA and none lost; 449 s with seeks forward and back, 240 of 240 every 10 seconds in stretches without seeks; format changes Auto, 16:9, 4:3 smooth with the accelerator always on; heavy scenes with the FPGA working about 30 ms of 41.7; DVD and DivX as in 2.8; PAL Video CD full screen on the CRT. Details in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-30).

What's new in 2.8 (the right shape for every film on the CRT, format box with Start) and in earlier versions is in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-28).

**Thanks:** Umberto Parisi (rmonic79) of the Beceri Videoludici, whose work on [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust) made CRT Adjust possible ([YouTube channel](https://youtube.com/@rmonic79_ibecerivideoludici)); Max Riptide for the many tests and suggestions; mad4gith for the ideas to improve MMC.

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card: it holds everything MMC needs. Always update all files together (**from 2.8:** extract the zip over the old files and restart; the 3.0 `mediacenter` player and `menu_mediacenter.rbf` core are needed together, the accelerator is in the core; `MiSTer.ini` and `mediacenter.ini` stay as they are): Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, script `mediacenter.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`, `MMC_MiSTer_ini.txt` to `/media/fat/`. Copy the lines of `MMC_MiSTer_ini.txt` into `/media/fat/MiSTer.ini`. The [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) Main, for game discs, is downloaded from its own project. Restart the MiSTer. Full guide in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installation).

**Your feedback is very valuable:** write to me, even when everything works. If there are problems, open a report on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) with photos of the screen and the `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` and `/tmp/mediacenter_crt` files.
