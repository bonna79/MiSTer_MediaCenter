# MiSTer MediaCenter

**Italiano** · [English](README_EN.md)

> Questo progetto è stato sviluppato con l'aiuto dell'intelligenza artificiale: il codice e la documentazione sono stati scritti con Claude, l'IA di Anthropic, e provati da rgm su un MiSTer vero.

MiSTer MediaCenter trasforma il MiSTer FPGA (DE10-Nano) in un lettore multimediale. Un solo programma, con gli stessi comandi, riproduce:

- Video CD;
- DVD-Video, anche originali protetti, con i loro menu;
- video DivX, Xvid, MP4, MKV e MPEG;
- musica MP3, Ogg e FLAC.

Legge dal lettore ottico USB, dalla scheda SD, dalle chiavette USB e dalle cartelle di rete.

Il processore ARM del MiSTer legge e decodifica dischi e file. Lo scaler dell'FPGA ingrandisce l'immagine a tutto schermo.

Il pacchetto contiene due programmi:

- **`MiSTer_MediaCenter`** è il Main, il programma principale del MiSTer. Comprende tutte le funzioni di Physical Disc:
  - avvio automatico dei dischi PSX, Saturn, Mega CD, PC Engine CD e degli altri core di `_Physical Disc Cores`;
  - subcanale Q;
  - protezione LibCrypt della PSX letta dal disco originale;
  - CD+G sul Mega CD.

  In più riconosce Video CD, DVD-Video e DVD dati e li apre con il lettore.
- **`mediacenter`** è il lettore vero e proprio, che gira sull'ARM.

## Cosa riproduce

