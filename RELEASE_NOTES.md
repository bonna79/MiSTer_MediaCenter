<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 4.6.3** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB, rete e server Jellyfin, su HDMI e su CRT a 15 kHz.

**Novità della 4.6.3:**

- **Menu in giapponese e in cinese semplificato:** si sceglie dalla pagina Impostazioni, riga Lingua menu (日本語, 简体中文), oppure con `MC_LANG=ja` o `MC_LANG=zh` in `mediacenter.ini`. I caratteri vengono dal carattere libero Noto Sans CJK, disegnati in anticipo per ogni dimensione usata dai menu, così si leggono bene sia sul CRT a 15 kHz sia sul televisore LCD. La schermata audio resta in inglese.
- **Bandiera della lingua:** nella pagina Impostazioni, accanto al nome della lingua del menu c'è la sua bandiera, ben visibile anche sul CRT.
- **MMC dall'elenco dei core:** il nuovo file `_Utility/Mister_Media_Center.mgl` mette la voce **Mister_Media_Center** nella cartella Utility dell'elenco dei core del MiSTer. Dal menu del MiSTer apre MMC come lo script dagli Scripts. Dentro un core che usa un altro Main (quello ufficiale o quello di Physical Disc) il MiSTer scrive "No rbf found!": torna prima al menu.
- **Nuova immagine** con la scritta Ver. 4.6.3, mostrata solo quando apri MMC dallo script o dal file MGL. Quando MMC parte da solo per un DVD, un Video CD, un CD o DVD dati o un altro disco, l'immagine non si vede.

Cambiano il Main `MiSTer_MediaCenter` e il lettore `mediacenter`; il core del menu `menu_mediacenter.rbf` è lo stesso della 4.6.2. **Dopo l'aggiornamento riavvia il MiSTer**, perché cambia il Main.

**Documentazione completa (PDF):** [italiano](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.3/MMC_4.6.3_documentazione_completa_IT.pdf) · [English](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.3/MMC_4.6.3_documentation_complete_EN.pdf). Spiega il chip FPGA, il lettore ARM, i passaggi fra ARM e FPGA, le uscite HDMI e CRT, le novità dalla 4.5 alla 4.6.3 (lingue, impostazioni, sottotitoli, script di aggiornamento, acceleratore dopo i salti, giapponese e cinese, file MGL) e come ricostruire tutto da zero. I PDF sono anche nella cartella [docs](https://github.com/bonna79/MiSTer_MediaCenter/tree/main/docs).

Provata su un MiSTer vero dal 23 al 24 novembre 2026, su CRT Sony Trinitron (SCART RGB) e HDMI insieme:

| Prova | Risultato |
| --- | --- |
| Menu in giapponese e in cinese, anche dalla pagina Impostazioni | Si legge molto bene sul CRT e sul televisore LCD |
| Bandiera accanto a Lingua menu | Funziona |
| MMC dal file MGL nel menu del MiSTer | Si apre come dallo script |
| MGL scelto dentro un core normale e dentro un core A0CD (altro Main) | "No rbf found!", come previsto |
| Disco all'accensione: DVD video, DVD dati con DivX, XviD e MKV | Partono da soli come prima |
| One Piece 1080p con salti | Acceleratore sempre acceso, 0 immagini perse dall'FPGA |

Dettagli nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-463). Le novità della 4.6.2 e delle versioni precedenti sono nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-462).

**Ringraziamenti:**

