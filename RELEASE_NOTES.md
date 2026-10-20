<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 2.7** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB e rete, su HDMI e su CRT a 15 kHz.

**Novità della 2.7:** l'uscita per il CRT diventa quella di un vero lettore DVD.

- **CRT a 720 punti a 13,5 MHz** (standard DVD): i DVD arrivano al CRT punto per punto, le altre larghezze con un filtro orizzontale. Gli elenchi restano a 640 punti, al centro.
- **Comandi del lettore dentro il bordo rosso di Regola CRT**, il video resta a 720 punti pieni.
- **Modo 15 kHz Auto dagli fps del video:** 25/50 fps a 576i 50 Hz, 23,976/29,97/59,94 fps a 480i 60 Hz, sempre interlacciati.
- **DVD in YUV:** i colori dei DVD li converte l'FPGA, sottotitoli e menu disegnati in YUV (`MC_NOYUV=1` per tornare all'RGB).
- **Video 720p a piena risoluzione** (1280x720, prima 1280x360): scritte più nitide, sempre fluido (0,1% di immagini non mostrate).
- **Corretto:** un DVD dopo un video 720p partiva a metà risoluzione (720x288).
- **Immagine di MMC per 3 secondi** all'avvio dello script (`MC_SPLASH=0` la spegne).
- **Con il CRT consigliamo `MC_FIELDS=1`** in `/media/fat/linux/mediacenter.ini`: semiquadri dei DVD intatti sul CRT, fusi dallo scaler sull'HDMI. Esempio nel [README](https://github.com/bonna79/MiSTer_MediaCenter#4-impostazioni-del-lettore-facoltative).

Provata su un MiSTer vero il 19 e 20 ottobre 2026 su CRT Sony Trinitron e HDMI insieme: Star Wars Episodio III e Shrek 2 PAL; DVD in YUV con nessuna immagine saltata; MP4 720p a 1280x720 con una sola immagine non mostrata su 9528 e scritte perfette sul CRT.

Le novità delle versioni precedenti (2.6: video larghi fluidi, ritorno dai giochi PSX, pubblicità dei DVD) sono nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-26). Tutte le novità della 2.5 (Regola CRT, DVD interlacciati veri sul CRT, filtro tremolio, menu dei DVD fluidi) sono descritte nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-25).

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

**English:** **MiSTer MediaCenter 2.7** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB and network, over HDMI and on a 15 kHz CRT.

**What's new in 2.7:** the CRT output becomes that of a real DVD player.

- **720-pixel CRT output at 13.5 MHz** (the DVD standard): DVDs reach the CRT pixel for pixel, other widths through a horizontal filter. Lists stay 640 pixels wide, centred.
- **Player controls inside the red border of CRT Adjust**, the video stays a full 720 pixels.
- **Auto 15 kHz mode from the video's frame rate:** 25/50 fps at 576i 50 Hz, 23.976/29.97/59.94 fps at 480i 60 Hz, always interlaced.
- **DVDs in YUV:** the FPGA converts DVD colours, subtitles and menus drawn in YUV (`MC_NOYUV=1` goes back to RGB).
- **720p videos at full resolution** (1280x720, before 1280x360): sharper text, still smooth (0.1% of pictures not shown).
- **Fixed:** a DVD after a 720p video started at half resolution (720x288).
- **MMC picture for 3 seconds** when the script starts (`MC_SPLASH=0` turns it off).
- **With a CRT we recommend `MC_FIELDS=1`** in `/media/fat/linux/mediacenter.ini`: DVD fields whole on the CRT, blended by the scaler on HDMI. Example in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#4-player-settings-optional).

Tested on a real MiSTer on 19 and 20 October 2026 on a Sony Trinitron CRT and HDMI together: Star Wars Episode III and Shrek 2 PAL; DVDs in YUV with no dropped pictures; 720p MP4 at 1280x720 with only one picture not shown out of 9528 and perfect text on the CRT.

What's new in earlier versions (2.6: smooth wide videos, return from PSX games, DVD adverts) is in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-26). Everything new in 2.5 (CRT Adjust, true interlaced DVDs on the CRT, flicker filter, smooth DVD menus) is described in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-25).

**Thanks:** Umberto Parisi (rmonic79) of the Beceri Videoludici, whose work on [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust) made CRT Adjust possible ([YouTube channel](https://youtube.com/@rmonic79_ibecerivideoludici)); Max Riptide for the many tests and suggestions; mad4gith for the ideas to improve MMC.

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card: it holds everything MMC needs. Always update all files together: Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, script `mediacenter.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`, `MMC_MiSTer_ini.txt` to `/media/fat/`. Copy the lines of `MMC_MiSTer_ini.txt` into `/media/fat/MiSTer.ini`. The [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) Main, for game discs, is downloaded from its own project. Restart the MiSTer. Full guide in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installation).

**Your feedback is very valuable:** write to me, even when everything works. If there are problems, open a report on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) with photos of the screen and the `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` and `/tmp/mediacenter_crt` files.
