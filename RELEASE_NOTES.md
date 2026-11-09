<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 4.0** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB e rete, su HDMI e su CRT a 15 kHz.

**Novità della 4.0:** porta nell'FPGA anche la lettura del **CABAC**, la parte più pesante della decodifica H.264: fino alla 3.6 la faceva l'ARM, che poi mandava al core circa 1 MB di comandi per ogni immagine. Ora l'ARM manda al core solo i byte compressi del video e non fa quasi più niente.

- **Il lettore CABAC nell'FPGA:** un nuovo blocco del core (`acc_parse`, con il decodificatore aritmetico `cab_dec`) legge le slice CABAC bit per bit, ricava i tipi di macroblocco, i vettori di movimento, le previsioni intra e i coefficienti, e passa al motore gli stessi comandi che prima scriveva l'ARM. L'ARM lavora 0,3-0,4 ms per immagine invece di 1,7-26,7 ms, e scrive in memoria 10-20 KB per immagine invece di 0,5-1,3 MB.
- **Tre clock separati:** il motore gira a 100 MHz, il lettore CABAC a 85,7 MHz e il resto del core a 108 MHz, con code di passaggio fra le zone. Così ogni parte rispetta i tempi anche con il chip pieno al 94%.
- **Più margine nelle scene pesanti:** HTC.mp4 (1080p a 30 fps), il file più al limite con la 3.6, passa da 6 a 1 immagine in ritardo e l'ARM da 26,7 a 0,4 ms per immagine. L'attesa della memoria dell'FPGA scende in tutti i file provati.
- **Stesse immagini della 3.6:** le immagini fatte con il CABAC nell'FPGA sono state confrontate in simulazione con quelle della 3.6 su 11 video di prova: identiche byte per byte.
- **Sempre una strada di ritorno:** i video H.264 CAVLC usano l'acceleratore come nella 3.6. Se il lettore CABAC trova una slice rovinata, il CABAC torna sull'ARM fino alla prossima immagine IDR. `MC_NOCABAC=1` in `mediacenter.ini` fa leggere il CABAC all'ARM come nella 3.6 (solo per i confronti).
- **Nuova immagine** con la scritta Ver. 4.0 H264 1080P e il numero della build, mostrata 3 secondi quando si avvia lo script mediacenter dagli Scripts.

Il core del menu della 4.0 usa il 94% della logica (39.305 ALM), il 43% dei blocchi di memoria interna e il 74% dei moltiplicatori dell'FPGA. Rispetta tutti i tempi a 100 °C e a -40 °C, nei modelli lento e veloce del chip (seed 12 del fitter, margine minimo +0,014 ns sull'uscita HDMI). Il lettore e il core vanno aggiornati insieme: con un core più vecchio il lettore lascia il CABAC all'ARM.

Provata su un MiSTer vero il 9 novembre 2026, su CRT Sony Trinitron (SCART RGB, 480i) e HDMI insieme. Tutte le prove dal menu, con l'audio sempre pulito e nessuna immagine persa dall'FPGA:

| Prova | 3.6 | 4.0 |
| --- | --- | --- |
| One Piece Episodio 940, H.264 1920x1080 | 0,0%, ARM 12,3 ms per immagine | **0,1%**, nessuna in ritardo, ARM **0,3 ms** |
| HTC.mp4, H.264 1920x1080 a 29,97 fps, tutto | 0,2%, 6 in ritardo, ARM 26,7 ms (al limite) | **0,1%**, 1 in ritardo, ARM **0,4 ms** |
| Tears of Steel, H.264 1728x720 2,40:1 | 0,0%, ARM 3,8 ms | **0,1%**, 3 in ritardo nei primi secondi, ARM **0,3 ms** |
| One Piece Episodio 938, H.264 1280x720 | 0,1%, ARM 1,7 ms | **0,1%**, nessuna in ritardo, ARM **0,3 ms** |
| Inuyasha, H.264 1440x810, MKV da DVD-R | 0,0%, ARM 5,6 ms | **0,0%**, nessuna in ritardo, ARM **0,4 ms** |
| Sega intro, H.264 lossless (lo fa l'ARM) | logo visibile | logo visibile, nessuna in ritardo |
| DivX 576x432, DVD originale con menu, tracce audio e sottotitoli, Video CD | funzionano | funzionano come prima |

Dettagli nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-40) e nella [scheda tecnica](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/docs/SCHEDA_TECNICA_ACCELERATORE_IT.md). Le novità della 3.6 e delle versioni precedenti sono nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-36).

**Ringraziamenti:**

