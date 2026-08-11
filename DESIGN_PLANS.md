# Design plans

Shelved designs and forward direction for **Beagledex** (the ESP32-S3 device;
the repo and product name). Not yet built — TODO.md tracks active work.

---

## 1. Silicone strap / necklace (shelved)

### The framing

Beagledex is **not a watch mechanically** — it's a lug-less puck, 37.6 × 45.2 ×
15 mm, no spring-bar ears. And it's a **translator/voice device**, which adds a
constraint most strap projects don't have:

> **Rule zero: the MIC and SPK openings must stay completely clear.** Muffle
> either and you break the one thing the device exists to do. Above comfort or
> looks.

So a "strap" is really a **cradle** holding the puck, routing a band, with
precise cutouts for MIC, SPK, USB-C, BOOT, PWR and the SD slot.

### Dimensions (from the Waveshare drawing)

| Feature | Dimension | Cradle design |
|---|---|---|
| Body | 37.6 × 45.2 × 15 mm | cavity 37.9 × 45.5 × 15.2 (0.15–0.3 mm clearance) |
| Glass (active) | 28.7 × 34.94 mm | screen window, fully clear |
| Bezel margin | 4.45 mm sides, 5.13 mm top/bottom | room for a 2.5 mm front lip that never touches glass |
| Back plate | 27.6 × 27.6, corners R1.8 | narrower than the front — the taper aids retention |
| Wall | — | 2.0–2.5 mm |

### Recommended build (fastest to a good result)

**3D-printed TPU (95A) cradle + an off-the-shelf 20 mm silicone watch strap.**
TPU prints on any FDM printer, feels rubbery like silicone, needs no molding, and
generic 20 mm straps come in both wrist and lanyard lengths — so **one cradle
does wristwatch and necklace** by swapping the band.

True cast silicone (two-part printed mold, platinum-cure, Shore A 40–60) is
prettier and waterproof but is tier 2: iterate the cutouts in TPU first (reprint
= minutes; recast = hours), then cast once they're right.

### Cradle spec

- **Retention**: 2.5 mm front lip all round (half the bezel is still clear glass);
  back taper stops it pushing through backward. Lip ~0.5 mm proud of the glass so
  the screen survives face-down.
- **Band attachment**: NATO pass-through (two 22 × 3 mm slots across the back,
  strap threads behind — traps the device, dual-use) *or* 20 mm spring-bar lugs
  (more watch-like, reuses off-the-shelf hardware). NATO first — more forgiving,
  better for a necklace.
- **Cutouts**: USB-C ~10 × 5 mm; BOOT + PWR as open holes or press-through bumps;
  SD as a slot **plus** an insertion channel; SPK + MIC as open grilles, never
  bridged by material.

### Gotchas

- **Thermal** — WiFi + amp warm the puck; don't fully seal it, leave the back or a
  vent open.
- **Necklace weight** — a chunky 15 mm puck flips on a thin cord; use a wider band.
- **IMU orientation** — `imu.wake` (wrist-raise) will care which axis is "up"; fix
  an orientation and keep it.
- **Sweat + USB-C** — a port flap protects the pins on a worn device.

### Deliverable when resumed

A parametric OpenSCAD cradle: all the dimensions above as variables, cutouts
placed, NATO slots or 20 mm lugs selectable. Render + print, or print as a casting
mold for tier 2.

---

## 2. Agent app — Beagledex as a voice remote for a backend agent

### The idea (from the user)

Speak to Beagledex → a **permissioned, harnessed backend agent** (MCP server /
exposed agent endpoint) does the real work on the user's infrastructure → the
result comes back and Beagledex speaks or shows it. The device stays a thin client
("token transmitter + UI layer"); the backend holds the capability and the risk.

### Why this is a small addition, not a new architecture

The shell was built for exactly this. Push-to-talk into a model is *the* primitive;
apps differ only in **which endpoint they hit and how they render the reply**. The
Agent app is the Translate app pointed at a different endpoint:

