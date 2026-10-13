# MiSTer MediaCenter (MMC)

**Italiano** · [English](README_EN.md)

> Questo progetto è stato sviluppato con l'aiuto dell'intelligenza artificiale: il codice e la documentazione sono stati scritti con Claude, l'IA di Anthropic, e provati da rgm su un MiSTer vero.

## Cos'è

MiSTer MediaCenter (MMC) trasforma il MiSTer FPGA (DE10-Nano) in un lettore multimediale per dischi e file video e audio. Un solo programma, con gli stessi comandi, riproduce DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4 e musica.

È pensato per i formati a **risoluzione standard**, quella di un DVD o più bassa, e per due tipi di schermo:

- **televisori LCD**, con il collegamento HDMI, alla risoluzione del televisore;
- **soprattutto televisori CRT**, con l'uscita analogica VGA a **15 kHz**: il video arriva al CRT con il suo segnale originale, 480i/576i per i DVD e 240p/288p per il menu, senza conversioni.

Le due uscite funzionano insieme: lo stesso video va al televisore LCD in HDMI e al CRT in VGA.

Il processore ARM del MiSTer legge e decodifica dischi e file. L'FPGA si occupa dell'uscita video: ingrandisce l'immagine per l'HDMI e genera il segnale a 15 kHz per il CRT.

Legge dal lettore ottico USB, dalla scheda SD, dalle chiavette USB e dalle cartelle di rete.

Il pacchetto è formato da tre parti:

| File | Cosa fa |
| --- | --- |
| `MiSTer_MediaCenter` | Il Main, il programma principale del MiSTer, usato nel menu. Quando inserisci un disco lo riconosce: dischi video e DVD dati li apre con il lettore, i giochi li avvia con il core giusto |
| `mediacenter` | Il lettore vero e proprio, che gira sull'ARM |
| `menu_mediacenter.rbf` | Il core del menu di MMC: il menu normale del MiSTer più l'uscita a 15 kHz che porta il video al CRT |

## Convivenza con MiSTer Physical Disc

MMC funziona insieme al Main di [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) di Anime0t4ku (`MiSTer_Physical-CD`), da installare a parte. Ognuno fa la sua parte:

| Chi | Dove lavora | Cosa fa |
| --- | --- | --- |
| **MiSTer MediaCenter** | Nel menu del MiSTer | Riconosce il disco inserito. DVD-Video, Video CD, Super Video CD e DVD dati li apre con il lettore; i giochi li avvia con il core di `_Physical Disc Cores` |
| **MiSTer Physical Disc** | Dentro i core delle console | Legge il disco del gioco: subcanale Q, protezione LibCrypt della PSX, CD+G per il karaoke sul Mega CD, immagini CHD |

Il MiSTer passa da un Main all'altro da solo, secondo le righe `main=` di `MiSTer.ini`. Per questo è importante configurare bene `MiSTer.ini` (vedi [Installazione](#3-modifica-mediafatmisterini)).

Se inserisci un DVD mentre sei in un gioco, esci al menu del MiSTer: lì MMC lo riconosce e lo fa partire.

## Cosa riproduce

