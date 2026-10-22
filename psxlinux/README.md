# SMS Plus for PSXLinux

A port of Charles Mac Donald's SMS Plus (Master System / Game Gear
emulator, see ../porting.txt for the original documentation) to
PSXLinux — the uClinux 2.4 port for the Sony PlayStation.

The frontend is fully self-contained: no libc, no SDK libraries, just
`psxsys.h` (raw syscall wrappers) from the kernel tree and the bare
hardware registers of the console.

## Files

| file          | purpose                                              |
|---------------|------------------------------------------------------|
| `main.c`      | GPU frontend, pad polling, frame pacing, ROM loader   |
| `spu.c/h`     | SPU ADPCM streaming audio (two-voice ring)            |
| `hw.h`        | hardware register definitions                         |
| `portlib.c/h` | freestanding runtime pieces: malloc pool, string/stdio|
| `psxlinux.ld` | linker script: everything at 0x00100000, flat bss      |
| `makefile`    | cross build (`mipsel-linux-gcc`) + elf2ecoff conversion|

## Building

```
cd psxlinux
make
```

produces `smsplus.bflt` (a raw ECOFF image, the format the kernel's
`fs/binfmt_ecoff.c` loader expects).  `ROM_SIZE` in `main.c` bounds the
ROM buffer (128K, see *Memory budget* below).

## Building a bootable disc

The kernel itself is booted as a PS-X EXE (the `SLID_NET.YAR` chain:
`vmlinux.ecoff` -> `eco2exe` -> `exefixup`), and this emulator runs as
the `init` program:

```
mkdir stage
cp <validated kernel exe> stage/SLID_NET.YAR   # BOOT= entry of SYSTEM.CNF
printf 'BOOT = cdrom:SLID_NET.YAR;1\nTCB = 4\nEVENT = 16\nSTACK = 801FFFF0\n' \
      > stage/SYSTEM.CNF
cp smsplus.bflt stage/gltron                   # kernel cmdline: init=/gltron
cp game.sms stage/game.sms                     # ROM, ISO level 1 (8.3 names)
cd stage && genisoimage -sysid PLAYSTATION -volid SLID_NET -iso-level 1 \
      -o game.iso .
```

The kernel's isofs driver strips `;1` and lowercases, so the emulator
opens `game.sms` (also tries `game.gg`, `rom.sms`, ...).

## Memory budget (why 128K ROMs)

The ECOFF loader places the image at 0x00100000 and the user stack
0x6000 above its end, without reserving any of it.  The kernel's page
allocator hands out from the top of RAM (free zone ~0x1B2000..0x200000,
"312k available" on the stock kernel), so a too-large bss overlaps
live kernel data (bss zeroing then panics the kernel).  Boot-load
probes on the stock kernel validated image+stack up to ~0x1860F0; the
current binary ends at ~0x1A98CC (stack top ~0x1AF8CC) which has
proven stable — keep new static buffers small and check the LOAD
segment `MemSiz` after touching any buffer.

BSS is dominated by: ROM buffer (128K), renderer pattern cache (128K),
16bpp LUT (64K), emulator contexts (~48K), bitmap (24K), VDP (16K).
The YM3812 (FM) emulator would need ~62K of *dynamic* tables on top,
which does not fit — FM support is therefore disabled on this target
(`PSXLINUX` guard in `system.c`, guarded use in `sms.c`) and sound is
PSG-only.

256K ROM images would need a kernel-side loader/allocator fix
(e.g. reserving the image span).  `/opt/PSXLinux2` has a bootmem
reservation for exactly this, but with the static 768K zone the kernel
boots with only ~248K free and currently OOM-panics before mounting the
CD — needs follow-up there.

## Hardware findings

All of these were verified against PCSX ReARMed and, where possible,
cross-checked with the kernel's own demo (`psxgame.c`) and the old
real-hardware smsplus PSX port (`../psx/psx.c`).

### GPU

