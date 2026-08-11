# Stack

What Beagledex is built from, and why each piece is there. The versions are what's
in use as of this writing; the reasoning is the durable part.

## The shape that explains everything

**The device is a thin UI layer; the host does the heavy lifting.** Beagledex
captures audio, plays audio, and draws pixels. Everything hard — API keys, speech
recognition, translation, text-to-speech, MP3 decoding, font rendering — lives on
the console (a Node server) or in the cloud. This isn't tidiness for its own sake;
it's forced by two facts:

- **Firmware flash is not a secret store.** A full chip dump is a six-second job on
  this board (there's one on disk in `~/esp32-backups/`). So no key, ever, ships to
  the device.
- **An ESP32-S3 is a small computer.** It can't run a good STT model, a CJK font
  engine, or an MP3 decoder well. The host can, trivially.

Almost every technology choice below is a consequence of that split. The device
stays dumb on purpose, which is also why the whole cloud/provider side can be
swapped without touching firmware.

---

## Firmware (the device)

**Language: C++ / Arduino framework for ESP32** — esp32:esp32 core **3.3.11**,
compiled with **arduino-cli 1.5.1**.
Arduino over bare ESP-IDF because the peripheral libraries (I2S, WiFi, display,
NVS) are batteries-included and the board's whole ecosystem (vendor examples,
xiaozhi) is Arduino-shaped. The FQBN is pinned in `scripts/build.mjs` because the
board options are *not* optional here — `PSRAM=opi`, `CDCOnBoot=cdc` (without it
Serial vanishes and the board looks dead), `FlashSize=16M`.

**Display: GFX Library for Arduino (Arduino_GFX) 1.6.7** driving a **CO5300** AMOLED
over QSPI.
Immediate-mode drawing, no framebuffer. That matters: a 368×448×16-bit framebuffer
is 322 KB, which on this board would live in *quad* (not octal) PSRAM and contend
with the audio capture buffer — the documented cause of crackle. Arduino_GFX writes
straight over QSPI with no buffer. **LVGL was deliberately rejected** for the same
reason: a widget tree and its framebuffer buy nothing a watch face and a token
stream need. (The panel is CO5300, not the SH8601 the box claims — established by
three independent measurements; see `firmware/pin_config.h`.)

**Audio: ESP_I2S + a vendor ES8311 C driver.**
The ES8311 is a mono codec on a shared I2C bus; ESP_I2S handles the I2S transport.
Capture is half-duplex by hardware necessity — mic and speaker are centimetres
apart with no echo cancellation, so a live loopback saturates.

**Storage: Preferences (NVS).**
WiFi credentials, the device token, the console URL and the engine toggle live in
the ESP32's NVS partition — not in `secrets.h` (a rebuild to change networks, one
careless commit from public) and not in tracked config (a WiFi password has no place
in git). NVS is unencrypted, which is acceptable for a personal device on its own
networks.

**Networking: WiFi + HTTPClient (ESP32 Arduino).**
A non-blocking round-robin over stored networks (home, phone hotspot) so it joins
whichever is in range without a UI-freezing scan. **WiFiMulti was tried and dropped**
— its scan blocks and it only ran during a capture, so the device never proactively
found the hotspot out and about.

**Assets: `esp_partition_mmap`.**
The watch face photo and the pixel mascot are RGB565 blobs in the `storage`
partition, memory-mapped and blitted with zero copy and zero decode. They survive
every app reflash (only `0x110000` is rewritten), and upload as a hash-verified
esptool partition write in ~4 s rather than 30 s over the serial protocol.

---

## Console (the host: web UI + orchestrator)

**Framework: Nuxt 4 (Vue 3) on Nitro** — nuxt **^4.0.0**, vue **^3.5.0**,
vue-router **^4.4.0**, on **Node 24**.
One framework for the fleet UI *and* the server API (`/api/translate`, device
register/heartbeat/logs, OTA manifest). Nitro's server routes are where the API keys
and all the orchestration live. Vue/Nuxt because the UI is modest and the value is in
having UI + API in one deployable unit.

**Storage: Nitro's file driver over plain files in `.data/db`.**
No database. Every store call is driver-agnostic, so moving to Vercel KV or Upstash
is one line in `nuxt.config.ts`. This is right for a fleet of tens; a real
time-series store is the answer only at hundreds of devices with log retention. The
flash history is written by the tool that moved the bytes (`scripts/flash.mjs`), so
the record can't drift from reality.

