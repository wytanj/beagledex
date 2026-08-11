# firmware

## `pin_config.h` — reconstructed, and why

Every example in `waveshareteam/ESP32-S3-Touch-AMOLED-1.8` does
`#include "pin_config.h"`, **and the header is not in the repository.** Nothing in that
repo compiles from a clean clone. The values here were reconstructed from two independent
sources that agree on the five I2S pins, which is what makes the rest trustworthy:

| Source | Gave us |
|---|---|
| Vendor `examples/arduino-v2/examples/15_ES8311` | `MCLK 16`, `BCLK 9`, `WS 45`, `DIN 10`, `DOUT 8`, sample rate, gain, volume |
| `78/xiaozhi-esp32` → `main/boards/waveshare/esp32-s3-touch-amoled-1.8/config.h` | the same five I2S pins, plus `SDA 15`, `SCL 14`, **`PA 46`**, display QSPI, `BOOT 0` |

Xiaozhi is the authoritative second source because **the factory firmware on this board
*is* Xiaozhi** — that config is what the shipping image actually runs.

**Revision-independent.** Two hardware variants exist (v1 / v2) with separate factory
images (`_250805`, `V2_260601`) and separate xiaozhi board directories — but both configs
were compared and **every pin is identical**: I2S, I2C, `PA`, display QSPI and `BOOT`.
Whatever differs between the variants, it isn't these pins, so this header needs no v1/v2
guard.

## `translator-p01` — build plan phases 00 + 01

- **Phase 00** ES8311 comes up; 4 s of mic→speaker loopback on boot
- **Phase 01** hold `BOOT` to record into PSRAM, release to hear it back

Runs with no network. Add `secrets.h` (copy `secrets.h.example`) and it also registers
with the console and ships logs there — but audio works either way, which is the point:
this phase proves the audio path in isolation so nothing else can be blamed.

### Dependencies

`es8311.c`, `es8311.h`, `es8311_reg.h` sit beside the sketch — already fetched from the
vendor repo. No libraries needed beyond the ESP32 Arduino core.

### Build

```bash
arduino-cli core install esp32:esp32

npm run fw:build          # → firmware/translator-p01/build/translator-p01.ino.bin

npm run flash -- --port COM3 \
  --bin firmware/translator-p01/build/translator-p01.ino.bin \
  --version 0.1.0 --features audio.loopback,audio.ptt
```

Build through `npm run fw:build`, not `arduino-cli compile` by hand: the FQBN in
`scripts/build.mjs` carries `CDCOnBoot=cdc`, and leaving that off is the first row of the
table below. It prints the `.bin` path and nothing else on stdout, so it composes:

```bash
npm run flash -- --port COM3 --version 0.1.6 \
  --bin "$(node scripts/build.mjs firmware/translator-p01 --quiet)"
```

PSRAM **must** be enabled or `ps_malloc` fails and the sketch halts with a message saying
so — a deliberate loud failure rather than a mysterious reboot.

### Expected serial output

Captured from 0.1.5 with `npm run monitor`, which resets the board on connect and stamps
each line with seconds since then:

```
   0.6s  translator-p01 0.1.5 — phase 00 + 01
   0.6s  device 28848590ba0c
   0.6s  buffer 1875 KB in PSRAM (30 s max)
   0.6s  no secrets.h — offline mode, serial logging only
   0.6s  [info/codec] ES8311 up @ 16000 Hz, vol 85, gain 2
   0.6s  [info/i2s] standard mode, 16-bit, 2 slots
   0.6s  [info/p00] capturing 3000 ms with output muted — say something
   4.7s  [info/p00] captured 3.00 s (96000 samples)
   4.7s  [info/p00] levels  L peak    259 rms    53  |  R peak      0 rms     0
   4.7s  [info/p00] one channel carrying signal — normal for a mono codec
   4.7s  [info/p00] playing it back — you should hear yourself now
   4.7s  [info/p01] playing back 3.00 s
   7.5s  [info/p01] playback done
   7.5s  [info/boot] ready — hold BOOT to record, release to hear it back
```

Two numbers that look wrong but aren't. The buffer is **1875 KB**, not the 937 KB of the
pre-0.1.2 builds: capture runs two 16-bit slots since the STEREO fix, so the same 30 s
costs twice the bytes. And **`R peak 0`** is correct — the ES8311 is mono, so only the
left slot ever carries the microphone. Levels in the low hundreds are room noise; speak
at it and `L peak` should climb well past 1000.

### If it doesn't work

