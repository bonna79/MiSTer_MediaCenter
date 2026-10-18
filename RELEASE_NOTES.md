<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 2.5** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB e rete, su HDMI e su CRT a 15 kHz.

**Novità della 2.5:**

La 2.5 è dedicata soprattutto ai televisori CRT e ai DVD.

- **Regola CRT:** una nuova schermata, l'ultima voce dello script **mediacenter**, adatta l'immagine al tuo CRT senza aprire il televisore. Regola **Larghezza**, **Posizione orizzontale**, **Altezza**, **Posizione verticale**, **Modo 15 kHz** e **Filtro tremolio**, e mostra la risoluzione **Misurata** sull'uscita. Dettagli in [Regola CRT](https://github.com/bonna79/MiSTer_MediaCenter#regola-crt-geometria). Questa funzione è stata possibile grazie al lavoro di Umberto Parisi dei [Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici) (vedi [Ringraziamenti](https://github.com/bonna79/MiSTer_MediaCenter#ringraziamenti)).
- **DVD interlacciati veri sul CRT:** il core del menu cambia immagine solo all'inizio di un semiquadro. I DVD PAL a 576i 50 Hz e gli NTSC a 480i 60 Hz scorrono fluidi, senza righe che saltano e senza immagini spezzate.
- **Elenchi disegnati per il CRT:** l'elenco dei file e Regola CRT sono disegnati a 640 punti con le righe del modo del CRT, con titoli e voci in grassetto. Le scritte restano intere e leggibili anche a 15 kHz.
- **Filtro tremolio** nel core FPGA: le scritte bianche sottili non sfarfallano più sull'uscita interlacciata.
- **Comandi e sottotitoli leggibili sul CRT:** caratteri più grandi, comandi su due righe con una riga per audio e sottotitoli, e i sottotitoli si spostano sopra i comandi quando sono visibili.
- **Righe di diagnosi nel log** (`mediacenter: diag ...`): per ogni scatto dicono quante immagini sono saltate e perché (lettura del disco, decodifica, disegno, audio). Servono a capire i problemi dalle vostre prove. `MC_DIAG=0` in `mediacenter.ini` le spegne.

Correzioni:

- **CRT dopo un gioco:** tornando al menu dopo un core, il MiSTer caricava il menu di serie e il CRT mostrava solo fruscio o l'avviso "modify MiSTer.ini" fino allo spegnimento. Ora il Main di MMC ricarica sempre il suo core del menu.
- **DVD sul CRT con righe che saltavano e immagini spezzate** ogni 10-15 secondi: corretto nel core, che usa l'indirizzo della nuova pagina solo quando è stabile, e nel lettore.
- **Cambio della traccia audio** dal 5.1 al commento a 2 canali: l'immagine si fermava per circa 5 secondi. Ora il video non aspetta mai e il nuovo audio parte dal punto del film.
- **DVD avviato dallo script mediacenter** più lento (18% di immagini saltate): ora il lettore usa tutti e due i core dell'ARM, comunque venga avviato.
- **Piccoli scatti durante il film** (100-200 ms): il lettore chiedeva lo stato del DVD a libdvdnav mentre questa leggeva il disco, e restava fermo ad aspettare. Ora lo stato lo copia chi legge il disco.
- **Menu dei DVD:** la scelta dei capitoli non scatta più, perché i pulsanti evidenziati vengono preparati una volta sola invece che a ogni immagine. Le scritte del menu appaiono subito e complete, invece che al secondo giro della musica: il lettore fa uscire dal decodificatore l'ultima immagine del menu appena il disco segnala la fine della sequenza video, come fanno i lettori da tavolo.
- **Tasti del joypad scritti sopra il menu del DVD:** la console di Linux ora resta nascosta mentre il lettore è aperto.
- **Disco che ripartiva da solo** dopo stop e uscita: il Main ignora il disco nel lettore finché non viene tolto.

Provata su un MiSTer vero il 17 e 18 ottobre 2026, su CRT Sony Trinitron via SCART RGB (anche su un secondo CRT): DVD PAL protetto con menu, scelta dei capitoli e tre tracce audio con il commento; 3709 immagini di film con solo 3 saltate; DivX e MP4.

**Ringraziamenti:**

- **Umberto Parisi** (rmonic79) dei **Beceri Videoludici**: la funzione Regola CRT è stata possibile grazie al suo lavoro su [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust). Lo ringrazio e vi consiglio di seguire il loro canale YouTube: [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).
- **Max Riptide**, per le numerose prove fatte e i suggerimenti.
- **Simone Roncaglio**, per le idee suggerite per migliorare MMC.

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD e aggiorna sempre tutti i file insieme, perché Main, lettore e core del menu sono cambiati tutti e tre:

| File | Cartella |
| --- | --- |
| `MiSTer_MediaCenter` (Main) | `/media/fat/` |
| `mediacenter` (lettore ARM) | `/media/fat/linux/` |
| `menu_mediacenter.rbf` (core del menu) | `/media/fat/linux/` |
| `mediacenter.sh` (script) | `/media/fat/Scripts/` |
| `libdvdcss.so.2` | `/media/fat/dvdcss/` |

