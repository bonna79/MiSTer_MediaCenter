# Technical sheet: the H.264 accelerator in the FPGA of MiSTer MediaCenter

*Version 3.0, October 2026. In Italian: [SCHEDA_TECNICA_ACCELERATORE_IT.md](SCHEDA_TECNICA_ACCELERATORE_IT.md).*

This sheet is for programmers who want to understand, change or improve the accelerator, even starting again from scratch. It explains:
- how the accelerator splits the work between the MiSTer's ARM processor and its FPGA;
- which files are needed;
- how it is turned on;
- how the data flow;
- how to test it.

File names are relative to the root of the [bonna79/MiSTer_MediaCenter](https://github.com/bonna79/MiSTer_MediaCenter) repository.

---

## 1. Why it exists and what it does

The MiSTer (DE10-Nano) has a **dual-core ARM Cortex-A9 at 800 MHz** and a **Cyclone V FPGA**. MiSTer MediaCenter (MMC) is built like this:
- the `mediacenter` player runs on the ARM and decodes video with FFmpeg;
- the `menu_mediacenter.rbf` menu core runs in the FPGA and shows the pictures over HDMI and on the 15 kHz CRT.

A **1920x1080 H.264** video needs about **44 ms per picture** on the ARM, but at 23.976 pictures per second only **41.7 ms** are available: without help about 14% of the pictures were not shown. A complete H.264 decoder does not fit in the FPGA, and no suitable free one exists. So 3.0 **splits the work**:

| Who | What it does | Why there |
| --- | --- | --- |
| ARM (FFmpeg with our change) | Reads the file, parses the H.264 stream (CABAC/CAVLC), works out for every macroblock the type, intra modes, references, motion vectors and the already dequantised coefficients | It is the sequential part, full of bit-by-bit decisions: a processor does it well |
| FPGA (the engine in the menu core) | Rebuilds the pixels: 4x4/8x8 inverse transform, intra prediction, motion compensation with 6-tap filters and weights, residual add, writing the picture to memory | It is the part with many identical calculations in parallel: hardware does it well |
| FPGA (the zero-copy block) | Copies the finished picture into the frame buffer, narrowing it to 1280 or 960 pixels | So the ARM never touches the pixels again |

With this split the ARM needs on average **5–15 ms per picture**, and the FPGA about 20–33 ms in heavy scenes, in parallel with the ARM.

**Limits today:**
- only 8-bit, 4:2:0, progressive H.264, 1920 pixels wide;
- no deblocking filter: FFmpeg already skips it in the player's "light mode" for video above 576 lines;
- everything else (DVD, Video CD, DivX, 720p, interlaced or 10-bit H.264, HEVC) is decoded by the ARM as before.

---

## 2. The files

### ARM side (the `mediacenter` player)

| File | What it holds |
| --- | --- |
| `tools/mediacenter/ffmpeg-mediacenter.patch` | The change to FFmpeg 6.1.2. It adds `libavcodec/h264_mcacc.c` and hooks it into the H.264 decoder (`h264_slice.c`, `h264_picture.c`, `h264dec.h`). When the player turns it on, the decoder no longer rebuilds the pixels but writes the engine's commands. It also holds `MC_PARSEONLY`, used only for measurements. |
| `tools/mediacenter/build_ffmpeg.sh` | Builds FFmpeg for the MiSTer's ARM and applies the patch. |
| `tools/mediacenter/accel.c` | The hardware-independent ARM side: <ul><li>the "slots" (pictures in the FPGA's memory);</li><li>the dummy pixel buffer;</li><li>each picture's commands;</li><li>the sending order with two decoding threads;</li><li>sessions (seeks).</li></ul> |
| `tools/mediacenter/accel_link.c` | The link with the core: <ul><li>`/dev/mem`;</li><li>the mailbox;</li><li>the command ring and the cache handling;</li><li>waiting for finished pictures;</li><li>drawing through the zero-copy block;</li><li>the diagnostic lines.</li></ul> |
| `tools/mediacenter/zerocopy.c` | The zero-copy block, ARM side (mailbox at `0x23F00000`). The accelerator uses it to show its slots. |
| `tools/mediacenter/mediacenter.c` | The player. The accelerator is used in three places: <ul><li>`open_decoder()` turns it on with `acc_try()`;</li><li>`yuv_draw()` draws its pictures with `acc_zc_draw()`;</li><li>`acc_fallback()` and `acc_resume()` go back to the normal decoder and turn it on again.</li></ul> |

### FPGA side (the `menu_mediacenter.rbf` menu core)

| File | What it holds |
| --- | --- |
| `fpga/menu/menu.sv` | The menu core (the official MiSTer one plus MMC's additions). Connects `mc_acc`, `mc_zc`, `mc_bus` and `mc_out` to the DDR port. |
| `fpga/menu/rtl/mc_acc.sv` | The bridge between memory and engine. It does six jobs: <ul><li>reads the mailbox;</li><li>reads the page table;</li><li>reads the command ring;</li><li>queues the engine's memory requests;</li><li>writes the answer;</li><li>notices when the memory does not answer.</li></ul> |
| `accel/rtl/acc_engine.sv` | The reconstruction engine: reads the commands and writes the macroblocks. |
| `accel/rtl/acc_idct.sv` | 4x4 and 8x8 inverse transform. |
| `accel/rtl/acc_resid.sv` | Residual store of one macroblock (8 banks, double buffered). |
| `accel/rtl/acc_recon.sv` | Store of the rebuilt macroblock, from which it is written to DDR. |
| `accel/rtl/acc_mc.sv` | Inter prediction (P and B pictures): filters, weights, bi-prediction, reading the reference windows. |
| `accel/rtl/acc_ram.sv` | Dual-port block RAM that Quartus maps to M10K. |
| `fpga/menu/rtl/mc_zc.sv` | Zero-copy: reads Y, U and V through a page table, narrows 3:2 or 2:1 and writes YUYV lines into the frame buffer. |
| `fpga/menu/rtl/mc_bus.sv` | Shares the DDR port between `mc_out` (CRT output, always first), `mc_zc` and `mc_acc` (taking turns). |
| `fpga/menu/rtl/mc_out.sv` | The 15 kHz output for the CRT. Not part of the accelerator, but it goes first on the memory. |
| `fpga/menu/timing.tcl` | Checks timing at the 100 °C and -40 °C corners too. Quartus's multicorner analysis is off in `menu.qsf`. |

### Documentation, tests and builds

| File | What it holds |
| --- | --- |
| `accel/doc/comandi.md` | **The ARM → FPGA command format**: the contract between the two sides. |
| `accel/sim/model.py` | Python model of the engine: rebuilds the pictures from the commands and compares them with FFmpeg. |
| `accel/sim/make_tests.sh` | Makes the test videos, each aimed at a part of the engine. |
| `accel/sim/accdump.c`, `accel/sim/ffmpeg-accel-dump.patch` | Extract commands and expected pictures from a video with an FFmpeg built for the PC. |
| `accel/sim/acctest.c` | Tests the real ARM side on the PC (`accel.c`, and with `-DACC_FAKE` also `accel_link.c` against a pretend core). |
| `accel/sim/tb/` (`tb_engine.sv`, `run_engine.py`) | Engine test bench in Icarus Verilog, bit-exact comparison. |
| `accel/sim/core/` (`tb_core.sv`, `run_core.py`) | Test bench of `mc_acc` plus the engine, with the ring and a simulated DDR. |
| `accel/syn/` | Synthesis of the engine alone (size and speed). |
| `.github/workflows/fpga.yml` | Builds the menu core with Quartus 17.0.2 (docker image `theypsilon/quartus-lite-c5:17.0.2`). |
| `.github/workflows/accel.yml` | Synthesis of the engine alone. |
| `.github/workflows/build.yml` | Builds the Main, FFmpeg with the patch and the player; with `[release]` in the commit message it packages the release. |

---

## 3. The memory map

The ARM and the FPGA share the 1 GB DDR3. Linux uses the first 512 MB; the rest is for the cores. The core addresses DDR in 64-bit words: **core address = physical address / 8** (29 bits).

| Physical address | What is there | Written by | Read by |
| --- | --- | --- | --- |
| `0x00000000`–`0x1FFFFFFF` | Linux memory. Also holds the command ring (normal mode) and the decoder's pictures for zero-copy without the accelerator. | ARM | ARM, FPGA (by pages) |
| `0x22000000`–`0x237BB000` | The Main's frame buffer (3 pages for video) | `mc_zc`, ARM | HDMI scaler, `mc_out` |
| `0x23E00000` | **`mc_acc` mailbox**: words 0–4 from the player, 16–25 the core's answer. At `+0x1000` the ring page table (one word per 4 KB page). | ARM / `mc_acc` | `mc_acc` / ARM |
| `0x23F00000` | **`mc_zc` mailbox**: words 0–10 from the player, 16–18 answer. At `+0x1000` the page tables of the decoder's pictures (8 KB each). At `+0xC0000` the tables of the accelerator's 64 slots (4 KB each). | ARM / `mc_zc` | `mc_zc` / ARM |
| `0x2FC00000`–`0x2FFFFFFF` | Command ring in `MC_ACC_RING=so` mode (4 MB outside Linux) | ARM | `mc_acc` |
| `0x30000000`–`0x3FFFFFFF` | **64 slots of 4 MB**: the pictures rebuilt by the engine | engine | engine (references), `mc_zc` |

**A picture in a slot**, as byte values from the start of the slot:
- Y: rows of 2048 bytes (1920 pixels used), 1088 rows;
- U: from `2228224` (`278528 × 8`), rows of 1024 bytes;
- V: from `2785280` (`348160 × 8`), rows of 1024 bytes, 544 rows.

These are the `ACC_Y_STRIDE`, `ACC_C_STRIDE`, `ACC_U_OFF` and `ACC_V_OFF` constants in `accel_link.c` and the `YSTRIDE_L2`, `CSTRIDE_L2`, `UOFF` and `VOFF` parameters of `acc_engine.sv`: they must change together. The slot address is `0x30000000 + number × 4 MB`; the engine takes the number modulo 64.

Beware: while the frame buffer is off, the menu core clears DDR from `0x24000000` to `0x3FFFFFFF` (`ddram.sv`, `ddr_pause`). The accelerator only works with the frame buffer on.

---

## 4. How it turns on, step by step

1. **The Main loads our menu core.** `fpga_io.cpp` loads `/media/fat/linux/menu_mediacenter.rbf` in place of `menu.rbf`. If another Main (Physical Disc's, for example) brings back the official menu, `user_io.cpp` loads ours again, once per menu. The core is recognised by the `vMCY` version string in its `CONF_STR` (`menu_is_mediacenter()`).
2. **The player asks for a YUV frame buffer.** It writes `fb_cmd3 422 0 <width> <height> <aspect×1000> 1000` to `/dev/MiSTer_cmd`. For 1080p the frame buffer is 1280x540 (or 960x540 in the 4:3 format) in YUYV, with 3 pages. The Main sets up the scaler and sets the core's `status[5]` (FB) to 1. In the core, `fb_own & FB` enables `mc_zc` and `mc_acc` (`menu.sv`).
3. **The player opens the decoder.** In `open_decoder()` (`mediacenter.c`), H.264, HEVC and VP9 above 576 lines get the **light mode**:
   - no deblocking;
   - `AV_CODEC_FLAG2_FAST`;
   - for 1080p, half the lines (`half_lines`).

   Then `zcp_open()` prepares zero-copy and `acc_try()` tries the accelerator.
4. **`acc_try()` (`accel_link.c`) checks the conditions:**
   - H.264 codec, `yuv420p`, width 1920, `half_lines`;
   - a real frame buffer;
   - no earlier failure in this file (`acc_file_off`);
   - `MC_NOACC` not set.

   Then it:
   - opens the zero-copy mailbox (`zcp_map()`);
   - asks Linux not to move locked pages (`zcp_pin()`: `vm.compact_unevictable_allowed=0` while the file plays);
   - calls `al_open()`.
5. **`al_open()`:**
   - opens `/dev/mem` with `O_SYNC`;
   - maps the mailbox at `0x23E00000` and the L2 cache controller (PL310) registers at `0xFFFEF000`;
   - prepares the ring (chapter 6);
   - opens a **session**: writes mailbox words 2, 3, 4 and 0, drains the writes, then writes a new session number into word 1.

   The core answers with `ACOK` in word 16 and the same number in word 17, with 0 bytes read and 0 pictures. It is tried twice, within 0.3 s each. If the core does not answer (an old core or the official one), the accelerator stays off and the log says so.
6. **The player writes the page tables of the 64 slots** into the zero-copy mailbox (`+0xC0000`): `mc_zc` needs them to read the slots as if they were decoder pictures.
7. **`acc_attach()` (`accel.c`) hooks into FFmpeg:**
   - sets `mc_acc_hooks`;
   - `get_buffer2 = acc_get_buffer`;
   - `skip_loop_filter = AVDISCARD_ALL`.

   The log says `diag acceleratore: acceso (...)`.

From then on every decoded picture goes through the FPGA.

---

## 5. The ARM side: FFmpeg with our change

`h264_mcacc.c` is included by `h264_slice.c`. It hooks into the decoder in four places:

| When | Function | What it does |
| --- | --- | --- |
| Picture start (`h264_frame_start`) | `mcacc_begin()` | Checks that the picture is progressive, not MBAFF, 8-bit and 4:2:0 (otherwise `fail`). Asks the player for a buffer (`begin`), which already holds the `A1` command with the slot and the size in macroblocks. |
| Slice start (`decode_slice`) | `mcacc_slice()` | Writes `A2`: reference lists (the slot of each reference picture, asked with `slot`) and weights. |
| Every macroblock | `mcacc_mb()` instead of `ff_h264_hl_decode_mb()` | Writes `A3`: type, position, mask of the blocks with coefficients, intra modes or references and vectors, then the non-zero coefficients. Does the DC transforms (intra 16x16 and chroma) on the ARM, since they are cheap, and clears the coefficient blocks as the reconstruction would. |
| Picture end (`ff_h264_field_end`) | `ff_h264_mcacc_end()` | Writes `A4` and hands the buffer over (`end`). |

If `mc_acc_hooks` is `NULL` or the context is not the player's, the decoder works as the original.

The full command format is in [`accel/doc/comandi.md`](../accel/doc/comandi.md) (in Italian). In short:
- 32-bit little-endian words;
- `A1` picture start, `A2` slice, `A3` macroblock, `A4` end, `A0` padding (to reach an even number of words, since the core reads 64-bit words);
- a 1080p picture is about **1 MB of commands**: about 30 MB/s at 24 pictures per second.

### `accel.c`: pictures, slots and order

- **Slots.** Every picture FFmpeg allocates (`acc_get_buffer`) takes one of the 64 free slots (an atomic bit mask). The slot stays taken while FFmpeg or the player hold the picture; when the `AVBuffer` is freed the slot is free again. The slot is not FFmpeg's DPB index: a DPB entry can be reused while the player is still showing the picture.
- **Dummy pixels.** All pictures point to one shared pixel buffer (`acc.dummy`). FFmpeg writes into it only to paint missing pictures grey; nobody reads it.
- **Two decoding threads.** Two pictures may be written at the same time. Each picture gets a number in decoding order (`seq`) when it starts. A finished picture waits until all earlier ones are sent: pictures always reach the FPGA in order. At most 16 pictures are being written at once (`ACC_INFLIGHT`).
- **Sessions.** At every seek (`acc_restart`) the session number goes up. Pictures of the old session still being written are dropped.
- **Failure.** Every problem calls `acc_fail()` with the reason. The player sees it and goes back to the normal decoder (chapter 9).

---

## 6. The ARM ↔ FPGA link (`accel_link.c`)

### The `mc_acc` mailbox (at `0x23E00000`, 32-bit words)

| Word | Written by | Meaning |
| --- | --- | --- |
| 0 | ARM | `0x4341434D` ("MCAC") while the player is there; 0 stops the core |
| 1 | ARM | session number: a new value resets the engine and restarts the ring |
| 2 | ARM | physical address of the ring page table |
| 3 | ARM | pages in the ring (1–1024) |
| 4 | ARM | write pointer: bytes written into the ring since the session started (a multiple of 8) |
| 16 | core | `0x4B4F4341` ("ACOK") |
| 17 | core | session being run |
| 18 | core | bytes read from the ring: the ARM may write over them |
| 19 | core | pictures finished |
| 20 | core | bit 0: unexpected command; bit 1: the memory never answered; bits 15:8: last slot; bit 16: `mc_zc` can narrow 2:1 |
| 21 | core | clocks between the last two finished pictures |
| 22 | core | of those, clocks with no commands to read |
| 23 | core | of those, clocks waiting for the memory |
| 24 | core | macroblocks done |
| 25 | core | clocks since the session started |

The core reads the mailbox every 1024 clocks (9.5 µs at 108 MHz). When it sees a new session number it reads the mailbox again (the values written before the number), waits for the transfers under way, resets the engine and reads the page table.

### The command ring

It is a **4 MB** circular buffer (1024 pages of 4 KB). The ARM writes, the core reads, and the two counters (word 4 and word 18) tell how much room there is. Two modes:

- **`MC_ACC_RING=wc` (default).** Pages of the player's own memory, locked (`MAP_LOCKED`). The physical address of each page comes from `/proc/self/pagemap`. The same pages are then mapped a second time through `/dev/mem` with `O_SYNC`: for Linux memory this gives a *write-combine* mapping, which skips the L1 and L2 caches but writes fast. Before use:
  - the cache lines left by Linux's page clearing are pushed out (`al_evict`: `__clear_cache` plus 32 MB read and written through the 512 KB L2);
  - a test pattern is written and checked again.
- **`MC_ACC_RING=so`.** 4 MB outside Linux at `0x2FC00000`. No caches to handle, but every store waits for the memory (*strongly ordered*): useful to compare timings.

**Cache coherence.** The FPGA's port to DDR (f2sdram) does not see the ARM's caches. So:
- the ARM writes the ring without caches;
- after each piece (at most 64 KB, `ACC_CHUNK`) it runs `dsb` and asks the PL310 L2 controller to drain its store buffer (*Cache Sync* register, `0xFFFEF730`), because of erratum 769419;
- only then does it move the write pointer (word 4).

The rebuilt pictures, instead, live outside Linux: the ARM never reads them, so there is no cache problem at all.

### Sending, waiting, drawing

- **`al_send()`** copies a picture's commands into the ring when there is room. If the ring stays full for more than 1 s, or the core reports an error, the accelerator stops.
- **`al_wait()`** waits for the finished-picture counter (word 19) to reach the picture to show. It waits at most 0.5 s: after 3 empty waits in a row the player goes back to the normal decoder.
- **`acc_zc_draw()`** fills the zero-copy mailbox:
  - frame buffer page;
  - lines to make (540);
  - the slot's table;
  - words per line;
  - Y, U and V offsets in the slot (it allows for FFmpeg's cropping);
  - Y step (2 rows, since half the lines are made);
  - the 2:1 bit when the frame buffer is 960 wide.

  Then it writes a new sequence number and waits for the core to repeat it in word 18 of its answer.

### The `mc_zc` mailbox (at `0x23F00000`)

| Word | Meaning |
| --- | --- |
| 0 | `0x435A434D` ("MCZC") |
| 1 | sequence: a new value starts a picture |
| 2 | frame buffer page (physical address) |
| 3 | frame buffer stride in bytes \| lines to make << 16 |
| 4 | page table (physical address) |
| 5 | pages in the table \| 64-bit words per output line << 16 |
| 6 | Y offset |
| 7 | Y bytes between two output lines |
| 8, 9 | U and V offsets |
| 10 | U/V bytes between two output lines; bit 16: chroma line = output line / 2; bit 17: narrow 2:1 instead of 3:2 |
| 16 | clocks spent (answer) |
| 17 | 0 done, 1 refused (bad values) |
| 18 | sequence done (written last) |

---

## 7. The FPGA side

### How the memory is shared (`menu.sv`, `mc_bus.sv`)

The menu core has one DDR port (`DDRAM_*`, 64 bits, bursts of up to 255 words).
- With the frame buffer off, `ddram.sv` uses it to clear memory.
- With the frame buffer on, `mc_bus` uses it, in this order:
  1. **`mc_out`** (CRT output) always goes first: when it needs a line, the others wait for the transfer under way to end;
  2. **`mc_zc`** and **`mc_acc`** take turns: a turn lasts at most 16 transfers for `mc_zc` (`ZT`) and 256 for `mc_acc` (`AT`);
  3. read data go back to whoever asked: the port changes hands only when no read is under way.

### `mc_acc.sv`: the bridge

- A **request queue** (8 entries) holds all transfers in order: the engine's, the mailbox, table and ring reads, and the answer. A **tag queue** (32 entries) says for every read whose data come back and how many words. So the engine sees a simple memory, and no tight timing path runs from the engine to the DDR port.
- **The ring page table** is read at the start of the session into an internal RAM.
- **Ring reads** stop at the end of every 4 KB page. Working out the page end and the last page one clock earlier was needed to meet timing at -40 °C.
- **The command FIFO** (512 words of 64 bits) gives the engine 32 bits at a time, the low half first. A count of reserved words makes sure the words on their way always find room.
- **If the memory does not answer** for too long, the block steps aside and reports it (word 20, bit 1).
- **The answer** (words 16–25) is written again at every mailbox check where something changed: bytes read, pictures finished or a new session.

### `acc_engine.sv`: the engine

The engine works on **two macroblocks at a time**:
- **stage A** reads the macroblock's commands, inverse-transforms the coefficients (`acc_idct`) and puts the residual into `acc_resid`;
- **stage B** does the prediction (intra in the engine, inter with `acc_mc`), adds the residual, clips to 0–255, puts the result into `acc_recon` and writes it to the slot in DDR.

Picture and slice commands wait for stage B to be idle.

- **Intra prediction.** 4x4, 8x8 (with the reference sample filter), 16x16 and chroma, including the "plane" mode. Neighbouring rows and columns come from internal line memories.
- **Inter prediction (`acc_mc`):**
  - quarter-pixel luma with the 6-tap filter, eighth-pixel bilinear chroma;
  - explicit and implicit weights, bi-prediction;
  - edges repeated outside the picture.

  The macroblock is split into regions with one vector per list: 16x16 when all is equal, otherwise 8x8 or 4x4. While one region is calculated, the reference windows of the next are already being read. With two lists, the list 0 prediction is kept in a small RAM and combined with list 1's as FFmpeg does.
- **Speed measured in simulation** (108 MHz):
  - P pictures about 251 clocks per macroblock (19 ms for a 1080p);
  - B about 340 (27 ms);
  - I about 140 (10.6 ms).

  A 1080p picture has 8160 macroblocks and the budget is 552 clocks per macroblock. On the MiSTer, in heavy scenes, the engine takes about 20–33 ms per picture, of which up to about 19 ms waiting for the memory.

### `mc_zc.sv`: from slot to frame buffer

- Reads the mailbox every 4096 clocks.
- For every output line it reads one Y line and one U and V line through the page table, in bursts of at most 32 words split at the 4 KB pages.
- Narrows:
  - **3:2** (1920 → 1280): every 3 pixels give 2, with 2/3 and 1/3 weights computed in logic, not in the DSP blocks, which the HDMI scaler needs;
  - **2:1** (1920 → 960): the average of two pixels.
- Writes the YUYV line into the frame buffer. It has two line buffers that take turns.

### Then: colours and outputs

The YUYV 4:2:2 frame buffer is converted to RGB inside the core (frame buffer format, bit 5). Then:
- the MiSTer scaler takes it to the HDMI resolution;
- `mc_out` resamples it to 720 pixels for the 15 kHz CRT (480i/576i).

The player flips pages with `fb_cmd3 page <n>`.

---

## 8. The journey of a picture

1. The player's video thread hands a packet to FFmpeg.
2. One of FFmpeg's two threads starts the picture:
   - `acc_get_buffer` gives it a slot;
   - `mcacc_begin` writes `A1`;
   - `A2` for every slice, `A3` for every macroblock, `A4` at the end.
3. `acc_end` puts the picture in line. When its turn comes, `al_send` copies it into the ring and moves word 4.
4. `mc_acc` sees the pointer move, reads the ring in bursts and fills the command FIFO.
5. The engine rebuilds the macroblocks and writes them into the slot. For references it reads the slots of earlier pictures.
6. At the end of the picture `mc_acc` raises word 19 and writes the timings.
7. When the picture is due, `yuv_draw` calls `acc_zc_draw`:
   - `al_wait` waits for word 19;
   - the `mc_zc` mailbox gets the slot's data;
   - `mc_zc` writes the free frame buffer page.
8. The player shows the page with `fb_cmd3 page`, on time for the audio.
9. When FFmpeg and the player let go of the picture, its slot is free again.

---

## 9. Seeks, errors and going back to the normal decoder

- **Seek forward or back:** `acc_seek()` → `al_restart()`. New session in `accel.c` and in the core; pictures of the old session are dropped ("scartate dopo un salto" in the log).
- **Failure:**
  - format not 8-bit 4:2:0;
  - size changed;
  - out of slots;
  - ring full for over 1 s;
  - memory not answering;
  - unexpected command;
  - 3 pictures in a row not finished within 0.5 s;
  - zero-copy refusing or not answering.

  At the next round `acc_fallback()` closes the decoder, opens a normal one and goes on from the same moment (`media_seek`). In the log: `diag acceleratore: spento per questo video a ... s (reason)`.
- **A format the core cannot draw** (for example a 720-wide frame buffer for a format chosen with Start): the accelerator stops only while it lasts (`acc_file_off = 2`). When the format can be drawn again for at least 1 s, `acc_resume()` turns it back on.
- **`MC_NOACC=1`** in `/media/fat/linux/mediacenter.ini`: the accelerator never turns on.
- **A core without the accelerator:** the session gets no answer and the player uses the normal decoder.

---

## 10. Diagnostics in the log

With diagnostics on (the default; `MC_DIAG=0` turns them off), `/tmp/mediacenter.log` holds (the log is in Italian):

- at open: `diag acceleratore: acceso (anello ..., svuotamento L2 si, sessione N, pronto in X ms)` or `spento (reason)`;
- every 10 seconds of video:

  ```
  diag acceleratore 120 s: inviate 240 immagini (980 KB l'una, invio 1.2 ms, 0 attese dell'anello),
  FPGA 33.0 ms tra due immagini (senza comandi 8.0 ms, attesa memoria 19.0 ms),
  disegnate 240 (attesa media 0.5 ms), in ritardo 0, perse 0, slot al massimo 20
  ```

  (example with indicative values):
  - *inviate* (sent) and *invio*: the ARM's time to copy into the ring;
  - *FPGA tra due immagini*: the FPGA time between two pictures, from word 21;
  - *senza comandi*: the engine was waiting for the ARM;
  - *attesa memoria*: the engine was waiting for DDR;
  - *disegnate*: drawn;
  - *in ritardo* (late): the picture was not ready within 0.5 s;
  - *perse* (lost): from an old session or after an error;
- at the end a summary with the same fields and, if it happened, the reason it stopped.

The `diag decoder` and `diag zero-copy` lines tell how hard the rest of the player is working.

---

## 11. Performance and resources (version 3.0)

| Measure | Value |
| --- | --- |
| Menu core with the accelerator | 55% of the logic (ALM), 17% of the internal memory, 85% of the DSPs (95 of 112) |
| Clock | 108 MHz (`clk_sys`); timing margin +0.26 ns, positive at -40 °C too |
| Engine alone (synthesis `accel/syn`) | about 12.9k ALM (31%), 39 DSP, 30 M10K blocks, Fmax about 112 MHz |
| One Piece 1080p on the MiSTer | 238 s without seeks: 5724 pictures drawn by the FPGA, 0 lost, 0 late; ARM 5–13 ms per picture |

---

## 12. Building and testing

**The core.** `fpga.yml` compiles `fpga/menu/menu.qpf` with Quartus 17.0.2 (docker `theypsilon/quartus-lite-c5:17.0.2`). It runs by itself when `fpga/**` or `accel/rtl/**` change, and prints the fit summary and the slowest paths from `timing.tcl`. Locally:

```
docker run --rm -v "$PWD:/build" -w /build/fpga/menu theypsilon/quartus-lite-c5:17.0.2 quartus_sh --flow compile menu.qpf
docker run --rm -v "$PWD:/build" -w /build/fpga/menu theypsilon/quartus-lite-c5:17.0.2 quartus_sta -t timing.tcl
```

Each compile can shift the paths a little (fitter noise): always check `timing.tcl` at the 100 °C and -40 °C corners.

**The player.** `build.yml`:
- uses the `gcc-arm-10.2-2020.11` ARM compiler;
- builds FFmpeg 6.1.2 with `build_ffmpeg.sh`, which applies `ffmpeg-mediacenter.patch` (the cache is renewed when the patch changes);
- then runs `make mediacenter FFMPEG=... DVDNAV=...`.

**Simulation tests** (Icarus Verilog and Python), no MiSTer needed:
1. Build FFmpeg 6.1.2 for the PC (C only) with `ffmpeg-mediacenter.patch` and `accel/sim/ffmpeg-accel-dump.patch` (see `accel/sim/README.md`).
2. `accel/sim/make_tests.sh <folder> [1080p file]` makes the test videos:
   - t1–t6: intra, weights, CAVLC, coefficients, 1080p;
   - b1–b4, b6: B pictures.
3. `accdump <video> <prefix>` writes `<prefix>.cmd` (the commands) and `<prefix>.rec` (FFmpeg's pictures without deblocking).
4. `model.py <prefix>`: the Python model rebuilds from the commands and compares.
5. `accel/sim/tb/run_engine.py <prefix> [pictures]`: the Verilog engine, bit-exact comparison.
6. `accel/sim/core/run_core.py <prefix> [pictures] [ring pages] [latency]`: `mc_acc` plus the engine with the ring.
7. `acctest` (with `-DACC_FAKE`): the real ARM side (`accel.c`, `accel_link.c`) against a pretend core that reads the ring at random speeds.

Every change to the engine must stay **bit-exact** with FFmpeg on all test videos before it goes to the MiSTer.

**On the MiSTer:**
- `mediacenter --zctest <file>` tests zero-copy and the caches;
- `mediacenter --bench-dec` and `--bench-parse` measure the normal decoder and stream parsing alone (`MC_PARSEONLY`);
- the reference test is One Piece Episode 940 1080p, with the `diag acceleratore` and `diag decoder` logs.

---

## 13. Starting again from scratch: the suggested order

1. **Measure first.** With `--bench-parse` you see what stream parsing alone costs the ARM. If it is not comfortably below one picture's time, the accelerator is not enough.
2. **Fix the contract:** the command format (`comandi.md`) and a software model (`model.py`) that rebuilds exactly as FFmpeg does. Everything else is tested against it.
3. **The engine in simulation:** first I and P, then B, always bit-exact, then synthesis alone for size and speed.
4. **The link:** mailbox, ring, sessions, caches (the most delicate part on the MiSTer: f2sdram does not see the ARM's caches; the PL310 holds writes back).
5. **Drawing:** zero-copy from the slots to the frame buffer, with the CRT output going first.
6. **Going back to the normal decoder** in every case the FPGA does not do, so the player never hangs.

---

## 14. What can be improved

- **The deblocking filter in the FPGA**: the next planned step. It needs a little more data from the ARM (qp, edge strengths) and a pass over the macroblock edges; it must fit in the remaining time (today about 20–33 ms of 41.7 in heavy scenes) and in the remaining DSPs (about 15%).
- **Less memory waiting**: up to about 19 ms per picture in heavy scenes. Longer bursts for the reference windows, reusing windows between neighbouring regions and better-suited `mc_bus` turns could help.
- **Other formats:** widths other than 1920 (1440, 1280), interlaced H.264 (MBAFF/PAFF), 10-bit; each needs changes to the commands and the engine.
- **More compact commands**: about 1 MB per picture today; less data means less ARM and memory time.

---

*License: as the rest of the project (GPL-3.0, see `LICENSE`). Developed with the help of AI (Anthropic's Claude).*