**Text rendering: @napi-rs/canvas (Skia) ^1.0.5.**
The device's built-in font is ASCII, so it can't draw Japanese/Chinese/Arabic/
Devanagari. The console renders the reply to an RGB565 bitmap with Skia and the
device blits it — same division of labour as the API key. Skia specifically (not
Pillow) because it carries its own HarfBuzz: Arabic joins and shapes, Devanagari
forms conjuncts. Pillow's shaping (`raqm`) is absent on this host, so it would
silently render those scripts wrong.

**MP3 decode: mpg123-decoder (WASM) ^1.0.3.**
xAI's TTS only returns MP3, and the device plays raw PCM. A pure-WASM decoder runs
in-process in the Node server — no native build, no ffmpeg, no Python hop in the
request path — decoding MP3 → PCM which is then resampled to the 16 kHz the codec
plays.

---

## AI providers

**Default: xAI (grok).** STT (`/v1/stt`, takes raw 16 kHz PCM — exactly what the
codec produces, so nothing resamples), chat/translate
(`grok-4.20-0309-non-reasoning` — chosen by measurement: it matched the reasoning
models' translation output at ~⅓ the latency), and TTS.

**Alternate: Google Gemini** (`gemini-flash-lite-latest`), selectable per request
(`?provider=gemini`) and toggleable on the device. Gemini takes audio natively, so
transcribe + translate is one call instead of two — architecturally cleaner and
cheaper per unit. But **measured** it's ~2× slower than grok on this workload, so
grok stays the default; latency is the product for a hand-held translator. The point
of keeping both is that the device never sees the provider — it's a pure host choice,
so we A/B on real numbers instead of guessing. (TTS is one implementation for both,
so a comparison isolates the understanding leg.)

Not chosen: fully on-device or on-host local models (Whisper, Piper). They're the
genuine cost-to-zero and privacy lever and are noted in `DESIGN_PLANS.md`, but the
cloud path is near-free and higher quality, and for a home device on always-present
WiFi the trade isn't worth it yet.

---

## Tooling

**Firmware build/flash: arduino-cli + esptool 5.3.1**, wrapped in Node
(`scripts/build.mjs`, `scripts/flash.mjs`).
`flash.mjs` wraps esptool so the flash *record* is written by the thing that moved
the bytes. It refuses `--erase-flash` (the `nvsfactory` calibration at `0x9000` is in
no vendor download) and defaults to `0x110000`, refuses to flash a version the sketch
disagrees with, and uses `--no-stub` because the stub flasher desyncs on this board's
native USB-Serial/JTAG bridge.

**Device scripts: Python 3.14** — `monitor.py` / `serial-log.py` / `push.py` use
**pyserial 3.5** to talk to the board over USB (watch serial, bridge it into the
store, push config); `face.py` uses **Pillow 12.3.0** to pixelate/convert images to
the RGB565 blobs the device blits.
Python for the serial and image work because pyserial and Pillow are the path of
least resistance there; Node for the build/flash orchestration because esptool
integrates cleanly and the console is already Node. The project is deliberately
bilingual — each half uses what fits.

**Version control: git + GitHub** (`github.com/wytanj/beagledex`).
Settings live in git as tracked JSON (`config/`) — intent is versioned and a bad
setting is one `git revert` away. Observation (flash history, logs) stays in `.data`,
untracked, because it belongs to the bench it happened on. Secrets never enter git:
`.env`/`.env.*` and generated blobs are ignored, and every commit is audited against
the real key/password values before it lands.

---

## Hardware

**Waveshare ESP32-S3-Touch-AMOLED-1.8, v2** — ESP32-S3 (dual LX7 @ 240 MHz, 16 MB
flash, 8 MB quad PSRAM, WiFi + BLE 5 LE), **CO5300** 368×448 AMOLED (QSPI),
**CST820** touch (I2C), **ES8311** audio codec, **AXP2101** PMIC, **PCF85063** RTC,
**QMI8658** IMU, microSD slot. The board is BLE-only (no Bluetooth Classic), which is
why "listen via Bluetooth" means relaying to a phone, not pairing headphones — see
`DESIGN_PLANS.md`.
