<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 4.6.2** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB, rete e server Jellyfin, su HDMI e su CRT a 15 kHz.

**Novità della 4.6.2:**

- **Acceleratore sempre acceso dopo i salti:** con PgGiù/PgSu (avanti e indietro veloce) e con sinistra/destra, dopo alcuni salti l'FPGA poteva ricevere ancora comandi del punto di prima, rispondeva "unexpected command" e il lettore spegneva l'acceleratore fino alla fine del file: il 1080p diventava a scatti. Il problema c'era anche nella 4.6.1. Ora dopo ogni salto il core svuota per intero la coda dei comandi vecchi. Il nuovo core usa il seed 16, l'unico che rispetta tutti i tempi a 100 °C e a -40 °C, nei modelli lento e veloce, sia in setup che in hold.
- **File oltre i 2 GB negli elenchi:** film MKV o MP4 più grandi di 2 GB non comparivano, nemmeno in una cartella semplice. Ora si vedono e si aprono come gli altri, anche oltre i 4 GB. La stessa correzione vale per le immagini DVD `.iso`.
- **Nuova immagine** con la scritta Ver. 4.6.2, mostrata quando si apre lo script dagli Scripts.

Cambiano il core del menu `menu_mediacenter.rbf` e il lettore `mediacenter`: il Main nasce dagli stessi sorgenti della 4.6.1. **Dopo l'aggiornamento riavvia il MiSTer**, perché cambia il core.

**Documentazione completa (PDF, 22 pagine):** [italiano](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.2/MMC_4.6.2_documentazione_completa_IT.pdf) · [English](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.2/MMC_4.6.2_documentation_complete_EN.pdf). Spiega il chip FPGA, il lettore ARM, i passaggi fra ARM e FPGA, le uscite HDMI e CRT, le novità dalla 4.5 alla 4.6.2 (lingue, impostazioni, sottotitoli, script di aggiornamento, acceleratore dopo i salti) e come ricostruire tutto da zero. I PDF sono anche nella cartella [docs](https://github.com/bonna79/MiSTer_MediaCenter/tree/main/docs).

Provata su un MiSTer vero il 21 e il 22 novembre 2026, in due giri (il secondo dopo aver spento del tutto il MiSTer), su CRT Sony Trinitron (SCART RGB) e HDMI insieme:

| Prova | Risultato |
| --- | --- |
| One Piece 1080p, 567 s con circa 38 salti | Acceleratore sempre acceso, 0 immagini perse dall'FPGA, 1,8% non mostrate (attorno ai salti) |
| One Piece 720p dopo lo spegnimento, circa 24 salti | Acceleratore sempre acceso, 0 perse, 2,7% non mostrate (attorno ai salti) |
| HTC (scene pesanti) senza salti | 0% non mostrate, 300 immagini su 300 ogni 10 secondi |
| File unico oltre i 4 GB (5 h 34 min), più di un'ora con salti fino a 5 h 23 min | Si vede nell'elenco; acceleratore sempre acceso, 116.241 immagini, 0 perse, 0,4% non mostrate |
| DVD originale (menu, capitoli, salti, cambio audio) | Funziona come prima |

Dettagli nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-462). Le novità della 4.6.1 e delle versioni precedenti sono nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-461).

**Ringraziamenti:**

