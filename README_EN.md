# MiSTer MediaCenter

[Italiano](README.md) · **English**

> This project was developed with the help of artificial intelligence: the code and the documentation were written with Claude, Anthropic's AI, and tested by rgm on a real MiSTer.

MiSTer MediaCenter turns the MiSTer FPGA (DE10-Nano) into a media player. A single program, with the same controls everywhere, plays:

- Video CDs;
- DVD-Video, copy-protected retail discs included, with their menus;
- DivX, Xvid, MP4, MKV and MPEG video;
- MP3, Ogg and FLAC music.

It reads from the USB optical drive, the SD card, USB sticks and network shares.

The MiSTer's ARM processor reads and decodes discs and files. The FPGA scaler enlarges the picture to full screen.

The package contains two programs:

- **`MiSTer_MediaCenter`** is the Main, the MiSTer's main program, used in the menu. When you insert a disc it recognizes it: Video CDs, DVD-Video and data DVDs open with the player, games start with the right `_Physical Disc Cores` core.
- **`mediacenter`** is the player itself, which runs on the ARM.

Inside the console cores, Anime0t4ku's [MiSTer Physical Disc](https://github.com/Anime0t4ku) Main (`MiSTer_Physical-CD`) does the work; install it separately. It reads the game disc and handles the Q subchannel, PSX LibCrypt, CD+G on the Mega CD and CHD images. The two Mains live side by side: the MiSTer switches between them by itself, following the `main=` lines in `MiSTer.ini`.

The player's on-screen texts are in Italian.

## What it plays

