<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 3.6** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB e rete, su HDMI e su CRT a 15 kHz.

**Novità della 3.6:** porta l'acceleratore H.264 nell'FPGA anche ai video **720p**, **810p** e ai film in formato cinema come **Tears of Steel (1728x720)**, e lo rende più veloce nel 1080p.

- **Acceleratore anche per 720p, 810p e altre larghezze:** fino alla 3.5 l'FPGA ricostruiva solo i video larghi 1920 punti. Ora prende tutti i video H.264 a 8 bit, 4:2:0, progressivi da 1024x400 a 2048x1088. Il core del menu copia poi l'immagine nel framebuffer portandola alla misura dello schermo, con gli stessi punti e le stesse righe che sceglieva l'ARM. Tears of Steel passa dal 29,6% di immagini non mostrate allo 0,0%, e l'ARM da circa 41 ms a 3,8 ms per immagine. Con un core del menu più vecchio il lettore spegne da solo l'acceleratore per queste misure.
- **Video 1440x810 con l'acceleratore:** per esempio gli MKV di Inuyasha. Il core restringe l'immagine a 960x405 con lo stesso restringimento 3:2 del 1080p; all'ARM restano circa 6 ms per immagine invece di 17.
- **FPGA più veloce nel 1080p:** il motore legge le finestre di riferimento del macroblocco successivo mentre calcola quello attuale, legge con una finestra sola i blocchi piccoli vicini con lo stesso vettore e il filtro anti-blocchi scrive due parole per volta in memoria. HTC.mp4 passa da 23,5 a 20,3 ms di lavoro dell'FPGA per immagine (-14%): più margine per le scene pesanti.
- **Partenza più sicura:** con l'acceleratore il lettore prepara 48 immagini prima di partire (prima 24) e tiene sempre liberi 10 posti dell'FPGA; HTC.mp4 non ha più immagini in ritardo all'inizio (prima 6). Senza acceleratore ne prepara 24 (prima 12) e le aspetta anche dopo un salto: un 720p con due salti passa da 5 immagini in ritardo a nessuna.
- **Film più larghi del 16:9 anche senza acceleratore:** quando il video lo decodifica l'ARM, un film 2,40:1 ristretto alla sua finestra resta in YUV (i colori li converte l'FPGA) e il restringimento usa le istruzioni NEON del processore: il disegno di un'immagine passa da 14,3 a 7,8 ms.
- **Nuova immagine** con la scritta Ver. 3.6, mostrata 3 secondi solo quando si avvia lo script mediacenter dagli Scripts (non quando parte un DVD, un Video CD o un Super Video CD).

Il core del menu della 3.6 usa il 78% della logica, il 37% dei blocchi di memoria interna e l'85% dei moltiplicatori dell'FPGA, e rispetta i tempi a 108 MHz a 100 °C e a -40 °C (seed 9 del fitter, margine +0,066 ns). Il lettore e il core vanno aggiornati insieme.

Provata su un MiSTer vero il 29–31 ottobre 2026, su CRT Sony Trinitron (SCART RGB, 480i) e HDMI insieme. Tutte le prove dal menu, con l'audio sempre pulito:

| Prova | Prima | 3.6 |
| --- | --- | --- |
| Tears of Steel, H.264 1728x720 2,40:1, i primi 241 s, con le scene più pesanti, da USB | 29,6% di immagini non mostrate, ARM circa 41 ms per immagine (al limite) | **0,0%**, nessuna in ritardo, nessuna persa dall'FPGA, ARM 3,8 ms |
| One Piece Episodio 938, H.264 1280x720, 156 s | 0,3% (3.5, senza acceleratore) | **0,1%** (nei primi secondi), nessuna in ritardo, nessuna persa dall'FPGA, ARM 1,7 ms |
| Inuyasha, H.264 1440x810, 627 s, dal DVD | 0,2%, 10 in ritardo, ARM 16,9 ms (3.5) | **0,0%**, 2 in ritardo (due pause di lettura del disco), nessuna persa dall'FPGA, ARM 5,6 ms |
| One Piece Episodio 940, H.264 1920x1080, 788 s | 0,3% (3.5) | **0,0%**, nessuna in ritardo, nessuna persa dall'FPGA |
| HTC.mp4, 1080p 29,97: lavoro dell'FPGA per immagine | 23,5 ms (3.5) | **20,3 ms** (-14%), 0,1% di immagini non mostrate |
| HTC.mp4, partenza | 6 immagini in ritardo nei primi secondi | **nessuna** |
| Video 720p decodificato dall'ARM (VisionQuest, prima dell'acceleratore per il 720p) con due salti | 0,5%, 5 in ritardo | **0,2%**, nessuna in ritardo |

Le immagini che il core disegna con l'acceleratore sono state confrontate prima in simulazione con quelle di FFmpeg e del lettore: identiche punto per punto (16 casi di ridimensionamento; Tears of Steel: 210 immagini nel modello e 6 nel motore Verilog).

Dettagli nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-36) e nella [scheda tecnica](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/docs/SCHEDA_TECNICA_ACCELERATORE_IT.md). Le novità della 3.5, della 3.0 e delle versioni precedenti sono nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-35).

**Ringraziamenti:**