- **A0 CPU->VRAM transfers use three setup words**: `0xA0000000`,
  `(y<<16)|x`, `(h<<16)|w` (w in halfwords), then `w*h` data words.
  Merging the coordinates into the command word desynchronizes the
  GPU's packet parser: every subsequent GP0 word — fills, sprites,
  E1-E5 state — is silently swallowed as *pixel data* of a bogus
  transfer whose width/height come from the wrong words.  This was the
  nastiest bug of the port: nothing crashes, the GPU just eats the
  command stream forever.
- **Fill rectangles (0x02)**: `word0 = 0x02 | b<<16 | g<<8 | r`,
  `word1 = abs XY`, `word2 = (h<<16)|w`.  Fill ignores the draw offset
  and clip area, so clear a specific buffer with absolute y.
- **Textured rectangles (0x64)**: the CLUT attribute lives in bits
  16-31 of the UV word: `(clut_attr<<16) | (v<<8) | u`, where
  `clut_attr = (cy<<6)|((cx>>4)&0x3F)`.
- **4bpp textures** (used by the SMS tile renderer): one pattern is
  32 bytes; the A0 header is the same three-word form.  peops' 4bpp
  sampler takes texel `2b` from the **low** nibble of byte b and
  texel `2b+1` from the high nibble (verified against
  `drawPoly4TEx4` in `plugins/dfxvideo/soft.c`), and addresses
  texel (u,v) at VRAM byte `v*2048 + (xbase<<1) + u/2` - i.e. texture
  rows stride the full 1024-halfword VRAM width, so a 4bpp texture
  page only occupies 64 halfwords of each row.
- **Draw coordinates are integer pixels**: the GPU's polygon and
  rectangle coordinates carry no subpixel fraction (a value packed
  as 1/8-pixel eighths lands at 8x the intended position).
- **Display 256x240** (`GP1(08) = 0x08000000`, like the original
  `psx/psx.c` frontend): the SMS's own 256-pixel width, which lets
  every tile be drawn 1:1 with the 4-word rectangle primitive.
  Double buffered at display y 0/240; tile patterns cached in VRAM
  as 4bpp textures on pages 0x05..0x08 (flip variants), CLUTs at
  (512,208).
- **GPUSTAT waits**: PCSX only exposes bit28 as "ready to receive";
  waiting on bit29 hangs forever.  Every wait in this port is bounded.
  Bit26 ("drawing") is polled with a timeout only.
