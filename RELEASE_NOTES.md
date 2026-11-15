<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 4.6.1** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB, rete e server Jellyfin, su HDMI e su CRT a 15 kHz.

**Novità della 4.6.1:**

- **Menu in spagnolo, francese e tedesco**, oltre all'italiano e all'inglese: dalla pagina Impostazioni (riga Lingua menu: Italiano, English, Español, Français, Deutsch) oppure con `MC_LANG=es`, `MC_LANG=fr` o `MC_LANG=de` in `mediacenter.ini`. Valgono per elenchi, comandi durante il video di DVD, Video CD, Super Video CD, DivX/Xvid, MKV e MP4, schermata audio, Jellyfin, Regola CRT e Impostazioni. Il carattere piccolo dei menu ha ora le lettere accentate (à, é, ñ, ç, ü, ß…). Cinese e giapponese arriveranno più avanti.
- **Schermata audio più leggibile sul CRT:** la riga dei tasti in basso e le scritte sotto i pulsanti ora sono gialle (FFFF00) invece che grigio scuro.
- **Script per installare o aggiornare MMC nello zip:** `Update_Mister_Media_Center.sh` (cartella `Scripts`). Dal menu Scripts scarica l'ultima release da GitHub: su un MiSTer senza MMC installa tutto, se MMC c'è già sostituisce solo i file cambiati. Controlla il file scaricato (dimensione, impronta sha256 di GitHub, test dello zip), salva i vecchi file in `/media/fat/linux/mmc_backup`, chiede di riavviare se cambiano il Main o il core e non tocca mai `MiSTer.ini` né `mediacenter.ini`. Se in `MiSTer.ini` manca `main=MiSTer_MediaCenter` nella sezione `[menu]`, ricorda di copiare le righe di `MMC_MiSTer_ini.txt`.

Cambiano solo il lettore `mediacenter` e gli script: il Main e il core del menu (seed 12) nascono dagli stessi sorgenti della 4.6.

**Documentazione completa (PDF, 21 pagine):** [italiano](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.1/MMC_4.6.1_documentazione_completa_IT.pdf) · [English](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.1/MMC_4.6.1_documentation_complete_EN.pdf). Spiega il chip FPGA, il lettore ARM, i passaggi fra ARM e FPGA, le uscite HDMI e CRT, le novità dalla 4.5 alla 4.6.1 (lingue, impostazioni, sottotitoli, script di aggiornamento) e come ricostruire tutto da zero. I PDF sono anche nella cartella [docs](https://github.com/bonna79/MiSTer_MediaCenter/tree/main/docs).

Provata su un MiSTer vero il 14 e il 15 novembre 2026, su CRT Sony Trinitron (SCART RGB) e HDMI insieme:

| Prova | Risultato |
| --- | --- |
| Menu in spagnolo, francese e tedesco, anche dalla pagina Impostazioni | Funziona |
| Lettere accentate nei menu | Funzionano |
| Schermata audio con la riga dei tasti e le scritte dei pulsanti gialle | Funziona |
| Aggiornamento con lo script: Main in uso e lettore sostituiti, seconda volta "Hai gia' la versione v4.6" | Funziona |
| Promemoria dello script quando `MiSTer.ini` non avvia ancora il Main di MMC | Funziona |

Dettagli nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-461). Le novità della 4.6 e delle versioni precedenti sono nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-46).

**Ringraziamenti:**