```
capture (shell) → POST audio → [console] → STT → agent/MCP → condense → TTS → play
```

`/api/translate` already proves every link. The Agent app needs a sibling
`/api/agent` and a new registry entry. That's it on the device.

### Where the pieces live

- **Console is the orchestrator**, same reasoning as the xAI key: it holds the
  agent connection and credentials, runs STT/TTS, and speaks to the MCP/agent so
  the device never has to. The device only POSTs audio and plays/shows a reply.
- **MCP stays server-side entirely.** The device has no business holding tool
  connections or the agent loop; the console (or the user's exposed agent) runs it.
- **The console→agent hop needs its own auth**, separate from the DEVICE_TOKEN
  that gates the console. A voice remote to a permissioned backend is powerful —
  destructive/outward actions should require explicit confirmation, and the
  harness/permissioning on the agent side is what keeps it safe. Design the
  confirmation UX before wiring anything that can act.

### The "light model" role

The user's instinct is right: agent output is often verbose or structured (JSON,
logs, multi-paragraph). Speaking it raw is slow and grating. A **cheap, fast model
condenses the agent's output into one speakable sentence** before TTS —
"summarise this result for reading aloud in one sentence."

No new provider needed: `grok-4.20-0309-non-reasoning` (already used for
translation, already on the key, ~1 s) fits this exactly. Truly local/tiny models
are a much bigger lift and not worth it while WiFi is present.

### "Listen via BLE" — the honest constraint

This one has a hard silicon limit worth knowing before designing around it:

- **The ESP32-S3 is Bluetooth 5 LE only — no Bluetooth Classic, so no A2DP.** It
  **cannot** pair with ordinary Bluetooth headphones for audio. That's the chip,
  not the firmware.
- Realistic paths to private/remote listening:
  1. **BLE GATT → companion phone app**, phone plays the audio (to its speaker or
     its own BT headphones). This makes BLE a relay to the phone — consistent with
     the existing phase-6 `net.ble` plan.
  2. **LE Audio (LC3)** to LE-Audio earbuds — emerging, poor Arduino support today,
     not practical yet.
  3. **The device's own speaker** already plays replies with no BLE at all; BLE
     only buys privacy.
- So "listen via BLE" realistically means **stream to a phone that does the
  playing** — the phone as the listening device, which also fits "phone does the
  heavy lifting."

### The "out and about" architecture: phone as a local brain (walkie-talkie)

When away from home the console (a home PC) is not reachable unless it is
internet-exposed — a real gap in the WiFi-hotspot path. The better answer pairs
Beagledex to the user's phone (Oppo Find N3) over BLE and lets the *phone* be the
orchestrator: Beagledex is mic + push-to-talk + screen; the phone does the work.

    Beagledex  --BLE GATT-->  Oppo app  --cellular-->  MCP / agent endpoint
       PTT + mic                STT (Android, free)
       + screen                 TTS (Android, free) --> user's earbuds
                                calls the endpoint directly

Why this is the right shape for heavy on-the-go use:

- **Cost → near zero.** Android's built-in SpeechRecognizer and TextToSpeech run
  on-device for free, so STT and TTS leave the bill entirely. Only the call to the
  user's own MCP/agent endpoint remains — over their own cellular to their own
  infra.
- **Battery, both devices.** BLE draws roughly an eighth of WiFi on Beagledex, and
  there is no phone-hotspot AP radio burning on the phone. This is the real battery
  win, not a hotspot.
- **Privacy.** Voicing backend/infra commands no longer ships your ops chatter to a
  third-party STT/LLM — STT/TTS stay on the phone, and the command goes only to your
  own endpoint.
- **Fixes reachability.** The phone is always with you and always has the uplink, so
  there is no "expose the home console to the internet" problem.

The hard constraints, stated honestly:

- **It needs a companion Android app.** This is the single biggest build in the
  project and a new platform — everything else is firmware + a Node console. That is
  the whole cost of this route.