- **bit31 is useless for vsync in 240p**: PCSX only toggles the
  interlace field bit in interlaced display modes.  Frame pacing uses
  **Timer 1 in hblank mode instead** (mode bit8 = 0x0100, 262 ticks per
  field; the kernel's tick owns Timer 2, nothing claims Timer 1).  The
  pacing loop is wrap-safe and gives up after a bounded spin so a
  frozen timer can never wedge the frontend.

### SPU (streaming audio)

- Two voices (L at SPU RAM 0x1000, R at 0x1700), each an 8-slot ring of
  14 ADPCM blocks x 28 samples.  Every slot's last block carries
  End+Repeat (flag 0x03) so playback chains via LSAX; the reader
  position is tracked through the SPU IRQ (I_STAT bit9) and slots are
  produced 2+ ahead.  A two-pass ADPCM encoder (5 standard filters,
  shift +/-1 candidates, squared-error pick) converts the mixed
  22050 Hz frame buffers on the fly.
- **Manual SPU RAM writes** follow the kernel demo's protocol:
  `TSA = dest`, `CTRL = 0x0005` (manual write), data in bursts, then
  `CTRL = 0xC000`.  Writing 0xC010 (the "obvious" manual-write bit)
  does not work.
- **Sustain-forever envelope = ADSR1 0x000F / ADSR2 0x0000** (instant
  attack, decay rate 0 = frozen).  0x00FF decays to silence.
- The kernel's IRQ convention: **I_MASK bit SET = enabled**; `I_STAT`
  writes are `&= value`, so writing `~(1<<bit)` acknowledges a source.
  Never OR bits into I_MASK, and never leave an enabled source without
  a registered handler (`do_IRQ` panics).

### Pads

- The SIO data port (0x1F801040) is **byte-wide**: a 16-bit access
  dispatches two protocol bytes (PCSX literally calls
  `sioWrite8(value); sioWrite8(value>>8)`).
- Digital pad poll: `CTRL=slot (0x1003/0x2003)`, send `0x01`, `0x42`,
  then two `0x00`; replies `0xFF`, `0x5A` (ID), then the 16-bit button
  word (1 = released): SELECT 0x0001, START 0x0008, UP 0x0010,
  RIGHT 0x0020, DOWN 0x0040, LEFT 0x0080, CROSS 0x4000, SQUARE 0x8000.
- Button data is only trusted when the ID byte was 0x5A; SELECT must be
  held 30 frames to exit the emulator.

### Kernel / disc

- `sys_read` on isofs can return short, and the psxcd data-interrupt
  path can race and deliver **zeroed sectors while the byte count still
  advances**.  A partially corrupted ROM boots but renders black, so
  the loader loops short reads and verifies every 8KB chunk by
  re-reading it (up to 4 tries, then a final confirmation read).
- The kernel has no /dev tree on the ISO (devfs does not mount), so
  userspace printf falls back to raw SIO1 writes, which the harness's
  `sio.c` echo patch mirrors into the PCSX log (the F-line diagnostics
  arrive that way).  The GPU breadcrumb marks that early bring-up used
  to localize hangs in frame dumps were removed once the console path
  worked end to end.

## Harness notes (PCSX ReARMed)

Two changes were made to the shared test harness and are needed for
correct pad behaviour:

- `libpcsxcore/sio.c`: the serial-console echo patch (kernel printk to
  stdout) must not swallow the pad (`0x01`) and memcard (`0x81`) start
  tokens, or the pad state machine never engages and every poll returns
  stale bytes that read as random button presses.
- `frontend/plugin_lib.c`: the frame-dump debug hook was capped at 40
  flips (~0.7 s of app runtime); raised to 240.

`/opt/PSXLinux` remains unmodified (read-only reference).  Kernel-side
work belongs in `/opt/PSXLinux2`.

## Controls

| PSX pad    | emulator            |
|------------|---------------------|
| CROSS      | button 1            |
| SQUARE     | button 2            |
| START      | pause / start       |
| D-pad      | directions (2 pads) |
| SELECT (hold 0.5 s) | exit      |
| SELECT + START     | sound on/off |

## Performance

Under PCSX ReARMed's interpreter the guest runs at roughly real
PlayStation speed, so the wall-clock frame rate is essentially
`60 * 262 / t1` with `t1` the Timer-1 hblank count per frame reported
by the F-line (a full-speed SMS frame needs 262).

- **SMS frames are drawn by the GPU itself** (`tiles_submit_frame`):
  the software renderer's per-pixel memory traffic — measured at
  230-480 of a frame's ~550 ticks — is replaced by ~770 textured
  rectangles, one per visible name-table entry, into a 256x240
  display.  Pattern data is cached in VRAM as 4bpp textures; a
  variant is re-uploaded lazily, inside the frame's packet list, only
  when a quad first samples it after the game rewrites it.  The whole
  list reaches the GPU with one DMA.
- **Sound starts disabled**: the PSG update plus the SPU streaming
  cost about 16% of a frame, and most host setups have no audible SPU
  output anyway.  The buffers are still allocated, so SELECT+START
  toggles the stream at any time.
- **Game Gear and raster effects** use the software bitmap path
  (`render_line` plus a 1:1 textured quad), and so do SMS frames that
  follow a mid-frame scroll-register write (flagged by the
  `vdp_raster` hook in `vdp.c`).

Measured with Alex Kidd under PCSX (t1 ticks per frame, fps):

| scene                | software path   | GPU tile path   |
|----------------------|-----------------|-----------------|
| title / attract      | 772-906, 17-20  | 556-566, ~28    |
| scrolled demo        | 953-1070, 15-17 | 563-566, ~28    |
| early play           | 658, 23         | 400-517, 30-39  |

The remaining cost is genuine emulation work: the Z80 executes about
7000 real instructions (0 burned cycles) per frame, ~473 ticks, and
the tile list build plus VRAM refresh costs ~80.  `-O2` and `-O3`
build at the same wall speed (including `-O3` on the Z80 object
alone), so the build uses `-O2` to stay under the stack budget.