| Symptom | Cause |
|---|---|
| **Flash verifies, board looks completely dead, zero serial output** | `CDCOnBoot` defaults to **Disabled**, which routes `Serial` to the UART0 *pins* — nothing reaches USB, over the very cable you just flashed with. The FQBN in `scripts/build.mjs` sets `CDCOnBoot=cdc`; if you compile by hand, don't omit it. Check `otadata` before suspecting the bootloader: blank (both slots `0xffffffff`) now means it runs `ota_0`, so a verified write at `0x110000` *is* the running app. |
| `es8311_create failed` | Nothing at 0x18 on SDA=15/SCL=14. Wrong board revision, or another driver holding the bus. |
| Loopback runs, silence | `PA` (GPIO46) not high. Check `startCodec()` asserted it. |
| `ps_malloc failed` | PSRAM not enabled in board options. |
| Records but plays nothing | `recorded == 0` → I2S read returning 0. Check `DIN` is 10, not 8. |
| Crackling or dropouts | PSRAM bandwidth contention. This board's PSRAM is **quad, not octal**. Keep I2S DMA buffers in internal SRAM and don't repaint the display during capture. |
| **Screen frozen on an old image while serial is perfectly healthy** | Expected, not a hang. Nothing in phases 00–01 touches the panel, and the SH8601 self-refreshes whatever framebuffer it was left holding — so the last frame the vendor Xiaozhi firmware drew is still latched there. A reset won't clear it: this sketch drives no panel reset or power line. Cut power fully to blank it, or wait for phase 4 `ui.display`. Diagnose from serial, never from the screen. |
| **Screen went black after a long button hold — is the board off?** | Probably not. A PWRKEY long press is handled in AXP2101 hardware, independent of firmware, and drops the panel rail — but USB 5 V keeps the ESP32 running, so push-to-talk and the microphone still work with a dark screen. In phases 00–01 the panel is not a power indicator in either direction. To check for real: if `USB\VID_303A&PID_1001` still enumerates as a COM port the chip *is* powered, because that bridge is inside the S3 die. Unplug USB — and the cell, if one is fitted — to actually power down. Phase 4 display work will need to bring that rail back up through the PMIC. |

## `partitions-translator.csv` — the A/B layout

Replaces Waveshare's Xiaozhi layout (factory, one OTA slot, and two SPIFFS partitions of
vendor assets) with two app slots and one big storage partition:

| Offset | Name | Size | Note |
|---|---|---|---|
| `0x009000` | `nvsfactory` | 200 KB | per-unit calibration — **never erase** |
| `0x03b000` | `nvs` | 840 KB | |
| `0x10d000` | `otadata` | 8 KB | |
| `0x10f000` | `phy_init` | 4 KB | |
| `0x110000` | `ota_0` | 3 MB | app images go here |
| `0x410000` | `ota_1` | 3 MB | |
| `0x710000` | `storage` | 8.94 MB | |

Two rules it obeys. Everything below `0x110000` keeps its vendor offsets, so `nvsfactory`
stays exactly where it was. And `ota_0` starts precisely where `factory` used to, so the
app flash address never changed — tooling and muscle memory both survived. Partitions are
contiguous and end exactly on `0x1000000` with nothing stranded.

With no `factory` partition, a blank `otadata` makes the bootloader run `ota_0`; that is
the intended state after writing this table. The cost is that two bad OTA images in a row
need a USB recovery rather than a factory fallback — a ~6 s reflash here, so it buys 3 MB
of storage cheaply.

**Verified on the device**: written, read back, and `ptable-readback.bin` is byte-identical
to `partitions-translator.bin`. 0.1.5 boots from `ota_0` under it.

```bash
python -m esptool --port COM3 --no-stub write-flash 0x8000 firmware/partitions-translator.bin

# read it back and prove it took — these two files should match byte for byte
python -m esptool --port COM3 --no-stub read-flash 0x8000 0xc00 firmware/ptable-readback.bin
```

## Flashing rules for this board

- **App images go to `0x110000`** (`ota_0`), not the conventional `0x10000` — which on
  this partition layout is inside `nvs`. `scripts/flash.mjs` defaults to `0x110000`.
- **Never `erase-flash`.** `nvsfactory` at `0x9000` holds per-unit calibration that is in
  no vendor download. `flash.mjs` refuses the flag.
- **Use `--no-stub`.** The esptool stub flasher desyncs on this board's native
  USB-Serial/JTAG bridge — `Packet content transfer stopped` at ~16 KB, identically at
  115200 and 921600. `--no-stub` is slower (~170 kbit/s) but reliable at any length.
- Backups of this unit's irreplaceable region live in `~/esp32-backups/`.
