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

### Build order when resumed

1. `/api/agent`: STT → forward text to the agent/MCP endpoint → condense with the
   light model → TTS. Mirror `/api/translate`; reuse `tts.ts`.
2. Agent app in the registry: PTT, show the transcript, speak the reply. ~one
   screen, copied from the Translate app.
3. Confirmation UX for anything that acts, before exposing write-capable tools.
4. BLE relay (phase 6) only if private/phone listening is actually wanted; the
   speaker path needs none of it.

### Naming

Product is now **Beagledex**. The repo is already `beagledex`; the firmware sketch
is still `firmware/watch/`. Renaming the sketch dir churns build paths and the
version-guard (which reads the `.ino` beside the bin), so defer it to a deliberate
rename rather than doing it mid-feature.