| Disc or file | How it starts | Features |
| --- | --- | --- |
| DVD-Video, CSS-protected too | Starts by itself when you insert the disc | Disc menus, chapters, audio tracks, subtitles, fast forward and rewind |
| Data DVD (DVD-R or DVD+R) with DivX, MP4, MP3… | When you insert the disc, its file list opens by itself | Same as files |
| Video CD 1.0, 1.1 and 2.0 | Starts by itself when you insert the disc | PBC menus, chapters, audio channel |
| Super Video CD (MPEG-2) | Starts by itself when you insert the disc | PBC menus, chapters, audio tracks, fast forward and rewind; without menus the tracks play one after the other |
| DivX, Xvid, MP4, MKV, MPEG and VOB video | From **Scripts → mediacenter**, choosing SD, USB or network | Audio tracks, chapters (or 60-second jumps), fast forward and rewind |
| MP1, MP2, MP3, Ogg, FLAC, WAV and AAC music | From **Scripts → mediacenter** | The tracks of a folder play one after the other, like an album |
| DVD `.iso` images and `VIDEO_TS` folders | From **Scripts → mediacenter** | Same as a DVD |
| Audio CDs | Start by themselves, with the core set in `AUDIOCD=` or with [MiSTer Hi-Fi](https://github.com/Anime0t4ku/MiSTer_Hi-Fi) | |
| PSX, Saturn, Mega CD, PC Engine CD… games | Start by themselves, with the `_Physical Disc Cores` | Handled by the Physical Disc Main (Q subchannel, LibCrypt, CD+G) |

Known limits:

- High-definition video (MKV and MP4) plays, with smooth audio, but the ARM cannot show every frame. Tests on a real MiSTer, with the figure the player writes to `/tmp/mediacenter.log`:

  | Video | Frames not shown |
  | --- | --- |
  | DVD (MPEG-2 720x576) | about 1% |
  | H.264 8-bit 1280x720 | about 10%, looks smooth |
  | H.264 8-bit 1440x810 | about 65%, stutters |
  | H.264 10-bit (Hi10p) 1280x720 | about 78%, stutters |

  The recommended maximum is **H.264 8-bit, 1280x720, 24-25 frames per second**. For fully smooth playback go down to 1024x576 or 960x540, or to DVD resolution in H.264 or Xvid. Avoid 10-bit H.264 (Hi10p) and HEVC/H.265. Convert with HandBrake: H.264 encoder, High profile, 8-bit.
- CD-Rs with DivX or MP3 files do not start by themselves: open them from **Scripts → mediacenter → Disco nel lettore** (disc in the drive).
- The 15 kHz VGA output has not been tested yet.

## Tested on a real MiSTer

| Test | Result |
| --- | --- |
| Video CD | Starts by itself when the disc is inserted |
| Original copy-protected PAL DVD (Star Wars Episode III) | Smooth playback; menus, audio tracks and subtitles work. Only 11 frames dropped out of 19286 (0.06%), no lines in fast scenes |
| Data DVD-R with DivX files | The file list opens by itself and videos play full screen, with no audio or video defects |
| Video output used | HDMI at 1280x720 |

## Installation

### 1. Download the files

From the [Releases](../../releases) page download `MiSTer_MediaCenter.zip` and extract it into the root folder of the SD card (`/media/fat`). The folders in the zip already match the SD card.

To try the latest version, download the files from the [Actions](../../actions) page: open the latest successful build (green tick) and download `MiSTer_MediaCenter` and `mediacenter`.

### 2. Check where the files are

| File | Location on the SD card |
| --- | --- |
| `MiSTer_MediaCenter` | `/media/fat/MiSTer_MediaCenter` |
| `mediacenter` | `/media/fat/linux/mediacenter` |
| `mediacenter.sh` | `/media/fat/Scripts/mediacenter.sh` |

The original Main (`/media/fat/MiSTer`) is left untouched.

Protected DVDs also need the file `libdvdcss.so.2` in the `/media/fat/dvdcss/` folder. It is the same location used by [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) and its `install_dvdcss` script. It is not included for licensing reasons. Without this file only unprotected DVDs can be played.

### 3. Edit `/media/fat/MiSTer.ini`

Add these lines, or correct them if they are already there:

```ini
[MiSTer]
PHYSICAL_DISC_BOOT=1

[menu]
main=MiSTer_MediaCenter

[A0CD-*]
main=MiSTer_Physical-CD
```

- **`[menu]`** uses MiSTer MediaCenter in the MiSTer menu, which is where discs are recognized.
- **`[A0CD-*]`** uses the Physical Disc Main inside the `_Physical Disc Cores` (PSX, Saturn, Mega CD…). Without this line games start but the disc is not read, and a notice appears on screen. If the Physical Disc Main file on your SD card has another name, use that.
- Each section takes a single `main=` line: if there are two, only the last one counts.
- **`PHYSICAL_DISC_BOOT=1`** is optional: it also starts the disc that is already in the drive when you switch the MiSTer on.

Other optional settings:

| Setting | Values | Effect |
| --- | --- | --- |
| `DVD=` | `MEDIACENTER`, `HYBRID`, `FPGA` | Chooses what opens DVD-Video. When missing, `mediacenter` is used; if `mediacenter` is not installed, the [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) core opens |
| `AUDIOCD=` | `PSX`, `A0CD-Saturn`, `A0CD-MegaCD`, `MISTERHIFI`, … | Chooses the program or core for audio CDs |
| `PHYSICAL_DISC_MGL=` | `0`, `1` | With `0`, automatic disc recognition is off |

Restart the MiSTer after the changes.

## Controls

Use the controller, with the MiSTer menu buttons.

| Button | In the file list | During playback |
| --- | --- | --- |
| A | Open | DVD: change subtitles |
| B | Go back | Stop |
| Start | | Play |
| Select | | Pause |
| ◄ ► | Previous/next page | Previous/next chapter; without chapters, jump 60 seconds; with music, change track |
| ▲ ▼ | Up and down | Change audio track, or channel (stereo, left, right) |
| L / R | | Rewind / fast forward |
| Y | | DVD and Video CD: disc menu |
| X | Exit | Exit (confirm with A) |

In a DVD menu the D-pad chooses the button, **A** or **Start** confirms it and **Y** returns to the film. In a Video CD menu the ▲▼ arrows choose the entry and **A** plays it.

## Copying discs to CD-R

Dumping games to CHD files and writing them back to CD-R, keeping the Q subchannel and the PSX LibCrypt protection, is not MediaCenter's job. [MiSTer-Disc-Tools](https://github.com/bonna79/MiSTer-Disc-Tools) does it: it is a separate program, installed on its own. CHD images with subchannel are played with the Physical Disc Main.

## Reporting a problem

Open a report on the [Issues](../../issues) page and attach:

1. what you did and what happened: disc or file, title, region (PAL or NTSC);
2. **photos or a video of the screen**, which help a lot to understand the problem;
3. the `/tmp/mediacenter.log` file. Copy it before switching the MiSTer off, because the `/tmp` folder is emptied at every restart;
4. the video output (HDMI, VGA, 15 kHz) and the resolution set in `MiSTer.ini`.

## For developers

### Test variables

| Variable | Effect |
| --- | --- |
| `MC_ARMSCALE=1` | Enlarges the picture on the ARM instead of the FPGA scaler |
| `MC_NOVSYNC=1` | Draws without waiting for vertical sync |
| `MC_DEBUG=1` | Writes DVD navigation events to the log |
| `MC_FULLQUALITY=1` | Turns off the light mode for high-definition H.264/HEVC video (no deblocking, half of the lines enlarged by the FPGA scaler and, above 30 fps, only reference pictures) |

To measure decoding speed use `/media/fat/linux/mediacenter --bench <file>` or `--bench /dev/sr0`. It analyses at most the first minute of the file (Ctrl+C stops it earlier). If the result is above 100% of real time, the file can be played.

During playback `mediacenter` sets the Linux framebuffer to the size of the picture with the `fb_cmd3` command, which only exists in this Main. The scaler then stretches it to the screen with the correct aspect ratio. With another Main, the ARM enlarges the picture.

### Building

The `.github/workflows/build.yml` workflow builds everything on every push with GCC ARM 10.2 (`arm-none-linux-gnueabihf`). A commit with `[release]` in its message, or a `v*` tag, publishes a release with the zip; the version number is in the `VERSION` file.

Locally, `make` builds the Main. `mediacenter` needs FFmpeg 6.1.2, libdvdread 6.1.3 and libdvdnav 6.1.1, built statically for the ARM:

```sh
git clone --depth 1 -b n6.1.2 https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
tools/mediacenter/build_ffmpeg.sh ffmpeg-src ffmpeg-arm
curl -fsSL https://download.videolan.org/pub/videolan/libdvdread/6.1.3/libdvdread-6.1.3.tar.bz2 | tar -xj
curl -fsSL https://download.videolan.org/pub/videolan/libdvdnav/6.1.1/libdvdnav-6.1.1.tar.bz2 | tar -xj
tools/mediacenter/build_dvdnav.sh libdvdread-6.1.3 libdvdnav-6.1.1 dvdnav-arm
make mediacenter FFMPEG=$PWD/ffmpeg-arm DVDNAV=$PWD/dvdnav-arm
```

## Origin and license

MiSTer MediaCenter is derived from [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) and from Anime0t4ku's Physical Disc support. It uses FFmpeg, libdvdread and libdvdnav. It is released under the GPL-3.0 license, like the projects it comes from: see [LICENSE](LICENSE) and [NOTICE](NOTICE).