- **Umberto Parisi** (rmonic79) dei **Beceri Videoludici**: la funzione Regola CRT è stata possibile grazie al suo lavoro su [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust). Vi consiglio di seguire il loro canale YouTube: [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).
- **Max Riptide**, per le numerose prove fatte e i suggerimenti.
- **mad4gith**, per le idee suggerite per migliorare MMC.
- **FDA**, per il supporto tecnico e l'aiuto a chiudere il seme 12 del core FPGA.

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD: contiene tutto quello che serve a MMC. Aggiorna sempre tutti i file insieme. **Dalla 4.6.1 o 4.6:** lancia **Update_Mister_Media_Center** dal menu Scripts oppure estrai lo zip sopra i file vecchi, poi riavvia (il core è cambiato); `MiSTer.ini` e `mediacenter.ini` restano come sono. **Dalla 4.0:** cancella anche il vecchio script `/media/fat/Scripts/mediacenter.sh` (altrimenti negli Scripts compaiono due voci). Chi non ha ancora lo script lo scarica una volta via ssh: `cd /media/fat/Scripts && curl -sSfL --cacert /etc/ssl/certs/cacert.pem -O https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/Scripts/Update_Mister_Media_Center.sh` ([guida](https://github.com/bonna79/MiSTer_MediaCenter#installare-o-aggiornare-con-lo-script)).

| File | Cartella |
| --- | --- |
| `MiSTer_MediaCenter` (Main) | `/media/fat/` |
| `mediacenter` (lettore ARM) | `/media/fat/linux/` |
| `menu_mediacenter.rbf` (core del menu) | `/media/fat/linux/` |
| `Mister_Media_Center.sh` (script del lettore) | `/media/fat/Scripts/` |
| `Update_Mister_Media_Center.sh` (script che installa o aggiorna) | `/media/fat/Scripts/` |
| `libdvdcss.so.2` | `/media/fat/dvdcss/` |
| `MMC_MiSTer_ini.txt` (righe per `MiSTer.ini`) | `/media/fat/` |

MD5 dei file nello zip: `MiSTer_MediaCenter` 168953fa87c9b2dc7c82f38d0f79c358, `mediacenter` 9471edda259c7f16cf19dd51cf22a272, `menu_mediacenter.rbf` 4e9b7bb85f167409ec1c2f5157a112a8, `Mister_Media_Center.sh` 395f18116ad0a3156ccc3500e551e2d2, `Update_Mister_Media_Center.sh` ea5d56246f3cb799fe00e7830cd4f0c4, `libdvdcss.so.2` 7a140afb27296b1a7417358bbcc44cfd.

Copia in `/media/fat/MiSTer.ini` le righe di `MMC_MiSTer_ini.txt`: `vga_scaler=0`, `forced_scandoubler=0` (e `composite_sync=1` con la SCART) in `[MiSTer]`, `main=MiSTer_MediaCenter` in `[menu]` e `main=MiSTer_Physical-CD` in `[A0CD-*]`. **Non mettere `main=MiSTer_MediaCenter` nella sezione `[MiSTer]`: va solo in `[menu]`.** **Con `direct_video=1` (convertitori HDMI→VGA, scaler esterni come RetroTINK) MMC non usa la sua uscita CRT:** sulla VGA arriva l'immagine dello scaler, Regola CRT non ha effetto e l'elenco può uscire ingrandito o tagliato. Per un CRT a 15 kHz sull'uscita analogica DB-15 lascia `direct_video=0` ([dettagli](https://github.com/bonna79/MiSTer_MediaCenter#uscita-video-hdmi-e-crt-a-15-khz)). Il Main di [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases), per i giochi su disco, si scarica dal suo progetto. Riavvia il MiSTer. Guida completa nel [README](https://github.com/bonna79/MiSTer_MediaCenter#installazione).

**I vostri commenti sono molto preziosi:** scrivetemi, anche quando funziona tutto. In caso di problemi apri una segnalazione in [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) con foto dello schermo e i file `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` e `/tmp/mediacenter_crt`.

---

**English:** **MiSTer MediaCenter 4.6.2** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB, network and Jellyfin server, over HDMI and on a 15 kHz CRT.

**What's new in 4.6.2:**

- **Accelerator stays on after jumps:** with PgDn/PgUp (fast forward and rewind) and with left/right, after some jumps the FPGA could still receive commands from the previous position, answered "unexpected command" and the player turned the accelerator off until the end of the file, so 1080p became jerky. The problem was also in 4.6.1. Now after every jump the core empties the whole queue of old commands. The new core uses seed 16, the only one that meets all timing at 100 °C and -40 °C, in the slow and fast models, for both setup and hold.
- **Files over 2 GB in the lists:** MKV or MP4 films larger than 2 GB did not appear, not even in a plain folder. Now they are listed and open like the others, also beyond 4 GB. The same fix applies to DVD `.iso` images.
- **New picture** reading Ver. 4.6.2, shown when the script is opened from Scripts.

The menu core `menu_mediacenter.rbf` and the `mediacenter` player change: the Main is built from the same sources as in 4.6.1. **Restart the MiSTer after updating**, because the core changes.

**Complete documentation (PDF, 22 pages):** [English](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.2/MMC_4.6.2_documentation_complete_EN.pdf) · [italiano](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.2/MMC_4.6.2_documentazione_completa_IT.pdf). It explains the FPGA chip, the ARM player, the hand-offs between ARM and FPGA, the HDMI and CRT outputs, what is new from 4.5 to 4.6.2 (languages, settings, subtitles, update script, accelerator after jumps) and how to rebuild everything from scratch. The PDFs are also in the [docs](https://github.com/bonna79/MiSTer_MediaCenter/tree/main/docs) folder.

Tested on a real MiSTer on 21 and 22 November 2026, in two rounds (the second after switching the MiSTer fully off), on a Sony Trinitron CRT (SCART RGB) and HDMI together:

| Test | Result |
| --- | --- |
| One Piece 1080p, 567 s with about 38 jumps | Accelerator always on, 0 pictures lost by the FPGA, 1.8% not shown (around the jumps) |
| One Piece 720p after power-off, about 24 jumps | Accelerator always on, 0 lost, 2.7% not shown (around the jumps) |
| HTC (heavy scenes) without jumps | 0% not shown, 300 of 300 pictures every 10 seconds |
| Single file over 4 GB (5 h 34 min), over an hour with jumps up to 5 h 23 min | Listed; accelerator always on, 116,241 pictures, 0 lost, 0.4% not shown |
| Original DVD (menus, chapters, jumps, audio switch) | Works as before |

Details in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-462). What's new in 4.6.1 and earlier versions is in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-461).

**Thanks:** Umberto Parisi (rmonic79) of the Beceri Videoludici, whose work on [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust) made CRT Adjust possible ([YouTube channel](https://youtube.com/@rmonic79_ibecerivideoludici)); Max Riptide for the many tests and suggestions; mad4gith for the ideas to improve MMC; FDA for the technical support and the help in closing timing on seed 12 of the FPGA core.

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card: it holds everything MMC needs. Always update all files together (**from 4.6.1 or 4.6:** start **Update_Mister_Media_Center** from the Scripts menu or extract the zip over the old files, then restart (the core changed); `MiSTer.ini` and `mediacenter.ini` stay as they are; **from 4.0:** also delete the old script `/media/fat/Scripts/mediacenter.sh`, otherwise Scripts shows two entries): Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, scripts `Mister_Media_Center.sh` and `Update_Mister_Media_Center.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`, `MMC_MiSTer_ini.txt` to `/media/fat/` (MD5s as in the Italian table above). If you do not have the update script yet, download it once over ssh: `cd /media/fat/Scripts && curl -sSfL --cacert /etc/ssl/certs/cacert.pem -O https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/Scripts/Update_Mister_Media_Center.sh` ([guide](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installing-or-updating-with-the-script)). Copy the lines of `MMC_MiSTer_ini.txt` into `/media/fat/MiSTer.ini`. **Do not put `main=MiSTer_MediaCenter` in the `[MiSTer]` section: it goes only in `[menu]`.** **With `direct_video=1` (HDMI→VGA converters, external scalers such as RetroTINK) MMC does not use its own CRT output:** VGA gets the scaler's picture, CRT Adjust has no effect and the list may look enlarged or cut. For a 15 kHz CRT on the DB-15 analog output keep `direct_video=0` ([details](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#video-output-hdmi-and-15-khz-crt)). The [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) Main, for game discs, is downloaded from its own project. Restart the MiSTer. Full guide in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installation).

**Your feedback is very valuable:** write to me, even when everything works. If there are problems, open a report on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) with photos of the screen and the `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` and `/tmp/mediacenter_crt` files.
