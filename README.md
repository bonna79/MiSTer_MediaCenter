# MiSTer MediaCenter

MiSTer MediaCenter è un lettore multimediale per MiSTer FPGA (DE10-Nano). Usa l'ARM e l'FPGA insieme: l'ARM legge dischi e file, l'FPGA presenta audio e video.

Questo repository è il Main (il programma ARM) del lettore. Si attiva al posto del Main standard tramite `MiSTer.ini` e non modifica l'installazione esistente.

## Stato

Il lettore Video CD deriva dal ramo `video-cd-boot-launch` di Main_MiSTer_Physical_Disc. L'obiettivo è farne un'unica applicazione che legga anche DVD (compresi i commerciali), file video DivX/Xvid/MP4/MKV e audio compresso, con gli stessi comandi.

| Funzione | Stato | Come funziona |
| --- | --- | --- |
| DVD-Video, anche commerciali | Sì | Riconosce il disco e apre il core DVD ([MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD)); i DVD protetti sono letti dal suo Main `MiSTer_DVDcss` con libdvdcss |
| Video CD 1.0/1.1/2.0 | Sì | Lettore integrato `vcd_player` sull'ARM: menu PBC, avanti/indietro, pausa, stop, capitoli, canale audio |
| Super Video CD | Parziale | Senza `vcd_player` o per gli SVCD (MPEG-2) si apre il core FPGA DVD |
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

## Lettore Video CD (in inglese, dal progetto originale)


Video CDs (VCD, SVCD and HQ-VCD, from any region: PAL, NTSC-U or NTSC-J) can be played through Auto Disc Detection.

Video CDs contain MPEG-1 video, which none of the console cores can decode (the PlayStation core does not include the MPEG board of the Asian SCPH-5903 model). They are therefore played by a separate application running on the MiSTer Linux side, `vcd_player`, which decodes the video in software and shows it on the MiSTer video output.

To enable it, copy `vcd_player` (built with `make vcd_player`, or taken from the build artifacts) to:

```
/media/fat/linux/vcd_player
```

Then, with Auto Disc Detection enabled, insert a Video CD while the MiSTer menu is showing. Playback starts automatically and plays every MPEG track of the disc in order. At the end of the disc, or when the OSD/Menu button is pressed, the player closes and the menu returns.

#### controls

Pressing any button of the gamepad shows the controls and a progress bar at the bottom of the screen. They disappear 10 seconds after the last button press. The buttons are those of the MiSTer menu mapping:

| Button | Action |
| --- | --- |
| Start | Play (also resumes normal speed after fast forward / rewind) |
| Select | Pause / resume |
| B | Stop (back to the beginning of the disc; Start plays again) |
| D-pad right / left | Next / previous chapter |
| R / L | Fast forward / rewind at 10x (press again, or Start, to play) |
| D-pad up / down | Audio channel: stereo, left only, right only (for discs with two languages on the two channels) |
| Y | Back to the disc menu (Video CD 2.0 discs with menus) |
| X, then A | Exit to the menu (any other button cancels) |

Chapters are the entry points stored on the disc (`ENTRIES.VCD`), or the MPEG tracks when the disc has none.

#### 2.0 menus (PBC)

Video CD 2.0 discs with playback control (`PSD.VCD`) start from the disc's own menu, which is drawn by the disc as a picture with numbered choices. The controls stay on screen while a menu is shown:

| Button | Action |
| --- | --- |
| D-pad up / down | Choose the menu entry (the number is shown at the bottom) |
| A | Play the chosen entry |
| Start | Play the menu's default entry |
| D-pad right / left | Next / previous page of the menu |
| Y | Back (the disc's "return") |
| B | Stop |
| X, then A | Exit to the menu |

Menu timeouts, waits and loops are followed as stored on the disc. Discs without menus, or with menus the player cannot read, play their tracks in order as before. Subtitles on Video CDs are part of the picture and are always shown; Super Video CDs (MPEG-2) are not decoded.

When `/media/fat/linux/vcd_player` is not present, nothing changes: Video CDs are handled exactly as before.

Audio is sent to the MiSTer audio output through `aplay`. If `aplay` is not available, video plays without sound.


## Impostazioni in `MiSTer.ini`

| Voce | Valori | Effetto |
| --- | --- | --- |
| `DVD=` | `HYBRID`, `FPGA` | Core da aprire per i DVD-Video; senza voce sceglie da solo |
| `PHYSICAL_DISC_BOOT=` | `0`, `1` | Avvia il disco già inserito all'accensione |
| `AUDIOCD=` | `PSX`, `A0CD-Saturn`, `MISTERHIFI`, … | Programma da aprire per i CD audio |

I Video CD usano `vcd_player` se è installato in `/media/fat/linux/vcd_player`, altrimenti il core FPGA DVD.

## Compilazione

Il workflow `.github/workflows/build.yml` compila il Main a ogni push con GCC ARM 10.2 (`arm-none-linux-gnueabihf`) e pubblica il binario come artifact. In locale: `make` con quel compilatore nel `PATH`.

## Provenienza e licenza

MiSTer MediaCenter deriva da [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) e dal supporto ai dischi fisici di Physical Disc. È distribuito sotto GPL-3.0, come i progetti da cui deriva: vedi [LICENSE](LICENSE) e [NOTICE](NOTICE).