In `MiSTer.ini`, senza forzature dello scaler nella sezione `[menu]`:

```ini
[MiSTer]
vga_scaler=0          ; per il CRT a 15 kHz
forced_scandoubler=0
composite_sync=1      ; solo con cavo SCART
menu_pal=1            ; facoltativo: elenchi a 576i 50 Hz come i DVD PAL

[menu]
main=MiSTer_MediaCenter

[A0CD-*]
main=MiSTer_Physical-CD
```

Riavvia il MiSTer. Guida completa, parte tecnica (come lavorano ARM e FPGA) e risultati delle prove nel [README](https://github.com/bonna79/MiSTer_MediaCenter#readme).

**I vostri commenti sono molto preziosi:** scrivetemi, anche quando funziona tutto. In caso di problemi apri una segnalazione in [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) con foto dello schermo e i file `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` e `/tmp/mediacenter_crt`.

---

**English:** **MiSTer MediaCenter 2.5** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB and network, over HDMI and on a 15 kHz CRT.

**What's new in 2.5:**

Version 2.5 is mostly about CRT TVs and DVDs.

- **CRT Adjust:** a new screen, the last entry of the **mediacenter** script, fits the picture to your CRT without opening the TV. It sets **Width**, **Horizontal position**, **Height**, **Vertical position**, **15 kHz mode** and **Flicker filter**, and shows the resolution **measured** on the output. Details in [CRT Adjust](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#crt-adjust-geometry). This feature was made possible by the work of Umberto Parisi of [Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici) (see [Thanks](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#thanks)).
- **True interlaced DVDs on the CRT:** the menu core changes picture only at the start of a field. PAL DVDs at 576i 50 Hz and NTSC ones at 480i 60 Hz play smoothly, with no jumping lines and no torn pictures.
- **Lists drawn for the CRT:** the file list and CRT Adjust are drawn 640 pixels wide with the lines of the CRT mode, with bold titles and entries. Text stays whole and readable at 15 kHz too.
- **Flicker filter** in the FPGA core: thin white text no longer flickers on the interlaced output.
- **Controls and subtitles readable on the CRT:** larger characters, controls on two rows with a line for audio and subtitles, and subtitles move above the controls while these are shown.
- **Diagnosis lines in the log** (`mediacenter: diag ...`): for every stutter they say how many pictures were dropped and why (disc read, decoding, drawing, sound). They help understand problems from your tests. `MC_DIAG=0` in `mediacenter.ini` turns them off.

Fixes:

- **CRT after a game:** back in the menu after a core, the MiSTer loaded the stock menu and the CRT showed only noise or the "modify MiSTer.ini" warning until power off. The MMC Main now always reloads its own menu core.
- **DVDs on the CRT with jumping lines and torn pictures** every 10-15 seconds: fixed in the core, which takes the new page address only once it is stable, and in the player.
- **Audio track change** from 5.1 to the 2-channel commentary: the picture stopped for about 5 seconds. The video now never waits and the new sound starts at the film's position.
- **DVD started from the mediacenter script** slower (18% of pictures dropped): the player now uses both ARM cores however it is started.
- **Small stutters during the film** (100-200 ms): the player asked libdvdnav for the DVD state while it was reading the disc, and had to wait. The state is now copied by the disc reader.
- **DVD menus:** chapter selection no longer stutters, because highlighted buttons are prepared once instead of for every picture. Menu text appears at once and complete, instead of on the second loop of the music: the player pushes the menu's last picture out of the decoder as soon as the disc signals the end of the video sequence, like set-top players.
- **Joypad keys written over the DVD menu:** the Linux console now stays hidden while the player is open.
- **Disc starting again by itself** after stop and exit: the Main ignores the disc in the drive until it is removed.

Tested on a real MiSTer on 17 and 18 October 2026, on Sony Trinitron CRTs over SCART RGB (also on a second CRT): protected PAL DVD with menus, chapter selection and three audio tracks with the commentary; 3709 film pictures with only 3 dropped; DivX and MP4.

**Thanks:**

- **Umberto Parisi** (rmonic79) of **Beceri Videoludici**: CRT Adjust was made possible by his work on [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust). Thanks to him, and I recommend following their YouTube channel: [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).
- **Max Riptide**, for the many tests and suggestions.
- **Simone Roncaglio**, for the ideas suggested to improve MMC.

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card and always update all files together, because the Main, the player and the menu core have all changed: Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, script `mediacenter.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`. In `MiSTer.ini` put `vga_scaler=0`, `forced_scandoubler=0` (and `composite_sync=1` with SCART; optionally `menu_pal=1` for lists at 576i 50 Hz like PAL DVDs) in `[MiSTer]`, `main=MiSTer_MediaCenter` in `[menu]` with no scaler overrides, and `main=MiSTer_Physical-CD` in `[A0CD-*]`. Restart the MiSTer. Full guide, technical part (how the ARM and the FPGA work) and test results in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md).

**Your feedback is very valuable:** write to me, even when everything works. If there are problems, open a report on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) with photos of the screen and the `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` and `/tmp/mediacenter_crt` files.
