Prima versione pubblica di MiSTer MediaCenter: un unico lettore per Video CD, DVD-Video (anche protetti, con i menu), DVD dati con DivX/MP4/MP3 e file da SD, USB e rete. Include tutte le funzioni di Physical Disc: PSX con subcanale Q e LibCrypt, Mega CD con CD+G, avvio automatico dei dischi.

**Installazione:** estrai `MiSTer_MediaCenter.zip` nella cartella principale della SD. Poi aggiungi a `MiSTer.ini`:

```ini
[menu]
main=MiSTer_MediaCenter

[A0CD-*]
main=MiSTer_MediaCenter
```

Per i DVD protetti copia `libdvdcss.so.2` in `/media/fat/dvdcss/`; per motivi di licenza non è inclusa. Tutti i dettagli sono nel [README](https://github.com/bonna79/MiSTer_MediaCenter#readme).

**Provato su MiSTer reale, uscita HDMI:**
- Video CD;
- DVD PAL protetto: 0,06% di immagini saltate;
- DVD-R con DivX.

**Non ancora provati:** uscita VGA a 15 kHz e CD-R con file.

Se trovi un problema, apri una segnalazione in [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues). Allega foto dello schermo e il file `/tmp/mediacenter.log`.

---

**English:** first public release of MiSTer MediaCenter. It is a single player for:
- Video CDs;
- DVD-Video, protected discs and menus included;
- data DVDs with DivX/MP4/MP3;
- files from SD, USB and network.

It includes every Physical Disc feature: PSX with Q subchannel and LibCrypt, Mega CD with CD+G, automatic disc start.

To install, extract `MiSTer_MediaCenter.zip` into the root of the SD card, then add `main=MiSTer_MediaCenter` to the `[menu]` and `[A0CD-*]` sections of `MiSTer.ini`. Protected DVDs need `libdvdcss.so.2` in `/media/fat/dvdcss/`, which is not included. The full guide is in the [README](https://github.com/bonna79/MiSTer_MediaCenter#english).

Tested on a real MiSTer over HDMI. Report problems on [Issues](https://github.com/bonna79/MiSTer_MediaCenter/issues), with photos of the screen and `/tmp/mediacenter.log`.
