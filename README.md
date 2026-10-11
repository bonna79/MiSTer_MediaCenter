# MiSTer MediaCenter

**Italiano** · [English](#english)

MiSTer MediaCenter trasforma il MiSTer FPGA (DE10-Nano) in un lettore multimediale con un unico programma e un unico set di comandi. Legge:

- Video CD;
- DVD-Video, anche commerciali protetti, con i loro menu;
- file video DivX/Xvid/MP4/MKV/MPEG;
- musica MP3/Ogg/FLAC.

Le sorgenti sono il lettore ottico USB, la SD, le chiavette USB e le cartelle di rete.

L'ARM legge e decodifica dischi e file. Lo scaler dell'FPGA ingrandisce l'immagine fino a riempire lo schermo.

Il pacchetto contiene due parti:

- **`MiSTer_MediaCenter`**: il Main (il programma principale del MiSTer). Contiene tutte le funzioni di Physical Disc: avvio automatico dei dischi PSX, Saturn, Mega CD, PC Engine CD e degli altri core di `_Physical Disc Cores`, subcanale Q, LibCrypt della PSX dal disco originale e CD+G sul Mega CD. In più riconosce Video CD, DVD-Video e DVD dati e li apre con il lettore.
- **`mediacenter`**: il lettore che gira sull'ARM.

## Cosa fa

| Disco o file | Come si apre | Funzioni |
| --- | --- | --- |
| DVD-Video, anche protetti CSS | Da solo, all'inserimento | Menu del disco con i pulsanti, capitoli, tracce audio, sottotitoli, avanti/indietro veloce |
| DVD dati (DVD-R/+R) con DivX, MP4, MP3… | Da solo, all'inserimento: si apre l'elenco dei file del disco | Come i file |
| Video CD 1.0/1.1/2.0 | Da solo, all'inserimento | Menu PBC, capitoli, canale audio |
| File video DivX/Xvid/MP4/MKV/MPEG/VOB | Da **Scripts → mediacenter**: SD, USB o rete | Tracce audio, capitoli o salti di 60 s, avanti/indietro veloce |
| Musica MP1/MP2/MP3, Ogg, FLAC, WAV, AAC | Da **Scripts → mediacenter** | La cartella si ascolta come un album |
| Immagini DVD `.iso` e cartelle `VIDEO_TS` | Da **Scripts → mediacenter** | Come un DVD |
| CD audio | Da solo, con il core scelto in `AUDIOCD=` oppure [MiSTer Hi-Fi](https://github.com/Anime0t4ku/MiSTer_Hi-Fi) | |
| Giochi PSX, Saturn, Mega CD, PC Engine CD… | Da solo, con i core di `_Physical Disc Cores` | Subcanale Q, LibCrypt, CD+G |

Limiti noti:

- Il video H.264 in alta definizione (720p e oltre) è troppo pesante per l'ARM. DivX/Xvid e MPEG-4 a definizione standard funzionano.
- I Super Video CD (MPEG-2) non sono decodificati.
- I CD-R con file DivX o MP3 non partono da soli: si aprono da **Scripts → mediacenter → Disco nel lettore**.
- L'uscita VGA a 15 kHz non è stata ancora provata.

## Prove fatte su MiSTer reale

| Prova | Risultato |
| --- | --- |
| Video CD | Parte all'inserimento del disco |
| DVD PAL originale protetto (Star Wars Episodio III) | Fluido. Menu, tracce audio e sottotitoli funzionano. 11 immagini saltate su 19286 (0,06%), nessuna riga nelle scene veloci |
| DVD-R dati con file DivX | Si apre da solo l'elenco dei file; i video vanno a schermo intero, senza difetti audio o video |
| Uscita | HDMI 1280x720 |

## Installazione

### 1. Scarica i file

Dalla pagina [Releases](../../releases) scarica `MiSTer_MediaCenter.zip`. Le cartelle dello zip sono già quelle della SD: estrailo nella cartella principale della SD (`/media/fat`). In alternativa, dalla pagina [Actions](../../actions) apri l'ultima build verde e scarica gli artifact `MiSTer_MediaCenter` e `mediacenter`.

### 2. Controlla che i file siano al posto giusto

| File | Dove va sulla SD |
| --- | --- |
| `MiSTer_MediaCenter` | `/media/fat/MiSTer_MediaCenter` |
| `mediacenter` | `/media/fat/linux/mediacenter` |
| `mediacenter.sh` | `/media/fat/Scripts/mediacenter.sh` |

Il Main standard (`/media/fat/MiSTer`) resta com'è e non viene toccato.

Per i DVD protetti serve anche `libdvdcss.so.2` in `/media/fat/dvdcss/`. È lo stesso percorso di [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) e del suo script `install_dvdcss`. Per motivi di licenza non è inclusa. Senza questo file si leggono solo i DVD non protetti.

### 3. Modifica `/media/fat/MiSTer.ini`

Aggiungi, o correggi se esistono già, queste sezioni:

```ini
[MiSTer]
PHYSICAL_DISC_BOOT=1

[menu]
main=MiSTer_MediaCenter

[A0CD-*]
main=MiSTer_MediaCenter
```

- `[menu]` fa partire il nuovo Main nel menu del MiSTer, dove avviene il riconoscimento dei dischi.
- `[A0CD-*]` usa lo stesso Main anche con i core di `_Physical Disc Cores` (PSX, Saturn, Mega CD…). Se nel file c'è ancora `main=MiSTer_Physical-CD` o un altro Main di Physical Disc, sostituiscilo con `MiSTer_MediaCenter`, altrimenti quei core ripartono con il vecchio Main.
- `PHYSICAL_DISC_BOOT=1` va nella sezione `[MiSTer]` ed è facoltativo: fa partire anche il disco già inserito all'accensione.

Altre voci facoltative:

| Voce | Valori | Effetto |
| --- | --- | --- |
| `DVD=` | `MEDIACENTER`, `HYBRID`, `FPGA` | Cosa apre i DVD-Video. Senza voce usa `mediacenter`; se non è installato, apre il core [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) |
| `AUDIOCD=` | `PSX`, `A0CD-Saturn`, `A0CD-MegaCD`, `MISTERHIFI`, … | Programma o core per i CD audio |
| `PHYSICAL_DISC_MGL=` | `0`, `1` | `0` spegne il riconoscimento automatico dei dischi |
| `MEGACD_SUBCODE_LOG=` | `0`, `1` | Nella sezione del Mega CD: scrive un log del subcanale CD+G |
| `MEGACD_CDDA_PACED=` | `0`, `1` | Nella sezione del Mega CD: da provare se la grafica CD+G ha difetti |

Dopo le modifiche riavvia il MiSTer.

## Comandi

Sono quelli del controller nel menu del MiSTer.

| Tasto | Elenco file | Riproduzione |
| --- | --- | --- |
| A | Apri | DVD: cambia sottotitoli |
| B | Indietro | Stop |
| Start | | Play |
| Select | | Pausa |
| ◄ ► | Pagina | Capitolo precedente/successivo (senza capitoli ±60 s; nell'album, brano) |
| ▲ ▼ | Su/giù | Traccia audio, oppure canale (stereo, sinistro, destro) |
| L / R | | Indietro / avanti veloce |
| Y | | DVD e Video CD: menu del disco |
| X | Esci | Esci (conferma con A) |

Nel menu di un DVD la croce sceglie il pulsante, **A** o **Start** lo conferma e **Y** torna al film. Nel menu di un Video CD ▲▼ scelgono la voce e **A** la avvia.

## Segnalare un problema

Apri una segnalazione nella pagina [Issues](../../issues) e allega:

1. cosa hai fatto e cosa è successo (disco o file, titolo, regione PAL/NTSC);
2. **foto o video dello schermo**, che aiutano molto a capire il problema;
3. il file `/tmp/mediacenter.log`, da copiare prima di spegnere il MiSTer, perché la cartella `/tmp` si svuota a ogni riavvio;
4. il tipo di uscita (HDMI, VGA, 15 kHz) e la risoluzione impostata in `MiSTer.ini`.

## Per chi sviluppa

### Variabili per i test

| Variabile | Effetto |
| --- | --- |
| `MC_ARMSCALE=1` | Ingrandisce l'immagine con l'ARM invece che con lo scaler dell'FPGA |
| `MC_NOVSYNC=1` | Disegna senza aspettare la sincronia verticale |
| `MC_DEBUG=1` | Scrive nel log gli eventi della navigazione DVD |

Per misurare la velocità di decodifica: `/media/fat/linux/mediacenter --bench <file>` oppure `--bench /dev/sr0`. Sopra il 100% del tempo reale il file è riproducibile.

Durante la riproduzione `mediacenter` porta il framebuffer di Linux alla dimensione dell'immagine con il comando `fb_cmd3`, che esiste solo in questo Main. Lo scaler lo allarga allo schermo con le proporzioni giuste. Con un altro Main l'immagine viene ingrandita dall'ARM.

### Compilazione

Il workflow `.github/workflows/build.yml` compila tutto a ogni push con GCC ARM 10.2 (`arm-none-linux-gnueabihf`). Un commit con `[release]` nel messaggio, oppure un tag `v*`, pubblica una release con lo zip, con la versione scritta in `VERSION`.

In locale, `make` compila il Main. `mediacenter` richiede FFmpeg 6.1.2, libdvdread 6.1.3 e libdvdnav 6.1.1 compilati in statico per l'ARM:

```sh
git clone --depth 1 -b n6.1.2 https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
tools/mediacenter/build_ffmpeg.sh ffmpeg-src ffmpeg-arm
curl -fsSL https://download.videolan.org/pub/videolan/libdvdread/6.1.3/libdvdread-6.1.3.tar.bz2 | tar -xj
curl -fsSL https://download.videolan.org/pub/videolan/libdvdnav/6.1.1/libdvdnav-6.1.1.tar.bz2 | tar -xj
tools/mediacenter/build_dvdnav.sh libdvdread-6.1.3 libdvdnav-6.1.1 dvdnav-arm
make mediacenter FFMPEG=$PWD/ffmpeg-arm DVDNAV=$PWD/dvdnav-arm
```

## Karaoke CD+G (Mega CD)

I dischi karaoke CD+G mostrano testi e immagini con il core Mega CD, attraverso il lettore CD del BIOS, come sulla console vera. Ci sono due modi per aprirli:

- apri l'MGL del Mega CD da **`_Physical Disc Cores`**;
- oppure metti `AUDIOCD=A0CD-MegaCD`, così il riconoscimento automatico apre i CD audio e CD+G con il core Mega CD.

Il lettore USB deve saper leggere il subcanale grezzo.

```ini
[A0CD-MegaCD]
main=MiSTer_MediaCenter
MEGACD_SUBCODE_LOG=1
```

## Copia dei dischi su CD-R

Copiare i giochi in CHD e riscriverli su CD-R, conservando il subcanale Q e la protezione LibCrypt della PSX, non è compito di MediaCenter. Se ne occupa il programma separato [MiSTer-Disc-Tools](https://github.com/bonna79/MiSTer-Disc-Tools), che si installa a parte e funziona accanto a questo Main.

## Provenienza e licenza

MiSTer MediaCenter deriva da [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) e dal supporto ai dischi fisici di Physical Disc di Anime0t4ku. Usa FFmpeg, libdvdread e libdvdnav. È distribuito sotto GPL-3.0, come i progetti da cui deriva: vedi [LICENSE](LICENSE) e [NOTICE](NOTICE).

---

## English

MiSTer MediaCenter turns the MiSTer FPGA (DE10-Nano) into a media player, with a single program and a single set of controls. It plays:

- Video CDs;
- DVD-Video, including copy-protected retail discs, with their menus;
- DivX/Xvid/MP4/MKV/MPEG video files;
- MP3/Ogg/FLAC music.

Sources are the USB optical drive, the SD card, USB sticks and network shares.

The ARM reads and decodes discs and files. The FPGA scaler enlarges the picture to fill the screen.

The package has two parts:

- **`MiSTer_MediaCenter`**: the Main (the MiSTer's main program). It includes every Physical Disc feature:
  - automatic start of PSX, Saturn, Mega CD, PC Engine CD and the other `_Physical Disc Cores` discs;
  - Q subchannel;
  - PSX LibCrypt read from the original disc;
  - CD+G on the Mega CD.

  On top of that, it recognizes Video CDs, DVD-Video and data DVDs and opens them with the player.
- **`mediacenter`**: the player, which runs on the ARM.

### What it plays

| Disc or file | How it opens | Features |
| --- | --- | --- |
| DVD-Video, CSS-protected too | By itself when inserted | Disc menus with buttons, chapters, audio tracks, subtitles, fast forward/rewind |
| Data DVD (DVD-R/+R) with DivX, MP4, MP3… | By itself when inserted: the disc's file list opens | Same as files |
| Video CD 1.0/1.1/2.0 | By itself when inserted | PBC menus, chapters, audio channel |
| DivX/Xvid/MP4/MKV/MPEG/VOB video files | From **Scripts → mediacenter**: SD, USB or network | Audio tracks, chapters or 60 s jumps, fast forward/rewind |
| MP1/MP2/MP3, Ogg, FLAC, WAV, AAC music | From **Scripts → mediacenter** | A folder plays as an album |
| DVD `.iso` images and `VIDEO_TS` folders | From **Scripts → mediacenter** | Same as a DVD |
| Audio CDs | By itself, with the core set in `AUDIOCD=` or [MiSTer Hi-Fi](https://github.com/Anime0t4ku/MiSTer_Hi-Fi) | |
| PSX, Saturn, Mega CD, PC Engine CD… games | By itself, with the `_Physical Disc Cores` | Q subchannel, LibCrypt, CD+G |

Known limits:

- High-definition H.264 (720p and above) is too heavy for the ARM. Standard-definition DivX/Xvid and MPEG-4 play fine.
- Super Video CDs (MPEG-2) are not decoded.
- CD-Rs with DivX or MP3 files do not start by themselves: open them from **Scripts → mediacenter → Disco nel lettore** (disc in the drive).
- The 15 kHz VGA output has not been tested yet.

The player's on-screen texts are in Italian.

### Tested on a real MiSTer

| Test | Result |
| --- | --- |
| Video CD | Starts when the disc is inserted |
| Original copy-protected PAL DVD (Star Wars Episode III) | Smooth. Menus, audio tracks and subtitles work. 11 pictures dropped out of 19286 (0.06%), no lines in fast scenes |
| Data DVD-R with DivX files | The file list opens by itself; videos play full screen with no audio or video defects |
| Output | HDMI 1280x720 |

### Installation

**1. Download the files.** From the [Releases](../../releases) page download `MiSTer_MediaCenter.zip`. Its folders already match the SD card: extract it into the root of the SD card (`/media/fat`). Alternatively, open the latest green build on the [Actions](../../actions) page and download the `MiSTer_MediaCenter` and `mediacenter` artifacts.

**2. Check that the files are in place.**

| File | Location on the SD card |
| --- | --- |
| `MiSTer_MediaCenter` | `/media/fat/MiSTer_MediaCenter` |
| `mediacenter` | `/media/fat/linux/mediacenter` |
| `mediacenter.sh` | `/media/fat/Scripts/mediacenter.sh` |

The standard Main (`/media/fat/MiSTer`) is left untouched.

Protected DVDs also need `libdvdcss.so.2` in `/media/fat/dvdcss/`. That is the same location used by [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) and its `install_dvdcss` script. It is not included for licensing reasons. Without it, only unprotected DVDs play.

**3. Edit `/media/fat/MiSTer.ini`.** Add these sections, or fix them if they are already there:

```ini
[MiSTer]
PHYSICAL_DISC_BOOT=1

[menu]
main=MiSTer_MediaCenter

[A0CD-*]
main=MiSTer_MediaCenter
```

- `[menu]` runs the new Main in the MiSTer menu, where discs are recognized.
- `[A0CD-*]` uses the same Main with the `_Physical Disc Cores` (PSX, Saturn, Mega CD…). If the file still says `main=MiSTer_Physical-CD`, or names another Physical Disc Main, replace it with `MiSTer_MediaCenter`. Otherwise those cores restart with the old Main.
- `PHYSICAL_DISC_BOOT=1` goes in the `[MiSTer]` section and is optional: it also starts a disc that is already in the drive at power-on.

Optional settings:

| Setting | Values | Effect |
| --- | --- | --- |
| `DVD=` | `MEDIACENTER`, `HYBRID`, `FPGA` | What opens DVD-Video. Without it, `mediacenter` is used; if it is not installed, the [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) core opens |
| `AUDIOCD=` | `PSX`, `A0CD-Saturn`, `A0CD-MegaCD`, `MISTERHIFI`, … | Program or core for audio CDs |
| `PHYSICAL_DISC_MGL=` | `0`, `1` | `0` turns automatic disc recognition off |
| `MEGACD_SUBCODE_LOG=` | `0`, `1` | In the Mega CD section: writes a log of the CD+G subchannel |
| `MEGACD_CDDA_PACED=` | `0`, `1` | In the Mega CD section: try it if CD+G graphics show defects |

Restart the MiSTer after the changes.

### Controls

They are the controller buttons of the MiSTer menu.

| Button | File list | Playback |
| --- | --- | --- |
| A | Open | DVD: change subtitles |
| B | Back | Stop |
| Start | | Play |
| Select | | Pause |
| ◄ ► | Page | Previous/next chapter (±60 s without chapters; track in an album) |
| ▲ ▼ | Up/down | Audio track, or channel (stereo, left, right) |
| L / R | | Rewind / fast forward |
| Y | | DVD and Video CD: disc menu |
| X | Exit | Exit (confirm with A) |

In a DVD menu, the D-pad picks a button, **A** or **Start** confirms it and **Y** returns to the film. In a Video CD menu, ▲▼ choose the entry and **A** plays it.

### Reporting a problem

Open a report on the [Issues](../../issues) page and attach:

1. what you did and what happened (disc or file, title, PAL/NTSC region);
2. **photos or a video of the screen**, which help a lot to understand the problem;
3. the `/tmp/mediacenter.log` file. Copy it before switching the MiSTer off, because `/tmp` is emptied at every restart;
4. the video output (HDMI, VGA, 15 kHz) and the resolution set in `MiSTer.ini`.

### Copying discs to CD-R

Dumping games to CHD and writing them back to CD-R, keeping the Q subchannel and the PSX LibCrypt protection, is not MediaCenter's job. The separate [MiSTer-Disc-Tools](https://github.com/bonna79/MiSTer-Disc-Tools) program does it. It is installed on its own and works alongside this Main.

### For developers

| Variable | Effect |
| --- | --- |
| `MC_ARMSCALE=1` | Enlarges the picture on the ARM instead of the FPGA scaler |
| `MC_NOVSYNC=1` | Draws without waiting for vertical sync |
| `MC_DEBUG=1` | Logs DVD navigation events |

To measure decoding speed: `/media/fat/linux/mediacenter --bench <file>` or `--bench /dev/sr0`. Above 100% of real time the file can be played.

During playback, `mediacenter` sets the Linux framebuffer to the size of the picture with the `fb_cmd3` command, which only exists in this Main. The scaler then stretches it to the screen with the right aspect ratio. With another Main, the ARM enlarges the picture.

The build steps and the CD+G notes are in the Italian sections above.

MiSTer MediaCenter is derived from [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) and from Anime0t4ku's Physical Disc support. It uses FFmpeg, libdvdread and libdvdnav. It is released under GPL-3.0, like the projects it comes from: see [LICENSE](LICENSE) and [NOTICE](NOTICE).