- **Umberto Parisi** (rmonic79) dei **Beceri Videoludici**: la funzione Regola CRT è stata possibile grazie al suo lavoro su [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust). Vi consiglio di seguire il loro canale YouTube: [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).
- **Max Riptide**, per le numerose prove fatte e i suggerimenti.
- **mad4gith**, per le idee suggerite per migliorare MMC.
- **FDA**, per il supporto tecnico e l'aiuto a chiudere il seme 12 del core FPGA.

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD: contiene tutto quello che serve a MMC. Aggiorna sempre tutti i file insieme. **Dalla 4.6:** lancia **Update_Mister_Media_Center** dal menu Scripts oppure estrai lo zip sopra i file vecchi, poi riavvia; `MiSTer.ini` e `mediacenter.ini` restano come sono. **Dalla 4.0:** cancella anche il vecchio script `/media/fat/Scripts/mediacenter.sh` (altrimenti negli Scripts compaiono due voci). Chi non ha ancora lo script lo scarica una volta via ssh: `cd /media/fat/Scripts && curl -sSfL --cacert /etc/ssl/certs/cacert.pem -O https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/Scripts/Update_Mister_Media_Center.sh` ([guida](https://github.com/bonna79/MiSTer_MediaCenter#installare-o-aggiornare-con-lo-script)).

| File | Cartella |
| --- | --- |
| `MiSTer_MediaCenter` (Main) | `/media/fat/` |
| `mediacenter` (lettore ARM) | `/media/fat/linux/` |
| `menu_mediacenter.rbf` (core del menu) | `/media/fat/linux/` |
| `Mister_Media_Center.sh` (script del lettore) | `/media/fat/Scripts/` |
| `Update_Mister_Media_Center.sh` (script che installa o aggiorna) | `/media/fat/Scripts/` |
| `libdvdcss.so.2` | `/media/fat/dvdcss/` |
| `MMC_MiSTer_ini.txt` (righe per `MiSTer.ini`) | `/media/fat/` |

Copia in `/media/fat/MiSTer.ini` le righe di `MMC_MiSTer_ini.txt`: `vga_scaler=0`, `forced_scandoubler=0` (e `composite_sync=1` con la SCART) in `[MiSTer]`, `main=MiSTer_MediaCenter` in `[menu]` e `main=MiSTer_Physical-CD` in `[A0CD-*]`. **Non mettere `main=MiSTer_MediaCenter` nella sezione `[MiSTer]`: va solo in `[menu]`.** Il Main di [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases), per i giochi su disco, si scarica dal suo progetto. Riavvia il MiSTer. Guida completa nel [README](https://github.com/bonna79/MiSTer_MediaCenter#installazione).

**I vostri commenti sono molto preziosi:** scrivetemi, anche quando funziona tutto. In caso di problemi apri una segnalazione in [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) con foto dello schermo e i file `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` e `/tmp/mediacenter_crt`.

---

**English:** **MiSTer MediaCenter 4.6.1** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB, network and Jellyfin server, over HDMI and on a 15 kHz CRT.

**What's new in 4.6.1:**

- **Spanish, French and German menus**, in addition to Italian and English: from the Settings page (Menu language row: Italiano, English, Español, Français, Deutsch) or with `MC_LANG=es`, `MC_LANG=fr` or `MC_LANG=de` in `mediacenter.ini`. They cover lists, controls during DVD, Video CD, Super Video CD, DivX/Xvid, MKV and MP4 playback, audio screen, Jellyfin, CRT Adjust and Settings. The small menu font now has accented letters (à, é, ñ, ç, ü, ß…). Chinese and Japanese will come later.
- **Audio screen easier to read on a CRT:** the keys line at the bottom and the captions under the buttons are now yellow (FFFF00) instead of dark grey.
- **Install or update script in the zip:** `Update_Mister_Media_Center.sh` (`Scripts` folder). From the Scripts menu it downloads the latest release from GitHub: on a MiSTer without MMC it installs everything, if MMC is already there it replaces only the files that changed. It checks the download (size, GitHub's sha256, zip test), saves the old files in `/media/fat/linux/mmc_backup`, asks you to restart if the Main or the core changed and never touches `MiSTer.ini` or `mediacenter.ini`. If `MiSTer.ini` has no `main=MiSTer_MediaCenter` in the `[menu]` section, it reminds you to copy the lines of `MMC_MiSTer_ini.txt`.

Only the `mediacenter` player and the scripts change: the Main and the menu core (seed 12) are built from the same sources as in 4.6.

**Complete documentation (PDF, 21 pages):** [English](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.1/MMC_4.6.1_documentation_complete_EN.pdf) · [italiano](https://github.com/bonna79/MiSTer_MediaCenter/releases/download/v4.6.1/MMC_4.6.1_documentazione_completa_IT.pdf). It explains the FPGA chip, the ARM player, the hand-offs between ARM and FPGA, the HDMI and CRT outputs, what is new from 4.5 to 4.6.1 (languages, settings, subtitles, update script) and how to rebuild everything from scratch. The PDFs are also in the [docs](https://github.com/bonna79/MiSTer_MediaCenter/tree/main/docs) folder.

Tested on a real MiSTer on 14 and 15 November 2026, on a Sony Trinitron CRT (SCART RGB) and HDMI together:

| Test | Result |
| --- | --- |
| Spanish, French and German menus, also from the Settings page | Working |
| Accented letters in the menus | Working |
| Audio screen with yellow keys line and button captions | Working |
| Update with the script: Main in use and player replaced, second run "You already have version v4.6" | Working |
| Script reminder when `MiSTer.ini` does not start the MMC Main yet | Working |

Details in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-461). What's new in 4.6 and earlier versions is in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-46).

**Thanks:** Umberto Parisi (rmonic79) of the Beceri Videoludici, whose work on [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust) made CRT Adjust possible ([YouTube channel](https://youtube.com/@rmonic79_ibecerivideoludici)); Max Riptide for the many tests and suggestions; mad4gith for the ideas to improve MMC; FDA for the technical support and the help in closing timing on seed 12 of the FPGA core.

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card: it holds everything MMC needs. Always update all files together (**from 4.6:** start **Update_Mister_Media_Center** from the Scripts menu or extract the zip over the old files, then restart; `MiSTer.ini` and `mediacenter.ini` stay as they are; **from 4.0:** also delete the old script `/media/fat/Scripts/mediacenter.sh`, otherwise Scripts shows two entries): Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, scripts `Mister_Media_Center.sh` and `Update_Mister_Media_Center.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`, `MMC_MiSTer_ini.txt` to `/media/fat/`. If you do not have the update script yet, download it once over ssh: `cd /media/fat/Scripts && curl -sSfL --cacert /etc/ssl/certs/cacert.pem -O https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/Scripts/Update_Mister_Media_Center.sh` ([guide](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installing-or-updating-with-the-script)). Copy the lines of `MMC_MiSTer_ini.txt` into `/media/fat/MiSTer.ini`. **Do not put `main=MiSTer_MediaCenter` in the `[MiSTer]` section: it goes only in `[menu]`.** The [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) Main, for game discs, is downloaded from its own project. Restart the MiSTer. Full guide in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installation).

**Your feedback is very valuable:** write to me, even when everything works. If there are problems, open a report on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) with photos of the screen and the `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` and `/tmp/mediacenter_crt` files.
