# MiSTer MediaCenter (MMC)

[Italiano](README.md) · **English**

> This project was developed with the help of artificial intelligence: the code and the documentation were written with Claude, Anthropic's AI, and tested by rgm on a real MiSTer.

## What it is

MiSTer MediaCenter (MMC) turns the MiSTer FPGA (DE10-Nano) into a media player for video and audio discs and files. One program, with the same controls, plays DVD-Video, Video CDs, Super Video CDs, DivX/Xvid/MP4 video and music.

It is made for **standard-definition** formats, at DVD resolution or lower, and for two kinds of screen:

- **LCD TVs**, through HDMI, at the TV's resolution;
- **above all CRT TVs**, through the analog VGA output at **15 kHz**: the video reaches the CRT as its original signal, 480i/576i for DVDs and 240p/288p for the menu, with no conversion.

Both outputs work at the same time: the same video goes to the LCD TV over HDMI and to the CRT over VGA.

The MiSTer's ARM processor reads and decodes discs and files. The FPGA handles the video output: it scales the picture for HDMI and generates the 15 kHz signal for the CRT.

It reads from the USB optical drive, the SD card, USB sticks and network shares.

The package has three parts:

| File | What it does |
| --- | --- |
| `MiSTer_MediaCenter` | The Main, the MiSTer's main program, used in the menu. When you insert a disc it recognizes it: video discs and data DVDs open with the player, games start with the right core |
| `mediacenter` | The player itself, which runs on the ARM |
| `menu_mediacenter.rbf` | MMC's menu core: the normal MiSTer menu plus the 15 kHz output that brings the video to the CRT |

The player's on-screen texts are in Italian.

## What's new in version 2.0

- **Colours converted by the FPGA:** the menu core converts colours from YUV to RGB, work the ARM used to do. 720p H.264 MP4 video now plays smoothly: frames not shown drop from 10% to about 2%.
- **Audio and video in sync:** the picture waits for the sound when needed, slowing down slightly instead of stuttering. Changing audio track during a film (for example from 5.1 to a stereo commentary) is seamless.
- **Super Video CD:** discs with PBC menus, and now also `.bin` + `.cue` images copied to the SD card.
- **15 kHz VGA output for CRTs** from the menu core, alongside HDMI.
- **Living alongside the Physical Disc Main:** game discs are now handled by the Physical Disc Main inside the console cores; MMC takes care of video discs and files.
- **Settings file** `/media/fat/linux/mediacenter.ini` to adjust the player without editing scripts.
- Videos wider than the screen (for example 1440x810) are now shown whole, narrowed to the screen width.

## Living alongside MiSTer Physical Disc

MMC works together with Anime0t4ku's [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) Main (`MiSTer_Physical-CD`), installed separately. Each does its own part:

| Who | Where it works | What it does |
| --- | --- | --- |
| **MiSTer MediaCenter** | In the MiSTer menu | Recognizes the inserted disc. DVD-Video, Video CDs, Super Video CDs and data DVDs open with the player; games start with the `_Physical Disc Cores` core |
| **MiSTer Physical Disc** | Inside the console cores | Reads the game disc: Q subchannel, PSX LibCrypt protection, CD+G karaoke on the Mega CD, CHD images |

The MiSTer switches between the two Mains by itself, following the `main=` lines in `MiSTer.ini`.

### Setup to use them together

Both Mains go in the root of the SD card:

| File | Where from |
| --- | --- |
| `/media/fat/MiSTer_MediaCenter` | This project ([Releases](../../releases)) |
| `/media/fat/MiSTer_Physical-CD` | [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) |

Then add these lines to `/media/fat/MiSTer.ini` (if the sections already exist, only fix the `main=` line):

```ini
; MiSTer menu: MiSTer MediaCenter recognizes discs
; and opens DVDs, Video CDs, Super Video CDs and video files
[menu]
main=MiSTer_MediaCenter
PHYSICAL_DISC_BOOT=1

; Game disc cores (_Physical Disc Cores): the Physical
; Disc Main reads the disc (PSX, Saturn, Mega CD...)
[A0CD-*]
main=MiSTer_Physical-CD
```

