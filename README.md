# MiSTer MediaCenter

MiSTer MediaCenter è un lettore multimediale per MiSTer FPGA (DE10-Nano). Usa l'ARM e l'FPGA insieme: l'ARM legge dischi e file, l'FPGA presenta audio e video.

Questo repository è il Main (il programma ARM) del lettore. Si attiva al posto del Main standard tramite `MiSTer.ini` e non modifica l'installazione esistente.

## Stato

Prima versione, non ancora provata su hardware.

| Funzione | Stato | Come funziona |
| --- | --- | --- |
| DVD-Video, anche commerciali | Sì | Riconosce il disco e apre il core DVD ([MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD)); i DVD protetti sono letti dal suo Main `MiSTer_DVDcss` con libdvdcss |
| Video CD 1.0/1.1/2.0 e Super Video CD | Sì | Riconosce il disco e apre il core FPGA DVD |
| CD audio | Sì | Apre il core scelto con `AUDIOCD=` oppure [MiSTer Hi-Fi](https://github.com/Anime0t4ku/MiSTer_Hi-Fi) |
| File DivX/Xvid/MKV e audio compresso da SD e USB | Pianificato | Decodifica ARM con FFmpeg, uscita video FPGA |

## Installazione

1. Scarica `MiSTer_MediaCenter` dall'ultima build nella pagina Actions o Releases.
2. Copialo in `/media/fat/` sulla SD.
3. Aggiungi a `/media/fat/MiSTer.ini`:

```ini
[menu]
main=MiSTer_MediaCenter
```

Per i DVD commerciali e i Video CD installa il core [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) e segui la sua guida: aggiunge la sezione `[DVD]` con `main=MiSTer_DVDcss` e lo script `install_dvdcss`.

## Impostazioni in `MiSTer.ini`

| Voce | Valori | Effetto |
| --- | --- | --- |
| `DVD=` | `HYBRID`, `FPGA` | Core da aprire per i DVD-Video; senza voce sceglie da solo |
| `AUDIOCD=` | `PSX`, `A0CD-Saturn`, `MISTERHIFI`, … | Programma da aprire per i CD audio |

I Video CD aprono sempre il core FPGA DVD.

## Compilazione

Il workflow `.github/workflows/build.yml` compila il Main a ogni push con GCC ARM 10.2 (`arm-none-linux-gnueabihf`) e pubblica il binario come artifact. In locale: `make` con quel compilatore nel `PATH`.

## Provenienza e licenza

MiSTer MediaCenter deriva da [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) e dal supporto ai dischi fisici di Physical Disc. È distribuito sotto GPL-3.0, come i progetti da cui deriva: vedi [LICENSE](LICENSE) e [NOTICE](NOTICE).