- **Umberto Parisi** (rmonic79) dei **Beceri Videoludici**: la funzione Regola CRT è stata possibile grazie al suo lavoro su [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust). Vi consiglio di seguire il loro canale YouTube: [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).
- **Max Riptide**, per le numerose prove fatte e i suggerimenti.
- **mad4gith**, per le idee suggerite per migliorare MMC.
- **FDA**, per il supporto tecnico e l'aiuto a chiudere il seme 12 del core FPGA.

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD: contiene tutto quello che serve a MMC. Aggiorna sempre tutti i file insieme. **Dalla 4.6.2, 4.6.1 o 4.6:** lancia **Update_Mister_Media_Center** dal menu Scripts oppure estrai lo zip sopra i file vecchi, poi riavvia (il Main è cambiato); `MiSTer.ini` e `mediacenter.ini` restano come sono. **Dalla 4.0:** cancella anche il vecchio script `/media/fat/Scripts/mediacenter.sh` (altrimenti negli Scripts compaiono due voci). Chi non ha ancora lo script lo scarica una volta via ssh: `cd /media/fat/Scripts && curl -sSfL --cacert /etc/ssl/certs/cacert.pem -O https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/Scripts/Update_Mister_Media_Center.sh` ([guida](https://github.com/bonna79/MiSTer_MediaCenter#installare-o-aggiornare-con-lo-script)).

| File | Cartella |
| --- | --- |
| `MiSTer_MediaCenter` (Main) | `/media/fat/` |
| `mediacenter` (lettore ARM) | `/media/fat/linux/` |
| `menu_mediacenter.rbf` (core del menu) | `/media/fat/linux/` |
| `Mister_Media_Center.sh` (script del lettore) | `/media/fat/Scripts/` |
| `Update_Mister_Media_Center.sh` (script che installa o aggiorna) | `/media/fat/Scripts/` |
| `libdvdcss.so.2` | `/media/fat/dvdcss/` |
| `Mister_Media_Center.mgl` (voce nell'elenco dei core) | `/media/fat/_Utility/` |
| `MMC_MiSTer_ini.txt` (righe per `MiSTer.ini`) | `/media/fat/` |

MD5 dei file nello zip: MD5_DA_INSERIRE

Copia in `/media/fat/MiSTer.ini` le righe di `MMC_MiSTer_ini.txt`: `vga_scaler=0`, `forced_scandoubler=0` (e `composite_sync=1` con la SCART) in `[MiSTer]`, `main=MiSTer_MediaCenter` in `[menu]` e `main=MiSTer_Physical-CD` in `[A0CD-*]`. **Non mettere `main=MiSTer_MediaCenter` nella sezione `[MiSTer]`: va solo in `[menu]`.** **Con `direct_video=1` (convertitori HDMI→VGA, scaler esterni come RetroTINK) MMC non usa la sua uscita CRT:** sulla VGA arriva l'immagine dello scaler, Regola CRT non ha effetto e l'elenco può uscire ingrandito o tagliato. Per un CRT a 15 kHz sull'uscita analogica DB-15 lascia `direct_video=0` ([dettagli](https://github.com/bonna79/MiSTer_MediaCenter#uscita-video-hdmi-e-crt-a-15-khz)). Il Main di [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases), per i giochi su disco, si scarica dal suo progetto. Riavvia il MiSTer. Guida completa nel [README](https://github.com/bonna79/MiSTer_MediaCenter#installazione).

**I vostri commenti sono molto preziosi:** scrivetemi, anche quando funziona tutto. In caso di problemi apri una segnalazione in [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) con foto dello schermo e i file `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` e `/tmp/mediacenter_crt`.

---

**English:** **MiSTer MediaCenter 4.6.3** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB, network and Jellyfin server, over HDMI and on a 15 kHz CRT.

**What's new in 4.6.3:**

- **Menu in Japanese and Simplified Chinese:** choose it on the Settings page, Menu language row (日本語, 简体中文), or with `MC_LANG=ja` or `MC_LANG=zh` in `mediacenter.ini`. The characters come from the free Noto Sans CJK font, drawn in advance for every size the menus use, so they read well both on the 15 kHz CRT and on the LCD TV. The audio screen stays in English.
- **Language flag:** on the Settings page, the flag of the menu language sits beside its name, easy to see on the CRT too.
- **MMC from the core list:** the new file `_Utility/Mister_Media_Center.mgl` adds the **Mister_Media_Center** entry to the Utility folder of the MiSTer core list. From the MiSTer menu it opens MMC like the script from Scripts. Inside a core that uses another Main (the official one or the Physical Disc one) the MiSTer shows "No rbf found!": go back to the menu first.
- **New picture** reading Ver. 4.6.3, shown only when you open MMC from the script or from the MGL file. When MMC starts by itself for a DVD, a Video CD, a data CD or DVD or another disc, the picture is not shown.

The `MiSTer_MediaCenter` Main and the `mediacenter` player change; the menu core `menu_mediacenter.rbf` is the same as in 4.6.2. **Restart the MiSTer after updating**, because the Main changes.

**Complete documentation (PDF):** [English](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.3/MMC_4.6.3_documentation_complete_EN.pdf) · [italiano](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.3/MMC_4.6.3_documentazione_completa_IT.pdf). It explains the FPGA chip, the ARM player, the hand-offs between ARM and FPGA, the HDMI and CRT outputs, what is new from 4.5 to 4.6.3 (languages, settings, subtitles, update script, accelerator after jumps, Japanese and Chinese, MGL file) and how to rebuild everything from scratch. The PDFs are also in the [docs](https://github.com/bonna79/MiSTer_MediaCenter/tree/main/docs) folder.

Tested on a real MiSTer from 23 to 24 November 2026, on a Sony Trinitron CRT (SCART RGB) and HDMI together:

| Test | Result |
| --- | --- |
| Japanese and Chinese menus, also from the Settings page | Read very well on the CRT and on the LCD TV |
| Flag beside Menu language | Working |
| MMC from the MGL file in the MiSTer menu | Opens as from the script |
| MGL chosen inside a normal core and inside an A0CD core (other Main) | "No rbf found!", as expected |
| Disc at power-on: video DVD, data DVD with DivX, XviD and MKV | Start by themselves as before |
| One Piece 1080p with jumps | Accelerator always on, 0 pictures lost by the FPGA |

Details in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-463). What's new in 4.6.2 and earlier versions is in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-462).

**Thanks:** Umberto Parisi (rmonic79) of the Beceri Videoludici, whose work on [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust) made CRT Adjust possible ([YouTube channel](https://youtube.com/@rmonic79_ibecerivideoludici)); Max Riptide for the many tests and suggestions; mad4gith for the ideas to improve MMC; FDA for the technical support and the help in closing timing on seed 12 of the FPGA core.

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card: it holds everything MMC needs. Always update all files together (**from 4.6.2, 4.6.1 or 4.6:** start **Update_Mister_Media_Center** from the Scripts menu or extract the zip over the old files, then restart (the Main changed); `MiSTer.ini` and `mediacenter.ini` stay as they are; **from 4.0:** also delete the old script `/media/fat/Scripts/mediacenter.sh`, otherwise Scripts shows two entries): Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, scripts `Mister_Media_Center.sh` and `Update_Mister_Media_Center.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`, `Mister_Media_Center.mgl` to `/media/fat/_Utility/`, `MMC_MiSTer_ini.txt` to `/media/fat/` (MD5s as in the Italian table above). If you do not have the update script yet, download it once over ssh: `cd /media/fat/Scripts && curl -sSfL --cacert /etc/ssl/certs/cacert.pem -O https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/Scripts/Update_Mister_Media_Center.sh` ([guide](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installing-or-updating-with-the-script)). Copy the lines of `MMC_MiSTer_ini.txt` into `/media/fat/MiSTer.ini`. **Do not put `main=MiSTer_MediaCenter` in the `[MiSTer]` section: it goes only in `[menu]`.** **With `direct_video=1` (HDMI→VGA converters, external scalers such as RetroTINK) MMC does not use its own CRT output:** VGA gets the scaler's picture, CRT Adjust has no effect and the list may look enlarged or cut. For a 15 kHz CRT on the DB-15 analog output keep `direct_video=0` ([details](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#video-output-hdmi-and-15-khz-crt)). The [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) Main, for game discs, is downloaded from its own project. Restart the MiSTer. Full guide in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installation).

**Your feedback is very valuable:** write to me, even when everything works. If there are problems, open a report on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) with photos of the screen and the `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` and `/tmp/mediacenter_crt` files.