Restart the MiSTer. From then on:

1. MiSTer MediaCenter runs in the menu: insert a DVD or a Video CD and the player starts; insert a game and MMC starts the right core;
2. as soon as the game core starts, the MiSTer switches by itself to the Physical Disc Main, which reads the disc;
3. when you leave the game and go back to the menu, the MiSTer switches back to MiSTer MediaCenter by itself.

Note:

- each section takes **a single** `main=` line: if there are two only the last one counts, so do not put both Mains in the same section;
- if the Physical Disc Main file on your SD card has another name, write that after `main=`;
- without the `[A0CD-*]` section games start but the disc is not read;
- `PHYSICAL_DISC_BOOT=1` is optional: it starts the disc already in the drive at power-on;
- Physical Disc Main options (for example `MEGACD_SUBCODE_LOG`) go in the `[A0CD-*]` section.

The other settings are explained in [Installation](#3-edit-mediafatmisterini).

If you insert a DVD while a game is running, go back to the MiSTer menu: MMC recognizes it there and starts it.

## What it plays

| Disc or file | How it starts | Features |
| --- | --- | --- |
| DVD-Video, protected originals included (CSS) | Starts by itself when you insert the disc | Disc menus, chapters, audio tracks, subtitles, fast forward and rewind |
| Video CD 1.0, 1.1 and 2.0 | Starts by itself when you insert the disc | PBC menus, chapters, audio channel |
| Super Video CD | Starts by itself when you insert the disc | PBC menus, chapters, audio tracks, fast forward and rewind |
| Super Video CD `.bin` + `.cue` images | From **Scripts → mediacenter**, opening the `.cue` file | As the disc, with its menus |
| Data DVDs (DVD-R or DVD+R) with DivX, MP4, MP3… | The file list opens by itself when you insert the disc | As for files |
| DivX, Xvid, MP4, MKV, MPEG and VOB video | From **Scripts → mediacenter**, choosing SD, USB or network | Audio tracks, chapters (or 60-second jumps), fast forward and rewind |
| DVD `.iso` images and `VIDEO_TS` folders | From **Scripts → mediacenter** | As a DVD |
| MP1, MP2, MP3, Ogg, FLAC, WAV and AAC music | From **Scripts → mediacenter** | The tracks of a folder play one after the other, like an album |
| Audio CDs | Start by themselves, with the core set in `AUDIOCD=` or with [MiSTer Hi-Fi](https://github.com/Anime0t4ku/MiSTer_Hi-Fi) | |
| PSX, Saturn, Mega CD, PC Engine CD… games | Start by themselves, with the `_Physical Disc Cores` | Handled by the Physical Disc Main |

CD-Rs with DivX or MP3 files do not start by themselves: open them from **Scripts → mediacenter → Disco nel lettore**.

## Formats and recommended resolutions

MMC is made for standard-definition video. These are the recommended maximum resolutions:

| Format | Recommended maximum resolution | Result |
| --- | --- | --- |
| DVD-Video (MPEG-2) | 720x576 (PAL), 720x480 (NTSC) | Perfect |
| Video CD (MPEG-1) | 352x288 (PAL), 352x240 (NTSC) | Perfect |
| Super Video CD (MPEG-2) | 480x576 (PAL), 480x480 (NTSC) | Perfect |
| DivX, Xvid (MPEG-4) | 720x576 | Smooth |
| MP4, MKV in 8-bit H.264 | 1280x720 | Smooth |
| Music | Any | Perfect |

**Video above 720p, 10-bit H.264 (Hi10p) and HEVC/H.265 are not recommended.** They play, with smooth audio, but the MiSTer cannot show every frame and the picture stutters. Measured on a real MiSTer:

| Video | Frames not shown |
| --- | --- |
| DVD (MPEG-2 720x576) | less than 1% |
| 8-bit H.264 1280x720 | 1.4–2.5% (was 10% before the FPGA converted the colours) |
| 8-bit H.264 1440x810 | about 65% (measured with version 1.0) |
| 10-bit H.264 (Hi10p) 1280x720 | about 78% (measured with version 1.0) |

With a 720p H.264 video the ARM needs about 30 ms to decode a picture, out of 41.7 ms available at 24 pictures per second. That is why the colour conversion, which took another 16 ms, moved to the FPGA: the ARM now copies the picture in 5.5 ms.

The limit is in the hardware, on both sides:

- **ARM:** all decoding runs on the DE10-Nano's 800 MHz ARM Cortex-A9, which has no hardware video decoder. It is enough for DVDs and DivX, not for high-definition H.264.
- **FPGA:** there is no room for an H.264 decoder, and no free one suited to the MiSTer exists. So the FPGA handles the video output (colour conversion, scaling for HDMI and the 15 kHz signal), not decoding.

On a CRT, high definition brings nothing anyway: the picture is brought back to 480 or 576 lines.

To watch an HD video, convert it to DVD resolution, for example with HandBrake: H.264 encoder, High profile, 8-bit, or Xvid. Avoid 10-bit H.264 (Hi10p) and HEVC/H.265.

## Video output: HDMI and 15 kHz CRT

- **HDMI (LCD TV):** the FPGA scaler enlarges the video to the resolution set with `video_mode` in `MiSTer.ini`, with the right shape (4:3, 16:9, 2.35:1 with black bars).
- **15 kHz VGA (CRT TV):** the `menu_mediacenter.rbf` core sends the video to the CRT as its original signal. The menu comes out at 240p/288p; a DVD comes out interlaced at 480i (NTSC) or 576i (PAL), as from a DVD player; a 16:9 film is shown with black bars above and below (letterbox).

For a CRT, add to the `[menu]` section of `MiSTer.ini`:

```ini
[menu]
main=MiSTer_MediaCenter
vga_scaler=0
forced_scandoubler=0
```

Also add `composite_sync=1` if your cable or TV uses composite sync (for example with a SCART cable).

The 15 kHz output has not been tested on a real CRT yet. If DVD pictures shake, the field order may need flipping: report it with a photo.

## Tested on a real MiSTer

Tests run by rgm on a DE10-Nano MiSTer with a Pioneer BDR-TD05 USB drive, HDMI output at 1280x720, up to 15 October 2026.

| Test | Result |
| --- | --- |
| Protected original PAL DVD (Star Wars Episode III) | Smooth. Menus, chapters, audio tracks, subtitles, fast forward and rewind work |
| DVD with three audio tracks (English 5.1, Italian, stereo commentary) | 4289 pictures, 2 skipped. Sound in sync with the lips; changing track during the film without stutter |
| Video CDs | Perfect, they start by themselves when the disc is inserted |
| Super Video CD (`.bin` + `.cue` image from the SD card) | Menu and all three test entries work |
| Data DVD-R with DivX and MKV files | The file list opens by itself, full-screen video |
| 720p H.264 MP4 (anime, 23.976 pictures per second) | Smooth, 1.4–2.5% of pictures not shown, sound in sync, correct colours with the FPGA conversion |
| 1440x810 H.264 MP4 | Shown whole, narrowed to the screen width, but stutters |
| Living alongside the Physical Disc Main | DVDs and Video CDs with MMC; a PAL PSX game with LibCrypt (The Legend of Dragoon) starts by itself and works with the Physical Disc Main |

Still to test: Super Video CD read from a burned disc, 15 kHz VGA output on a real CRT.

## Installation

### 1. Download the files

From the [Releases](../../releases) page download `MiSTer_MediaCenter.zip` and extract it to the root of the SD card (`/media/fat`). The folders in the zip already match the SD card layout. The zip holds the Main, the player, the menu core and the library for protected DVDs. Always update them together, because the FPGA colour conversion needs the Main and the core of the same version. Restart the MiSTer after copying the Main or the core.

The latest version is on the [Actions](../../actions) page. Open the latest successful run (green tick) of **Build MiSTer MediaCenter** and download `MiSTer_MediaCenter` and `mediacenter_arm`. The menu core is in the **Build MediaCenter menu core** run, file `mediacenter_rbf`.

Also install the [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) Main separately if you want to play original game discs.

### 2. Check where the files are

| File | Location on the SD card |
| --- | --- |
| `MiSTer_MediaCenter` | `/media/fat/MiSTer_MediaCenter` |
| `mediacenter` | `/media/fat/linux/mediacenter` |
| `mediacenter.sh` | `/media/fat/Scripts/mediacenter.sh` |
| `menu_mediacenter.rbf` | `/media/fat/linux/menu_mediacenter.rbf` |
| `MiSTer_Physical-CD` (Physical Disc Main) | `/media/fat/MiSTer_Physical-CD` |

The original Main (`/media/fat/MiSTer`) is left untouched.

Protected DVDs need the `libdvdcss.so.2` library in `/media/fat/dvdcss/`. The zip already includes it: it is [VideoLAN's libdvdcss](https://www.videolan.org/developers/libdvdcss.html) (GPL licence) for ARM, the same one tested on the MiSTer with protected DVDs. This is the same location used by [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) and its `install_dvdcss` script. In some countries the law forbids circumventing DVD copy protection: check the rules where you live. Without this file only unprotected DVDs play.

### 3. Edit `/media/fat/MiSTer.ini`

Add these lines, or correct them if they are already there:

```ini
[menu]
main=MiSTer_MediaCenter
PHYSICAL_DISC_BOOT=1

[A0CD-*]
main=MiSTer_Physical-CD
```

- **`[menu]`** uses MiSTer MediaCenter in the MiSTer menu, which is where discs are recognized.
- **`[A0CD-*]`** uses the Physical Disc Main inside the `_Physical Disc Cores` (PSX, Saturn, Mega CD…). Without this line games start but the disc is not read, and a notice appears on screen. If the Physical Disc Main file on your SD card has another name, use that.
- **Each section takes a single `main=` line:** if there are two, only the last one counts. Do not put both Mains in the same section.
- **`PHYSICAL_DISC_BOOT=1`** is optional: at power-on it starts the disc already in the drive, like a console switched on with a disc inside.
- Physical Disc Main options, such as `MEGACD_SUBCODE_LOG` and `MEGACD_CDDA_PACED`, go in the `[A0CD-*]` section.
- For a CRT, add the lines from [Video output](#video-output-hdmi-and-15-khz-crt) to `[menu]`.

Other optional settings, in the `[menu]` section:

| Setting | Values | Effect |
| --- | --- | --- |
| `DVD=` | `MEDIACENTER`, `HYBRID`, `FPGA` | Chooses what opens DVD-Video. When missing, `mediacenter` is used; if `mediacenter` is not installed, the [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) core opens |
| `AUDIOCD=` | `PSX`, `A0CD-Saturn`, `A0CD-MegaCD`, `MISTERHIFI`, … | Chooses the program or core for audio CDs. With `A0CD-MegaCD`, CD+G karaoke discs show on the Mega CD |
| `PHYSICAL_DISC_MGL=` | `0`, `1` | `0` turns automatic disc detection off |

Restart the MiSTer after the changes.

### 4. Player settings (optional)

The player reads its settings from the text file `/media/fat/linux/mediacenter.ini`, one per line. The file is not needed: create it only if you want to change something.

```ini
# move the sound 50 milliseconds later
MC_AV_OFFSET=0.05
```

| Setting | Effect |
| --- | --- |
| `MC_AV_OFFSET=0.05` | Moves the sound later (positive value) or earlier (negative), in seconds. Useful with TVs that delay the picture |
| `MC_NOYUV=1` | Has the ARM convert the colours instead of the FPGA |

The same settings can also be given as environment variables, like the test variables below.

## Controls

Use the controller, with the MiSTer menu buttons.

| Button | In the file list | During playback |
| --- | --- | --- |
| A | Opens | DVD: changes subtitles |
| B | Goes back | Stop; on Super Video CDs goes back to the first menu |
| Start | | Play |
| Select | | Pause |
| ◄ ► | Previous/next page | Previous/next chapter; without chapters jumps 60 seconds; in music changes track |
| ▲ ▼ | Up and down | Changes audio track, or the channel (stereo, left, right) |
| L / R | | Rewind / fast forward |
| Y | | DVD, Video CD and Super Video CD: disc menu |
| X | Exits | Exits (confirm with A) |

In a DVD menu the D-pad picks the button, **A** or **Start** confirms it and **Y** returns to the film. In a Video CD or Super Video CD menu ▲▼ pick the entry, **A** starts it and **Start** opens the default one.

## Copying discs to CD-R

Dumping games to CHD files and writing them back to CD-R, keeping the Q subchannel and the PSX LibCrypt protection, is not MMC's job. [MiSTer-Disc-Tools](https://github.com/bonna79/MiSTer-Disc-Tools) does it: it is a separate program, installed on its own. CHD images with subchannel are played with the Physical Disc Main.

## Reporting a problem

Open a report on the [Issues](../../issues) page and attach:

1. what you did and what happened: disc or file, title, region (PAL or NTSC);
2. **photos or a video of the screen**, which help a lot;
3. the `/tmp/mediacenter.log` file, copied before switching the MiSTer off, because `/tmp` is cleared at every restart;
4. the video output (HDMI or 15 kHz VGA) and the `MiSTer.ini` lines you changed.

## For developers

### Test variables

| Variable | Effect |
| --- | --- |
| `MC_ARMSCALE=1` | Scales the picture on the ARM instead of the FPGA scaler |
| `MC_NOVSYNC=1` | Draws without waiting for vertical sync |
| `MC_SLOWDRAW=1` | Draws pictures the old way, line by line |
| `MC_DEBUG=1` | Logs DVD navigation events |
| `MC_FULLQUALITY=1` | Turns off the light mode for high-definition H.264/HEVC video (no deblocking, half of the lines enlarged by the FPGA scaler and, above 30 fps, only reference pictures) |
| `MC_TEST=...` | Runs without screen or controller: simulated keys, pictures saved to `/tmp/mc_<seconds>.ppm`. With a `.cue` file it plays a Super Video CD image as if it were in the drive |

To measure decoding speed use `/media/fat/linux/mediacenter --bench <file>` or `--bench /dev/sr0`. It analyzes at most the first minute of the file (Ctrl+C stops it earlier). If the result is above 100% of real time, the file can be played.

During playback `mediacenter` sets the Linux framebuffer to the picture size with the `fb_cmd3` command, which only exists in this Main. The scaler then enlarges it to the screen with the right shape. Pictures wider than the screen are narrowed on the ARM, because the scaler can only enlarge. With another Main the picture is scaled on the ARM.

### Building

The `.github/workflows/build.yml` workflow builds the Main and the player on every push with ARM GCC 10.2 (`arm-none-linux-gnueabihf`). A commit with `[release]` in its message, or a `v*` tag, publishes a release with the zip; the version number is in the `VERSION` file. The `.github/workflows/fpga.yml` workflow builds the menu core (`fpga/menu`) with Quartus 17.0.

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

MiSTer MediaCenter derives from [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer), the MiSTer menu core and Anime0t4ku's Physical Disc support for physical discs. It uses FFmpeg, libdvdread and libdvdnav. It is distributed under the GPL-3.0 license, like the projects it derives from: see [LICENSE](LICENSE) and [NOTICE](NOTICE).
