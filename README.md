# translator·console

Fleet console for the ESP32-S3 voice translator. Tracks which firmware, with which
features, went to which device — and keeps the device logs next to it.

```bash
cp .env.example .env      # set DEVICE_TOKEN
npm install
npm run dev               # → http://localhost:3000
```

## What it's for

A flash history maintained by hand is a flash history that's wrong. So the record is
written by the thing that actually moved the bytes: `scripts/flash.mjs` wraps `esptool`,
and posts the device identity, the build, and the outcome as it goes. OTA updates post to
the same endpoints, so USB and over-the-air land in one timeline.

Three views:

| Page | Answers |
|---|---|
| `/` | What's online, what firmware each device runs, what was flashed recently, what's erroring |
| `/devices/:id` | Full flash history for one device, its feature state, and a live log tail |
| `/builds` | Which build ships which feature — the matrix you check before testing something |

## Flashing

```bash
# identity only — registers the device, writes nothing
npm run flash -- --port COM3

# real flash
npm run flash -- --port COM3 \
                 --bin firmware/translator-p01/build/translator-p01.ino.bin \
                 --version 0.1.0 --features audio.loopback,audio.ptt
```

Two safety rails, both from this board's actual partition table:

- **Default write address is `0x110000`**, the `ota_0` app slot — *not* the conventional
  `0x10000`, which on this layout lands inside `nvs`.
- **`--erase-all` and friends are refused.** `nvsfactory` at `0x9000` holds per-unit
  calibration that exists in no vendor download. Flash individual partitions.

The script uses `--no-stub` by default: the stub flasher desyncs on this board's native
USB-Serial/JTAG bridge (`Packet content transfer stopped` around 16 KB). `--stub` opts
back in if you're on an external UART adapter.

## Watching the device

The firmware only posts to the console when `secrets.h` is present, and phase 3
networking isn't built yet. Until it is, bridge the serial line into the store:

```bash
npm run monitor           # just watch — writes nothing
npm run log               # watch and write log records to .data/db
```

`scripts/serial-log.py` writes the same records the HTTP sink writes, so
`/devices/:id` tails them using the 4-second refresh it already has, and `lastSeen`
stays honest between flashes. Nothing has to be online for that — no WiFi on the
device, no web server on the host. The store is plain files, so the bridge writes
files; run `npm run dev` only when you want to look at them.

Firmware that logs as `[info/tag] message` is classified by level and tag. Anything
else lands as `info`/`serial`, which is the fallback, not the goal.

## Features

`server/utils/features.ts` is the single source of truth, with each feature tagged by
build-plan phase — so the fleet view doubles as a progress read. Unknown ids are dropped
on write, so a typo in a flash command can't invent a phantom feature.

A build declares what it **ships**; a device reports what it has **active** at boot. The
device page diffs them. Shipped-but-not-active is the interesting case — it usually means
the image went on but the feature failed to initialise.

## API

Device-facing routes require `Authorization: Bearer $DEVICE_TOKEN`. If `DEVICE_TOKEN` is
unset they run unauthenticated and log a warning — fine on localhost, never in a deploy.

| Route | Caller | Purpose |
|---|---|---|
| `POST /api/devices/register` | firmware, boot | Idempotent. Preserves the name and notes you typed. |
| `POST /api/devices/:id/heartbeat` | firmware, ~60 s | Battery, RSSI, uptime, active features |
| `POST /api/logs` | firmware | One line or a batch. Batch it. |
| `GET /api/ota/check` | firmware | Manifest; `204` when already current |
| `POST /api/translate` | firmware | **501 for now** — contract is fixed, providers aren't wired |
| `POST /api/builds` · `POST /api/flashes` | `flash.mjs`, CI | Records |
| `GET /api/devices` · `/builds` · `/flashes` · `/logs` · `/features` | UI | Reads |

`/api/translate` deliberately returns 501 rather than a plausible fake, so firmware work
can't be validated against a stub by accident.

## Settings live in git, not a database

`config/devices/<id>.json` is the source of truth for what a device *should* be:
brightness, sleep timeouts, watch face layout, which apps are enabled. It is tracked,
so `git log` is the audit trail and a bad setting is one `git revert` away. There is
deliberately no database for this.

The split that makes it work:

| | Where | Tracked |
|---|---|---|
| **Intent** — settings, face layout, enabled apps | `config/` | yes |
| **Observation** — flash history, logs, heartbeats | `.data/db` | no |

Configuration is something you decide and want versioned. History is something that
happened on one bench, and belongs to that machine. Conflating them is what makes
people want a database they do not need.

Consequence worth knowing: when the dashboard writes a setting it dirties the working
tree, so changes want committing. That is the feature — settings are diffable — but it
does mean this arrangement suits a local-first console rather than a deployed one.

## Storage

Nitro's own storage layer over plain files in `.data/db` — no native modules, nothing to
compile. Every call in `server/utils/store.ts` is driver-agnostic, so deploying to Vercel
(ephemeral filesystem) means changing one line in `nuxt.config.ts` to `vercel-kv` or
`upstash` and nothing else.

Fine for a fleet of tens. If you reach hundreds of devices or want log retention and
querying, move logs to a real time-series store — the fs driver rewrites a file per key.

## Not built yet

- **OTA binary hosting.** `/api/ota/check` returns the manifest with `url: null`. Serve
  signed images from blob storage; don't stream multi-MB binaries out of a serverless
  function. The device side of this is ready — the board now carries a clean A/B
  partition table (`firmware/partitions-translator.csv`), so there are two 3 MB app
  slots to swap between.
- **Per-device tokens.** One shared secret today. Per-device credentials (ideally the
  S3's `esp_ds` peripheral, which signs with a key firmware can't read) come with fleet
  scale.
- **Staged rollout and version pinning.** The schema has `channel` and the OTA route
  already prefers a device's pin; the UI to set either isn't there.
- **Auth on the console itself.** The read routes are open. Put it behind access control
  before it leaves your machine.