| Disco o file | Come si avvia | Funzioni |
| --- | --- | --- |
| DVD-Video, anche originali protetti (CSS) | Parte da solo quando inserisci il disco | Menu del disco, capitoli, tracce audio, sottotitoli, avanti e indietro veloce |
| Video CD 1.0, 1.1 e 2.0 | Parte da solo quando inserisci il disco | Menu PBC, capitoli, canale audio |
| Super Video CD | Parte da solo quando inserisci il disco | Menu PBC, capitoli, tracce audio, avanti e indietro veloce |
| DVD dati (DVD-R o DVD+R) con DivX, MP4, MP3… | Quando inserisci il disco si apre da solo l'elenco dei file | Come per i file |
| Video DivX, Xvid, MP4, MKV, MPEG e VOB | Da **Scripts → mediacenter**, scegliendo SD, USB o rete | Tracce audio, capitoli (o salti di 60 secondi), avanti e indietro veloce |
| Immagini DVD `.iso` e cartelle `VIDEO_TS` | Da **Scripts → mediacenter** | Come un DVD |
| Musica MP1, MP2, MP3, Ogg, FLAC, WAV e AAC | Da **Scripts → mediacenter** | I brani di una cartella si ascoltano di seguito, come un album |
| CD audio | Parte da solo, con il core indicato in `AUDIOCD=` oppure con [MiSTer Hi-Fi](https://github.com/Anime0t4ku/MiSTer_Hi-Fi) | |
| Giochi PSX, Saturn, Mega CD, PC Engine CD… | Partono da soli, con i core di `_Physical Disc Cores` | Gestiti dal Main di Physical Disc |

I CD-R con file DivX o MP3 non partono da soli: si aprono da **Scripts → mediacenter → Disco nel lettore**.

## Formati e risoluzioni consigliate

MMC è fatto per i video a risoluzione standard. Queste sono le risoluzioni massime consigliate:

| Formato | Risoluzione massima consigliata | Risultato |
| --- | --- | --- |
| DVD-Video (MPEG-2) | 720x576 (PAL), 720x480 (NTSC) | Perfetto |
| Video CD (MPEG-1) | 352x288 (PAL), 352x240 (NTSC) | Perfetto |
| Super Video CD (MPEG-2) | 480x576 (PAL), 480x480 (NTSC) | Perfetto |
| DivX, Xvid (MPEG-4) | 720x576 | Fluido |
| MP4, MKV in H.264 a 8 bit | 720x576, al massimo 1024x576 | Fluido |
| Musica | Qualsiasi | Perfetta |

**I video MP4 e MKV in HD, a 720p e oltre, sono sconsigliati.** Si riproducono, con l'audio sempre fluido, ma il MiSTer non riesce a mostrare tutti i fotogrammi e l'immagine scatta. Misure fatte su un MiSTer vero:

| Video | Fotogrammi non mostrati |
| --- | --- |
| DVD (MPEG-2 720x576) | circa 1% |
| H.264 8 bit 1280x720 | circa 10% |
| H.264 8 bit 1440x810 | circa 65% |
| H.264 10 bit (Hi10p) 1280x720 | circa 78% |

Il limite è nell'hardware, da tutte e due le parti:

- **ARM:** tutta la decodifica avviene sul processore ARM Cortex-A9 a 800 MHz del DE10-Nano, che non ha un decodificatore video hardware. Basta per DVD e DivX, non per l'H.264 in alta definizione.
- **FPGA:** non ha spazio per un decodificatore H.264, e non ne esiste uno libero adatto al MiSTer. Per questo l'FPGA si occupa solo dell'uscita video (ingrandimento per l'HDMI e segnale a 15 kHz), non della decodifica.

Sul CRT, poi, l'alta definizione non serve: l'immagine viene comunque riportata a 480 o 576 righe.

Per guardare un video HD conviene convertirlo, per esempio con HandBrake, a risoluzione DVD: codifica H.264, profilo High, 8 bit, oppure Xvid. Non usare H.264 a 10 bit (Hi10p) né HEVC/H.265.

## Uscita video: HDMI e CRT a 15 kHz

- **HDMI (televisore LCD):** lo scaler dell'FPGA ingrandisce il video alla risoluzione impostata con `video_mode` in `MiSTer.ini`, con le proporzioni giuste (4:3, 16:9, 2.35:1 con le bande nere).
- **VGA a 15 kHz (televisore CRT):** il core `menu_mediacenter.rbf` manda il video al CRT con il suo segnale originale. Il menu esce a 240p/288p; un DVD esce interlacciato a 480i (NTSC) o 576i (PAL), come da un lettore DVD; un film in 16:9 viene mostrato con le bande nere sopra e sotto (letterbox).

Per il CRT aggiungi alla sezione `[menu]` di `MiSTer.ini`:

```ini
[menu]
main=MiSTer_MediaCenter
vga_scaler=0
forced_scandoubler=0
```

Aggiungi anche `composite_sync=1` se il tuo cavo o il tuo televisore usano il sincronismo composito (per esempio con un cavo SCART).

L'uscita a 15 kHz non è ancora stata provata su un CRT vero. Se l'immagine dei DVD trema, potrebbe servire invertire l'ordine dei campi: segnalalo con una foto.

## Prove su un MiSTer vero

| Prova | Risultato |
| --- | --- |
| DVD PAL originale protetto (Star Wars Episodio III) | Fluido, 1,4% di fotogrammi saltati. Menu, capitoli, tracce audio, sottotitoli, avanti e indietro veloce funzionanti |
| Video CD | Perfetti, partono da soli quando si inserisce il disco |
| DVD-R dati con file DivX e MKV | L'elenco dei file si apre da solo, video a tutto schermo |
| MP4 H.264 720p | Si vede, con il 10% di fotogrammi saltati |
| Convivenza con il Main di Physical Disc | DVD e Video CD con MMC; un gioco PSX PAL con LibCrypt (The Legend of Dragoon) parte da solo e funziona con il Main di Physical Disc |
| Uscita video usata | HDMI a 1280x720 |

Da provare: Super Video CD su disco vero, uscita VGA a 15 kHz su CRT.

## Installazione

### 1. Scarica i file

Dalla pagina [Releases](../../releases) scarica `MiSTer_MediaCenter.zip` ed estrailo nella cartella principale della SD (`/media/fat`). Le cartelle dello zip sono già organizzate come sulla SD.

La versione più recente si scarica dalla pagina [Actions](../../actions). Apri l'ultima compilazione riuscita (spunta verde) di **Build MiSTer MediaCenter** e scarica `MiSTer_MediaCenter` e `mediacenter_arm`. Il core del menu è nella compilazione **Build MediaCenter menu core**, file `mediacenter_rbf`.

Installa a parte anche il Main di [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases), se vuoi giocare con i dischi originali.

### 2. Controlla dove sono i file

| File | Posizione sulla SD |
| --- | --- |
| `MiSTer_MediaCenter` | `/media/fat/MiSTer_MediaCenter` |
| `mediacenter` | `/media/fat/linux/mediacenter` |
| `mediacenter.sh` | `/media/fat/Scripts/mediacenter.sh` |
| `menu_mediacenter.rbf` | `/media/fat/linux/menu_mediacenter.rbf` |
| `MiSTer_Physical-CD` (Main di Physical Disc) | `/media/fat/MiSTer_Physical-CD` |

Il Main originale (`/media/fat/MiSTer`) non viene toccato.

Per i DVD protetti serve anche il file `libdvdcss.so.2` nella cartella `/media/fat/dvdcss/`. È la stessa posizione usata da [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) e dal suo script `install_dvdcss`. Per motivi di licenza non è incluso. Senza questo file si leggono solo i DVD non protetti.

### 3. Modifica `/media/fat/MiSTer.ini`

Aggiungi queste righe, oppure correggile se ci sono già:

```ini
[menu]
main=MiSTer_MediaCenter
PHYSICAL_DISC_BOOT=1

[A0CD-*]
main=MiSTer_Physical-CD
```

- **`[menu]`** fa usare MiSTer MediaCenter nel menu del MiSTer, che è dove vengono riconosciuti i dischi.
- **`[A0CD-*]`** fa usare il Main di Physical Disc dentro i core di `_Physical Disc Cores` (PSX, Saturn, Mega CD…). Senza questa riga i giochi partono ma il disco non viene letto, e sullo schermo compare un avviso. Se il file del Main di Physical Disc nella SD ha un altro nome, scrivi quello.
- **Ogni sezione accetta una sola riga `main=`:** se ce ne sono due, vale solo l'ultima. Non mettere tutti e due i Main nella stessa sezione.
- **`PHYSICAL_DISC_BOOT=1`** è facoltativo: all'accensione fa partire il disco che è già nel lettore, come una console accesa con il disco dentro.
- Le opzioni del Main di Physical Disc, come `MEGACD_SUBCODE_LOG` e `MEGACD_CDDA_PACED`, vanno nella sezione `[A0CD-*]`.
- Per il CRT aggiungi a `[menu]` le righe di [Uscita video](#uscita-video-hdmi-e-crt-a-15-khz).

Altre impostazioni facoltative, nella sezione `[menu]`:

| Impostazione | Valori | Effetto |
| --- | --- | --- |
| `DVD=` | `MEDIACENTER`, `HYBRID`, `FPGA` | Sceglie cosa apre i DVD-Video. Se manca, usa `mediacenter`; se `mediacenter` non è installato, apre il core [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) |
| `AUDIOCD=` | `PSX`, `A0CD-Saturn`, `A0CD-MegaCD`, `MISTERHIFI`, … | Sceglie il programma o il core per i CD audio. Con `A0CD-MegaCD` i CD+G del karaoke si vedono sul Mega CD |
| `PHYSICAL_DISC_MGL=` | `0`, `1` | Con `0` il riconoscimento automatico dei dischi è spento |

Dopo le modifiche riavvia il MiSTer.

## Comandi

Si usa il controller, con i tasti del menu del MiSTer.

| Tasto | Nell'elenco dei file | Durante la riproduzione |
| --- | --- | --- |
| A | Apre | DVD: cambia i sottotitoli |
| B | Torna indietro | Stop; nei Super Video CD torna al primo menu |
| Start | | Play |
| Select | | Pausa |
| ◄ ► | Pagina precedente/successiva | Capitolo precedente/successivo; senza capitoli salta di 60 secondi; nella musica cambia brano |
| ▲ ▼ | Su e giù | Cambia traccia audio, oppure il canale (stereo, sinistro, destro) |
| L / R | | Indietro / avanti veloce |
| Y | | DVD, Video CD e Super Video CD: menu del disco |
| X | Esce | Esce (conferma con A) |

Nel menu di un DVD la croce sceglie il pulsante, **A** o **Start** lo conferma e **Y** torna al film. Nel menu di un Video CD o di un Super Video CD le frecce ▲▼ scelgono la voce, **A** la avvia e **Start** apre quella predefinita.

## Copia dei dischi su CD-R

Copiare i giochi in file CHD e riscriverli su CD-R, mantenendo il subcanale Q e la protezione LibCrypt della PSX, non è compito di MMC. Se ne occupa [MiSTer-Disc-Tools](https://github.com/bonna79/MiSTer-Disc-Tools), un programma separato che si installa a parte. I CHD con il subcanale si giocano con il Main di Physical Disc.

## Segnalare un problema

Apri una segnalazione nella pagina [Issues](../../issues) e allega:

1. cosa hai fatto e cosa è successo: disco o file, titolo, regione (PAL o NTSC);
2. **foto o video dello schermo**, che aiutano molto a capire il problema;
3. il file `/tmp/mediacenter.log`, da copiare prima di spegnere il MiSTer, perché la cartella `/tmp` si svuota a ogni riavvio;
4. il tipo di uscita video (HDMI o VGA a 15 kHz) e le righe di `MiSTer.ini` che hai cambiato.

## Per chi sviluppa

### Variabili per i test

| Variabile | Effetto |
| --- | --- |
| `MC_ARMSCALE=1` | Ingrandisce l'immagine con l'ARM invece che con lo scaler dell'FPGA |
| `MC_NOVSYNC=1` | Disegna senza aspettare la sincronia verticale |
| `MC_DEBUG=1` | Scrive nel log gli eventi della navigazione nei DVD |
| `MC_FULLQUALITY=1` | Disattiva la modalità leggera dei video H.264/HEVC in alta definizione (deblocking spento, metà delle righe ingrandite dallo scaler FPGA e, sopra i 30 fps, solo le immagini di riferimento) |
| `MC_TEST=...` | Prova senza schermo né controller: tasti simulati e immagini salvate in `/tmp/mc_<secondi>.ppm`. Con `cdimage:<file .bin>:<settore traccia 2>:...` al posto del file riproduce un'immagine di Super Video CD come se fosse nel lettore |

Per misurare la velocità di decodifica usa `/media/fat/linux/mediacenter --bench <file>` oppure `--bench /dev/sr0`. Analizza al massimo il primo minuto del file (Ctrl+C lo ferma prima). Se il risultato supera il 100% del tempo reale, il file si può riprodurre.

Durante la riproduzione `mediacenter` imposta il framebuffer di Linux alla dimensione dell'immagine con il comando `fb_cmd3`, che esiste solo in questo Main. Lo scaler poi lo allarga allo schermo con le proporzioni corrette. Le immagini più larghe dello schermo vengono ristrette dall'ARM, perché lo scaler sa solo ingrandire. Con un altro Main l'immagine viene ingrandita dall'ARM.

### Compilazione

Il workflow `.github/workflows/build.yml` compila il Main e il lettore a ogni push con GCC ARM 10.2 (`arm-none-linux-gnueabihf`). Un commit con `[release]` nel messaggio, oppure un tag `v*`, pubblica una release con lo zip; il numero di versione è nel file `VERSION`. Il workflow `.github/workflows/fpga.yml` compila il core del menu (`fpga/menu`) con Quartus 17.0.

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

MiSTer MediaCenter deriva da [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer), dal core del menu del MiSTer e dal supporto ai dischi fisici di Physical Disc di Anime0t4ku. Usa FFmpeg, libdvdread e libdvdnav. È distribuito con licenza GPL-3.0, come i progetti da cui deriva: vedi [LICENSE](LICENSE) e [NOTICE](NOTICE).