- **BLE only (no Classic).** So it is a custom GATT link, not a standard headset
  profile. Raw 16 kHz PCM is 256 kbit/s — feasible as a *burst* after PTT release
  (a few hundred KB, which BLE 5 handles), but real-time bidirectional streaming is
  where it gets hard. Compress (ADPCM ~4:1) if streaming is ever needed.
- **Avoid audio-return over BLE.** The clean design has the **phone speak the reply
  to the user's earbuds**, not send audio back to Beagledex — that sidesteps the
  harder BLE direction and gives private listening for free. Beagledex still shows
  the rendered text on screen; the phone handles sound.

Firmware side: add a NimBLE GATT server as an alternate transport alongside WiFi —
BLE-to-phone when out, WiFi when home, same PTT and same device UI. The device stays
dumb either way.

Sequencing: this is a multi-session, new-platform effort. Prove the value first with
what exists (hotspot + console-local STT/TTS), and commit to the app only when
on-the-go is genuinely the primary use. Do not start the mobile app without an
explicit decision to take on mobile development.

### Cost levers (console-side, device untouched)

Independent of transport, because the device is dumb and every lever is a console
or phone change:

1. **Local STT + local TTS** on whatever host is the brain — `faster-whisper` +
   `Piper` on the PC console, or Android's built-in STT/TTS on the phone. Removes
   two of the three cloud costs and keeps voice in-house. Biggest lever.
2. **Intent matching before the LLM.** A finite command set can be matched on the
   host with fuzzy rules; the LLM is only the fallback for novel/ambiguous input, so
   most commands cost no LLM call at all — and are faster and more predictable.
3. **Prompt-cache the MCP tool definitions.** The constant system prompt + tool
   schemas cache; you pay only the variable part. xAI already reports cached_tokens.
4. **Trim the audio.** STT is priced per second — voice-activity-trim silence and cap
   capture length. Free savings, also helps latency and battery.
5. **Cache repeated replies** (SD card, hashed by text). "Done" / "all healthy"
   replay at zero cost.

### Provider adapter (built for translate; extend to the agent)

`/api/translate` is now provider-agnostic: `?provider=grok|gemini` selects the
understanding engine, behind one `Understanding` shape in `server/utils/understand.ts`.
The Translate app toggles it live (tap the status band) so real-world latency can be
compared on the actual device — that was the point. TTS stays one implementation for
both, so a head-to-head isolates the understanding leg.

- **grok (xAI)**: two calls — STT then chat. Proven.
- **gemini**: one call — Gemini takes audio natively, so transcribe + detect +
  translate happen together. Cheaper per unit (audio in ~$0.30/M on Flash-Lite vs
  xAI STT $0.10/hr *plus* a chat call) and a round trip shorter. Written to the
  documented `generateContent` format; **verify once a GEMINI_API_KEY exists** — it
  fails cleanly with an X-Error until then.

Why this matters beyond translate: the agent app should reuse the same adapter. And
Gemini's **function calling** fits voice-command routing unusually well — audio →
structured tool call to the MCP in a single call. Note the free-tier caveat:
free-tier "context [is] used to improve our products", so for backend/ops commands
use the paid tier or keep STT local. Translation on free tier is a fair trade;
infra commands are not.

### Build order when resumed

1. `/api/agent`: STT → forward text to the agent/MCP endpoint → condense with the
   light model → TTS. Mirror `/api/translate`; reuse `tts.ts`.
2. Agent app in the registry: PTT, show the transcript, speak the reply. ~one
   screen, copied from the Translate app.
3. Confirmation UX for anything that acts, before exposing write-capable tools.
4. Console-local STT/TTS + intent matching — the cost levers above, all host-side.
5. BLE + phone-brain relay (the walkie-talkie) — the on-the-go endgame, gated on a
   decision to build a mobile app.

### Naming

Product is now **Beagledex**. The repo is already `beagledex`; the firmware sketch
is still `firmware/watch/`. Renaming the sketch dir churns build paths and the
version-guard (which reads the `.ino` beside the bin), so defer it to a deliberate
rename rather than doing it mid-feature.