| Disco o file | Come si avvia | Funzioni |
| --- | --- | --- |
| DVD-Video, anche protetti (CSS) | Parte da solo quando inserisci il disco | Menu del disco, capitoli, tracce audio, sottotitoli, avanti e indietro veloce |
| DVD dati (DVD-R o DVD+R) con DivX, MP4, MP3… | Quando inserisci il disco si apre da solo l'elenco dei file | Come per i file |
| Video CD 1.0, 1.1 e 2.0 | Parte da solo quando inserisci il disco | Menu PBC, capitoli, canale audio |
| Video DivX, Xvid, MP4, MKV, MPEG e VOB | Da **Scripts → mediacenter**, scegliendo SD, USB o rete | Tracce audio, capitoli (o salti di 60 secondi), avanti e indietro veloce |
| Musica MP1, MP2, MP3, Ogg, FLAC, WAV e AAC | Da **Scripts → mediacenter** | I brani di una cartella si ascoltano di seguito, come un album |
| Immagini DVD `.iso` e cartelle `VIDEO_TS` | Da **Scripts → mediacenter** | Come un DVD |
| CD audio | Parte da solo, con il core indicato in `AUDIOCD=` oppure con [MiSTer Hi-Fi](https://github.com/Anime0t4ku/MiSTer_Hi-Fi) | |
| Giochi PSX, Saturn, Mega CD, PC Engine CD… | Parte da solo, con i core di `_Physical Disc Cores` | Subcanale Q, LibCrypt, CD+G |

Limiti noti:

- I video H.264 in alta definizione (720p e oltre) sono troppo pesanti per l'ARM. Vengono riprodotti in modalità leggera, con l'audio sempre fluido, ma il video può saltare dei fotogrammi. Per una riproduzione fluida conviene convertirli, per esempio con HandBrake, in H.264 a 720x576 o in Xvid. DivX, Xvid e MPEG-4 a risoluzione DVD funzionano bene.
- I Super Video CD (MPEG-2) non vengono riprodotti.
- I CD-R con file DivX o MP3 non partono da soli: si aprono da **Scripts → mediacenter → Disco nel lettore**.
- L'uscita VGA a 15 kHz non è ancora stata provata.

## Prove su un MiSTer vero

| Prova | Risultato |
| --- | --- |
| Video CD | Parte da solo quando si inserisce il disco |
| DVD PAL originale protetto (Star Wars Episodio III) | Riproduzione fluida, menu, tracce audio e sottotitoli funzionanti. Solo 11 fotogrammi saltati su 19286 (0,06%), nessuna riga nelle scene veloci |
| DVD-R dati con file DivX | L'elenco dei file si apre da solo e i video vanno a tutto schermo, senza difetti audio o video |
| Uscita video usata | HDMI a 1280x720 |

## Installazione

### 1. Scarica i file

Dalla pagina [Releases](../../releases) scarica `MiSTer_MediaCenter.zip` ed estrailo nella cartella principale della SD (`/media/fat`). Le cartelle dello zip sono già organizzate come sulla SD.

Chi vuole provare la versione più recente può scaricare i file dalla pagina [Actions](../../actions): apri l'ultima compilazione riuscita (spunta verde) e scarica `MiSTer_MediaCenter` e `mediacenter`.

### 2. Controlla dove sono i file

| File | Posizione sulla SD |
| --- | --- |
| `MiSTer_MediaCenter` | `/media/fat/MiSTer_MediaCenter` |
| `mediacenter` | `/media/fat/linux/mediacenter` |
| `mediacenter.sh` | `/media/fat/Scripts/mediacenter.sh` |

Il Main originale (`/media/fat/MiSTer`) non viene toccato.

Per i DVD protetti serve anche il file `libdvdcss.so.2` nella cartella `/media/fat/dvdcss/`. È la stessa posizione usata da [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) e dal suo script `install_dvdcss`. Per motivi di licenza non è incluso. Senza questo file si leggono solo i DVD non protetti.

### 3. Modifica `/media/fat/MiSTer.ini`

Aggiungi queste righe, oppure correggile se ci sono già:

```ini
[MiSTer]
PHYSICAL_DISC_BOOT=1

[menu]
main=MiSTer_MediaCenter

[A0CD-*]
main=MiSTer_MediaCenter
```

- **`[menu]`** fa usare il nuovo Main nel menu del MiSTer, che è dove vengono riconosciuti i dischi.
- **`[A0CD-*]`** fa usare lo stesso Main anche con i core di `_Physical Disc Cores` (PSX, Saturn, Mega CD…). Se nel file c'è ancora `main=MiSTer_Physical-CD`, o il nome di un altro Main di Physical Disc, sostituiscilo con `MiSTer_MediaCenter`. Altrimenti quei core ripartono con il vecchio Main.
- **`PHYSICAL_DISC_BOOT=1`** è facoltativo: fa partire anche il disco che è già nel lettore quando accendi il MiSTer.

Altre impostazioni facoltative:

| Impostazione | Valori | Effetto |
| --- | --- | --- |
| `DVD=` | `MEDIACENTER`, `HYBRID`, `FPGA` | Sceglie cosa apre i DVD-Video. Se manca, usa `mediacenter`; se `mediacenter` non è installato, apre il core [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) |
| `AUDIOCD=` | `PSX`, `A0CD-Saturn`, `A0CD-MegaCD`, `MISTERHIFI`, … | Sceglie il programma o il core per i CD audio |
| `PHYSICAL_DISC_MGL=` | `0`, `1` | Con `0` il riconoscimento automatico dei dischi è spento |

Dopo le modifiche riavvia il MiSTer.

## Comandi

Si usa il controller, con i tasti del menu del MiSTer.

| Tasto | Nell'elenco dei file | Durante la riproduzione |
| --- | --- | --- |
| A | Apre | DVD: cambia i sottotitoli |
| B | Torna indietro | Stop |
| Start | | Play |
| Select | | Pausa |
| ◄ ► | Pagina precedente/successiva | Capitolo precedente/successivo; senza capitoli salta di 60 secondi; nella musica cambia brano |
| ▲ ▼ | Su e giù | Cambia traccia audio, oppure il canale (stereo, sinistro, destro) |
| L / R | | Indietro / avanti veloce |
| Y | | DVD e Video CD: menu del disco |
| X | Esce | Esce (conferma con A) |

Nel menu di un DVD la croce sceglie il pulsante, **A** o **Start** lo conferma e **Y** torna al film. Nel menu di un Video CD le frecce ▲▼ scelgono la voce e **A** la avvia.

## Karaoke CD+G (Mega CD)

I dischi karaoke CD+G mostrano testi e immagini con il core Mega CD, attraverso il lettore CD del BIOS, come sulla console originale. Ci sono due modi per avviarli:

- apri il file MGL del Mega CD da **`_Physical Disc Cores`**;
- oppure metti `AUDIOCD=A0CD-MegaCD` in `MiSTer.ini`, così i CD audio e i CD+G si aprono da soli con il core Mega CD.

Il lettore USB deve saper leggere il subcanale grezzo.

Se la grafica non compare, aggiungi `MEGACD_SUBCODE_LOG=1` nella sezione del Mega CD: crea i file `/media/fat/logs/megacd_subcode.log` e `/media/fat/logs/megacd_subcode_raw.bin`, da allegare alla segnalazione. Se la grafica ha difetti, prova `MEGACD_CDDA_PACED=1` nella stessa sezione.

```ini
[A0CD-MegaCD]
main=MiSTer_MediaCenter
MEGACD_SUBCODE_LOG=1
```

## Copia dei dischi su CD-R

Copiare i giochi in file CHD e riscriverli su CD-R, mantenendo il subcanale Q e la protezione LibCrypt della PSX, non è compito di MediaCenter. Se ne occupa [MiSTer-Disc-Tools](https://github.com/bonna79/MiSTer-Disc-Tools), un programma separato che si installa a parte e funziona insieme a questo Main.

## Segnalare un problema

Apri una segnalazione nella pagina [Issues](../../issues) e allega:

1. cosa hai fatto e cosa è successo: disco o file, titolo, regione (PAL o NTSC);
2. **foto o video dello schermo**, che aiutano molto a capire il problema;
3. il file `/tmp/mediacenter.log`, da copiare prima di spegnere il MiSTer, perché la cartella `/tmp` si svuota a ogni riavvio;
4. il tipo di uscita video (HDMI, VGA, 15 kHz) e la risoluzione impostata in `MiSTer.ini`.

## Per chi sviluppa

### Variabili per i test

| Variabile | Effetto |
| --- | --- |
| `MC_ARMSCALE=1` | Ingrandisce l'immagine con l'ARM invece che con lo scaler dell'FPGA |
| `MC_NOVSYNC=1` | Disegna senza aspettare la sincronia verticale |
| `MC_DEBUG=1` | Scrive nel log gli eventi della navigazione nei DVD |
| `MC_FULLQUALITY=1` | Disattiva la modalità leggera dei video H.264/HEVC in alta definizione (deblocking spento e, sopra i 30 fps, solo le immagini di riferimento) |

Per misurare la velocità di decodifica usa `/media/fat/linux/mediacenter --bench <file>` oppure `--bench /dev/sr0`. Se il risultato supera il 100% del tempo reale, il file si può riprodurre.

Durante la riproduzione `mediacenter` imposta il framebuffer di Linux alla dimensione dell'immagine con il comando `fb_cmd3`, che esiste solo in questo Main. Lo scaler poi lo allarga allo schermo con le proporzioni corrette. Con un altro Main l'immagine viene ingrandita dall'ARM.

### Compilazione

Il workflow `.github/workflows/build.yml` compila tutto a ogni push con GCC ARM 10.2 (`arm-none-linux-gnueabihf`). Un commit con `[release]` nel messaggio, oppure un tag `v*`, pubblica una release con lo zip; il numero di versione è nel file `VERSION`.

In locale `make` compila il Main. `mediacenter` richiede FFmpeg 6.1.2, libdvdread 6.1.3 e libdvdnav 6.1.1, compilati in statico per l'ARM:

```sh
git clone --depth 1 -b n6.1.2 https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
tools/mediacenter/build_ffmpeg.sh ffmpeg-src ffmpeg-arm
curl -fsSL https://download.videolan.org/pub/videolan/libdvdread/6.1.3/libdvdread-6.1.3.tar.bz2 | tar -xj
curl -fsSL https://download.videolan.org/pub/videolan/libdvdnav/6.1.1/libdvdnav-6.1.1.tar.bz2 | tar -xj
tools/mediacenter/build_dvdnav.sh libdvdread-6.1.3 libdvdnav-6.1.1 dvdnav-arm
make mediacenter FFMPEG=$PWD/ffmpeg-arm DVDNAV=$PWD/dvdnav-arm
```

## Provenienza e licenza

MiSTer MediaCenter deriva da [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) e dal supporto ai dischi fisici di Physical Disc di Anime0t4ku. Usa FFmpeg, libdvdread e libdvdnav. È distribuito con licenza GPL-3.0, come i progetti da cui deriva: vedi [LICENSE](LICENSE) e [NOTICE](NOTICE).