- **Umberto Parisi** (rmonic79) dei **Beceri Videoludici**: la funzione Regola CRT è stata possibile grazie al suo lavoro su [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust). Vi consiglio di seguire il loro canale YouTube: [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).
- **Max Riptide**, per le numerose prove fatte e i suggerimenti.
- **mad4gith**, per le idee suggerite per migliorare MMC.

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD: contiene tutto quello che serve a MMC. Aggiorna sempre tutti i file insieme. **Dalla 3.5:** estrai lo zip sopra i file vecchi e riavvia; servono insieme il lettore `mediacenter` e il core `menu_mediacenter.rbf` della 3.6 (l'acceleratore per 720p, 810p e le altre larghezze è nel core). `MiSTer.ini` e `mediacenter.ini` restano come sono.

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

**English:** **MiSTer MediaCenter 3.6** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB and network, over HDMI and on a 15 kHz CRT.

**What's new in 3.6:** it brings the H.264 accelerator in the FPGA to **720p** and **810p** video and to cinema-format films such as **Tears of Steel (1728x720)**, and makes it faster on 1080p.

- **Accelerator for 720p, 810p and other widths too:** up to 3.5 the FPGA only rebuilt video 1920 pixels wide. Now it takes all 8-bit, 4:2:0, progressive H.264 video from 1024x400 to 2048x1088. The menu core then copies the picture into the frame buffer at the size of the screen, taking the same pixels and lines the ARM used to take. Tears of Steel goes from 29.6% of pictures not shown to 0.0%, and the ARM from about 41 ms to 3.8 ms per picture. With an older menu core the player turns the accelerator off by itself for these sizes.
- **1440x810 video with the accelerator:** for example Inuyasha's MKVs. The core narrows the picture to 960x405 with the same 3:2 narrowing as 1080p; the ARM spends about 6 ms per picture instead of 17.
- **Faster FPGA on 1080p:** the engine reads the reference windows of the next macroblock while it calculates the current one, reads small neighbouring blocks with the same vector through a single window, and the deblocking filter writes two words at a time to memory. HTC.mp4 goes from 23.5 to 20.3 ms of FPGA work per picture (-14%): more headroom for heavy scenes.
- **Safer start:** with the accelerator the player prepares 48 pictures before starting (before, 24) and always keeps 10 FPGA places free; HTC.mp4 no longer has late pictures at the start (before, 6). Without the accelerator it prepares 24 (before, 12) and waits for them after a seek too: a 720p with two seeks goes from 5 late pictures to none.
- **Films wider than 16:9 without the accelerator too:** when the ARM decodes the video, a 2.40:1 film narrowed to its window stays in YUV (the FPGA converts the colours) and the narrowing uses the processor's NEON instructions: drawing a picture goes from 14.3 to 7.8 ms.
- **New picture** reading Ver. 3.6, shown for 3 seconds only when the mediacenter script is started from Scripts (not when a DVD, Video CD or Super Video CD starts).

The 3.6 menu core uses 78% of the FPGA's logic, 37% of the internal memory blocks and 85% of the multipliers, and meets timing at 108 MHz at 100 °C and -40 °C (fitter seed 9, margin +0.066 ns). The player and the core must be updated together.

Tested on a real MiSTer on 29–31 October 2026, on a Sony Trinitron CRT (SCART RGB, 480i) and HDMI together. Every test from the menu, with the sound always clean:

| Test | Before | 3.6 |
| --- | --- | --- |
| Tears of Steel, H.264 1728x720 2.40:1, the first 241 s, with the heaviest scenes, from USB | 29.6% of pictures not shown, ARM about 41 ms per picture (at the limit) | **0.0%**, none late, none lost by the FPGA, ARM 3.8 ms |
| One Piece Episode 938, H.264 1280x720, 156 s | 0.3% (3.5, no accelerator) | **0.1%** (in the first seconds), none late, none lost by the FPGA, ARM 1.7 ms |
| Inuyasha, H.264 1440x810, 627 s, from the DVD | 0.2%, 10 late, ARM 16.9 ms (3.5) | **0.0%**, 2 late (two disc read pauses), none lost by the FPGA, ARM 5.6 ms |
| One Piece Episode 940, H.264 1920x1080, 788 s | 0.3% (3.5) | **0.0%**, none late, none lost by the FPGA |
| HTC.mp4, 1080p 29.97: FPGA work per picture | 23.5 ms (3.5) | **20.3 ms** (-14%), 0.1% of pictures not shown |
| HTC.mp4, start | 6 late pictures in the first seconds | **none** |
| 720p video decoded by the ARM (VisionQuest, before the 720p accelerator) with two seeks | 0.5%, 5 late | **0.2%**, none late |

The pictures the core draws with the accelerator were first compared in simulation with FFmpeg's and the player's: identical pixel for pixel (16 resizing cases; Tears of Steel: 210 pictures in the model and 6 in the Verilog engine).

Details in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-36) and the [technical sheet](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/docs/TECHNICAL_SHEET_ACCELERATOR_EN.md). What's new in 3.5, 3.0 and earlier versions is in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-35).

**Thanks:** Umberto Parisi (rmonic79) of the Beceri Videoludici, whose work on [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust) made CRT Adjust possible ([YouTube channel](https://youtube.com/@rmonic79_ibecerivideoludici)); Max Riptide for the many tests and suggestions; mad4gith for the ideas to improve MMC.

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card: it holds everything MMC needs. Always update all files together (**from 3.5:** extract the zip over the old files and restart; the 3.6 `mediacenter` player and `menu_mediacenter.rbf` core are needed together, the accelerator for 720p, 810p and the other widths is in the core; `MiSTer.ini` and `mediacenter.ini` stay as they are): Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, script `mediacenter.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`, `MMC_MiSTer_ini.txt` to `/media/fat/`. Copy the lines of `MMC_MiSTer_ini.txt` into `/media/fat/MiSTer.ini`. The [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) Main, for game discs, is downloaded from its own project. Restart the MiSTer. Full guide in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installation).

**Your feedback is very valuable:** write to me, even when everything works. If there are problems, open a report on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) with photos of the screen and the `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` and `/tmp/mediacenter_crt` files.
