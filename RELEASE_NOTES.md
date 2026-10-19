<p align="center"><img src="https://raw.githubusercontent.com/bonna79/MiSTer_MediaCenter/main/docs/mmc.png" alt="MiSTer Media Center" width="720"></p>

> Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic). / Developed with the help of AI (Anthropic's Claude).

**MiSTer MediaCenter 2.6** trasforma il MiSTer in un lettore per DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4/MKV e musica, da disco, SD, USB e rete, su HDMI e su CRT a 15 kHz.

**Novità della 2.6:**

- **Video più larghi dello schermo molto più fluidi:** un MKV H.264 a 1440x810 perdeva il 63,8% delle immagini, ora l'1,4%. Il lettore restringe l'immagine mentre la copia e la conversione dei colori la fa l'FPGA, come per i video a 720p.
- **Ritorno da un gioco PSX con il Main di Physical Disc:** a volte, uscendo dal gioco, il menu resta al Main di Physical Disc e sul CRT compare l'avviso "modify MiSTer.ini". Ora lo script **mediacenter** se ne accorge, rimette il Main di MMC e il suo core del menu. Poi basta **lanciare di nuovo mediacenter da Scripts**: al secondo avvio funziona tutto, HDMI e CRT. Spiegazione completa nel [README](https://github.com/bonna79/MiSTer_MediaCenter#dopo-un-gioco-con-il-main-di-physical-disc).
- **Pubblicità e trailer dei DVD senza immagine** (per esempio Shrek 2 dopo la scelta della lingua): si sentivano audio e sottotitoli ma l'immagine restava sul menu. Ora si vedono a tempo con l'audio. Anche le animazioni dei menu ripartono bene dal secondo giro.
- **Diagnosi delle immagini nel log** per 30 secondi dopo ogni salto o cambio di parte del disco.
- Nel pacchetto c'è anche `MMC_MiSTer_ini.txt` con le righe da copiare in `MiSTer.ini`.

Provata su un MiSTer vero il 18 ottobre 2026: MKV 1440x810 fluido; Shrek 2 PAL su CRT con le pubblicità visibili; Star Wars Episodio III con menu, capitoli e cambi audio senza difetti; gioco PSX e poi script mediacenter con il CRT di nuovo a posto.

Tutte le novità della 2.5 (Regola CRT, DVD interlacciati veri sul CRT, filtro tremolio, menu dei DVD fluidi) sono descritte nel [README](https://github.com/bonna79/MiSTer_MediaCenter#novità-della-versione-25).

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

**English:** **MiSTer MediaCenter 2.6** turns the MiSTer into a player for DVD-Video, Video CD, Super Video CD, DivX/Xvid/MP4/MKV video and music, from disc, SD, USB and network, over HDMI and on a 15 kHz CRT.

**What's new in 2.6:**

- **Videos wider than the screen much smoother:** a 1440x810 H.264 MKV lost 63.8% of its pictures, now 1.4%. The player narrows the picture while copying it and the FPGA converts the colours, as for 720p videos.
- **Back from a PSX game with the Physical Disc Main:** sometimes, when you leave the game, the menu stays with the Physical Disc Main and the CRT shows the "modify MiSTer.ini" notice. Now the **mediacenter** script notices, puts back the MMC Main and its menu core. Then just **start mediacenter from Scripts again**: on the second start everything works, HDMI and CRT. Full explanation in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#after-a-game-with-the-physical-disc-main).
- **DVD adverts and trailers without picture** (for example Shrek 2 after choosing the language): sound and subtitles played but the picture stayed on the menu. Now they show in time with the sound. Menu animations also restart properly from the second loop.
- **Picture diagnosis in the log** for 30 seconds after every jump or change of part of the disc.
- The package also holds `MMC_MiSTer_ini.txt` with the lines to copy into `MiSTer.ini`.

Tested on a real MiSTer on 18 October 2026: 1440x810 MKV smooth; Shrek 2 PAL on a CRT with the adverts visible; Star Wars Episode III with menus, chapters and audio changes without faults; a PSX game and then the mediacenter script with the CRT back to normal.

Everything new in 2.5 (CRT Adjust, true interlaced DVDs on the CRT, flicker filter, smooth DVD menus) is described in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#whats-new-in-version-25).

**Thanks:** Umberto Parisi (rmonic79) of the Beceri Videoludici, whose work on [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust) made CRT Adjust possible ([YouTube channel](https://youtube.com/@rmonic79_ibecerivideoludici)); Max Riptide for the many tests and suggestions; mad4gith for the ideas to improve MMC.

**Install:** extract `MiSTer_MediaCenter.zip` to the root of the SD card: it holds everything MMC needs. Always update all files together: Main `MiSTer_MediaCenter` to `/media/fat/`, ARM player `mediacenter` and menu core `menu_mediacenter.rbf` to `/media/fat/linux/`, script `mediacenter.sh` to `/media/fat/Scripts/`, `libdvdcss.so.2` to `/media/fat/dvdcss/`, `MMC_MiSTer_ini.txt` to `/media/fat/`. Copy the lines of `MMC_MiSTer_ini.txt` into `/media/fat/MiSTer.ini`. The [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) Main, for game discs, is downloaded from its own project. Restart the MiSTer. Full guide in the [README](https://github.com/bonna79/MiSTer_MediaCenter/blob/main/README_EN.md#installation).

**Your feedback is very valuable:** write to me, even when everything works. If there are problems, open a report on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues) with photos of the screen and the `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` and `/tmp/mediacenter_crt` files.