- **Umberto Parisi** (rmonic79) dei **Beceri Videoludici**: la funzione Regola CRT è stata possibile grazie al suo lavoro su [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust). Vi consiglio di seguire il loro canale YouTube: [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).
- **Max Riptide**, per le numerose prove fatte e i suggerimenti.
- **mad4gith**, per le idee suggerite per migliorare MMC.

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD: contiene tutto quello che serve a MMC. Aggiorna sempre tutti i file insieme. **Dalla 3.6 o prima:** estrai lo zip sopra i file vecchi e riavvia; servono insieme il lettore `mediacenter` e il core `menu_mediacenter.rbf` della 4.0 (il lettore CABAC è nel core: con il core vecchio il lettore lascia il CABAC all'ARM come nella 3.6). `MiSTer.ini` e `mediacenter.ini` restano come sono.

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

**English:** **MiSTer MediaCenter 4.0** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB and network, over HDMI and on a 15 kHz CRT.

**What's new in 4.0:** it also moves the reading of **CABAC**, the heaviest part of H.264 decoding, into the FPGA: up to 3.6 the ARM did it, then sent the core about 1 MB of commands for every picture. Now the ARM sends the core only the compressed bytes of the video and has almost nothing left to do.

- **The CABAC reader in the FPGA:** a new block of the core (`acc_parse`, with the arithmetic decoder `cab_dec`) reads the CABAC slices bit by bit, works out the macroblock types, motion vectors, intra predictions and coefficients, and hands the engine the same commands the ARM used to write. The ARM spends 0.3-0.4 ms per picture instead of 1.7-26.7 ms, and writes 10-20 KB per picture to memory instead of 0.5-1.3 MB.
- **Three separate clocks:** the engine runs at 100 MHz, the CABAC reader at 85.7 MHz and the rest of the core at 108 MHz, with queues between the zones. This way every part meets timing even with the chip 94% full.
- **More headroom in heavy scenes:** HTC.mp4 (1080p at 30 fps), the file closest to the limit with 3.6, goes from 6 late pictures to 1 and the ARM from 26.7 to 0.4 ms per picture. The FPGA's memory wait drops on every file tested.
- **The same pictures as 3.6:** the pictures made with CABAC in the FPGA were compared in simulation with those of 3.6 on 11 test videos: identical byte for byte.
- **Always a way back:** CAVLC H.264 video uses the accelerator as in 3.6. If the CABAC reader finds a damaged slice, CABAC goes back to the ARM until the next IDR picture. `MC_NOCABAC=1` in `mediacenter.ini` has the ARM read CABAC as in 3.6 (for comparisons only).
- **New picture** reading Ver. 4.0 H264 1080P with the build number, shown for 3 seconds when the mediacenter script is started from Scripts.

The 4.0 menu core uses 94% of the FPGA's logic (39,305 ALMs), 43% of the internal memory blocks and 74% of the multipliers. It meets all timing at 100 °C and -40 °C, in the slow and fast models of the chip (fitter seed 12, smallest margin +0.014 ns on the HDMI output). The player and the core must be updated together: with an older core the player leaves CABAC to the ARM.

Tested on a real MiSTer on 9 November 2026, on a Sony Trinitron CRT (SCART RGB, 480i) and HDMI together. Every test from the menu, with the sound always clean and no picture lost by the FPGA:

| Test | 3.6 | 4.0 |
| --- | --- | --- |
| One Piece Episode 940, H.264 1920x1080 | 0.0%, ARM 12.3 ms per picture | **0.1%**, none late, ARM **0.3 ms** |
| HTC.mp4, H.264 1920x1080 at 29.97 fps, whole file | 0.2%, 6 late, ARM 26.7 ms (at the limit) | **0.1%**, 1 late, ARM **0.4 ms** |
| Tears of Steel, H.264 1728x720 2.40:1 | 0.0%, ARM 3.8 ms | **0.1%**, 3 late in the first seconds, ARM **0.3 ms** |
| One Piece Episode 938, H.264 1280x720 | 0.1%, ARM 1.7 ms | **0.1%**, none late, ARM **0.3 ms** |
| Inuyasha, H.264 1440x810, MKV from DVD-R | 0.0%, ARM 5.6 ms | **0.0%**, none late, ARM **0.4 ms** |
| Sega intro, lossless H.264 (done by the ARM) | logo visible | logo visible, none late |
| DivX 576x432, original DVD with menus, audio tracks and subtitles, Video CD | work | work as before |

Details in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-40) and the [technical sheet](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/docs/TECHNICAL_SHEET_ACCELERATOR_EN.md). What's new in 3.6 and earlier versions is in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-36).

**Thanks:** Umberto Parisi (rmonic79) of the Beceri Videoludici, whose work on [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust) made CRT Adjust possible ([YouTube channel](https://youtube.com/@rmonic79_ibecerivideoludici)); Max Riptide for the many tests and suggestions; mad4gith for the ideas to improve MMC.

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card: it holds everything MMC needs. Always update all files together (**from 3.6 or earlier:** extract the zip over the old files and restart; the 4.0 `mediacenter` player and `menu_mediacenter.rbf` core are needed together, the CABAC reader is in the core and with the old core the player leaves CABAC to the ARM as in 3.6; `MiSTer.ini` and `mediacenter.ini` stay as they are): Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, script `mediacenter.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`, `MMC_MiSTer_ini.txt` to `/media/fat/`. Copy the lines of `MMC_MiSTer_ini.txt` into `/media/fat/MiSTer.ini`. The [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) Main, for game discs, is downloaded from its own project. Restart the MiSTer. Full guide in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installation).

**Your feedback is very valuable:** write to me, even when everything works. If there are problems, open a report on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) with photos of the screen and the `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` and `/tmp/mediacenter_crt` files.
