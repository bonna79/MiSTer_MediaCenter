# MiSTer MediaCenter

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
