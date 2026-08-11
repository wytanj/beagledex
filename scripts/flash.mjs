#!/usr/bin/env node
/**
 * Flash a device and record it, in one command.
 *
 *   npm run flash -- --port COM3 --bin firmware/app.bin \
 *                    --version 0.1.0 --features audio.loopback,audio.ptt
 *
 * Why this exists: a flash history that has to be maintained by hand is a flash
 * history that is wrong. This wraps esptool, so the record is written by the
 * thing that actually moved the bytes.
 *
 * What it does:
 *   1. reads chip identity over USB  → device id from the MAC
 *   2. registers the device and the build with the console
 *   3. runs esptool write-flash
 *   4. records the result — including failures, with the error text
 *
 * Safety rails, both derived from this board's real partition table:
 *   · defaults to 0x110000, the `ota_0` app slot — NOT the conventional
 *     0x10000, which on this layout lands inside nvs and bricks the app
 *   · refuses to erase the whole chip: `nvsfactory` at 0x9000 holds per-unit
 *     calibration that exists in no download
 */

import { spawn } from 'node:child_process'
import { readFile, stat } from 'node:fs/promises'
import { createHash } from 'node:crypto'
import process from 'node:process'

/* ── args ─────────────────────────────────────────────────────────────────── */

function parseArgs(argv) {
  const out = {}
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    if (!a.startsWith('--')) continue
    const key = a.slice(2)
    const next = argv[i + 1]
    if (next && !next.startsWith('--')) { out[key] = next; i++ }
    else out[key] = true
  }
  return out
}

const args = parseArgs(process.argv.slice(2))

if (args.help || !args.port) {
  console.log(`
flash.mjs — flash a device and record the event

  --port <COMn>        serial port                                  (required)
  --bin <path>         image to write; omit for a dry identity-only run
  --addr <hex>         flash offset                          (default 0x110000)
  --version <semver>   build version                            (default 0.0.0)
  --features <a,b,c>   feature ids this build ships
  --channel <c>        dev | beta | stable                        (default dev)
  --notes <text>       free-text note on the build
  --console <url>      console base url          (default http://localhost:3000)
  --token <tok>        DEVICE_TOKEN, if the console requires one
  --esptool <cmd>      how to invoke esptool     (default: python -m esptool)
  --stub               use the stub flasher (faster; unreliable on USB-JTAG here)

Examples
  # identity only — registers the device, writes nothing
  npm run flash -- --port COM3

  # flash a built image into the app slot (ota_0)
  npm run flash -- --port COM3 \\
                   --bin firmware/translator-p01/build/translator-p01.ino.bin \\
                   --version 0.1.0 --features audio.loopback,audio.ptt
`)
  process.exit(args.port ? 0 : 1)
}

/* Load .env ourselves rather than relying on the shell having sourced it. The
 * build and flash routes are token-guarded, and a flash whose record silently
 * 401s is precisely the drift this console exists to prevent — the history would
 * quietly stop matching what actually happened, which is worse than no history.
 * Anything already in the environment wins, so `DEVICE_TOKEN=x npm run flash`
 * still overrides. */
try {
  const text = await readFile(new URL('../.env', import.meta.url), 'utf8')
  for (const line of text.split('\n')) {
    const t = line.trim()
    if (!t || t.startsWith('#')) continue
    const eq = t.indexOf('=')
    if (eq < 1) continue
    const k = t.slice(0, eq).trim()
    if (!process.env[k]) process.env[k] = t.slice(eq + 1).trim()
  }
} catch { /* no .env is fine — the routes warn loudly when unauthenticated */ }

const PORT      = args.port
const ADDR      = args.addr ?? '0x110000'
const VERSION   = args.version ?? '0.0.0'
const CHANNEL   = args.channel ?? 'dev'
const CONSOLE   = (args.console ?? process.env.CONSOLE_URL ?? 'http://localhost:3000').replace(/\/$/, '')
const TOKEN     = args.token ?? process.env.DEVICE_TOKEN ?? ''
const FEATURES  = String(args.features ?? '').split(',').map(s => s.trim()).filter(Boolean)
const ESPTOOL   = (args.esptool ?? 'python -m esptool').split(' ')
const NO_STUB   = !args.stub

/* Offsets worth naming, from firmware/partitions-translator.csv, so a flash
 * record says which partition was written instead of asserting one. An
 * unrecognised address is recorded bare rather than mislabelled. */
const PARTITIONS = {
  '0x8000':   'partition table',
  '0x110000': 'ota_0',
  '0x410000': 'ota_1',
  '0x710000': 'storage',
}
const PART_NAME = PARTITIONS[ADDR.toLowerCase()]

for (const bad of ['erase-all', 'erase_all', 'erase-flash', 'erase']) {
  if (args[bad]) {
    console.error(`refusing --${bad}: nvsfactory at 0x9000 holds per-unit calibration`)
    console.error('that is not in any vendor download. Flash individual partitions instead.')
    process.exit(1)
  }
}

/* ── helpers ──────────────────────────────────────────────────────────────── */

function run(cmd, argv) {
  return new Promise(resolve => {
    const p = spawn(cmd, argv, { stdio: ['ignore', 'pipe', 'pipe'], shell: false })
    let out = ''
    const tee = chunk => { const s = chunk.toString(); out += s; process.stdout.write(s) }
    p.stdout.on('data', tee)
    p.stderr.on('data', tee)
    p.on('error', err => resolve({ code: -1, out: out + String(err) }))
    p.on('close', code => resolve({ code, out }))
  })
}

const esptool = extra => run(ESPTOOL[0], [
  ...ESPTOOL.slice(1), '--port', PORT, ...(NO_STUB ? ['--no-stub'] : []), ...extra,
])

async function api(path, body) {
  const headers = { 'content-type': 'application/json' }
  if (TOKEN) headers.authorization = `Bearer ${TOKEN}`
  try {
    const res = await fetch(`${CONSOLE}${path}`, {
      method: 'POST', headers, body: JSON.stringify(body),
    })
    const text = await res.text()
    if (!res.ok) {
      console.warn(`  ! ${path} → ${res.status} ${text.slice(0, 200)}`)
      return null
    }
    return text ? JSON.parse(text) : {}
  } catch (err) {
    console.warn(`  ! console unreachable at ${CONSOLE} (${err.message})`)
    console.warn('    flash still proceeds; the record just won\'t be written.')
    return null
  }
}

const FLASH_SIZES = { '1MB': 1, '2MB': 2, '4MB': 4, '8MB': 8, '16MB': 16, '32MB': 32 }

function parseIdentity(text) {
  const grab = re => text.match(re)?.[1]?.trim()
  const sizeLabel = grab(/Detected flash size:\s*(\S+)/)
  const psramMb = Number(grab(/Embedded PSRAM (\d+)MB/) ?? 0)
  return {
    mac: grab(/MAC:\s*([0-9a-f:]{17})/i),
    chip: grab(/Chip type:\s*([^(\n]+)/),
    revision: grab(/revision (v[\d.]+)/),
    flashBytes: sizeLabel && FLASH_SIZES[sizeLabel] ? FLASH_SIZES[sizeLabel] * 1024 ** 2 : undefined,
    psramBytes: psramMb ? psramMb * 1024 ** 2 : undefined,
  }
}

/* ── 1. identify ──────────────────────────────────────────────────────────── */

console.log(`\n── identify ${PORT} ────────────────────────────────────────────`)
const idRun = await esptool(['flash-id'])
if (idRun.code !== 0) {
  console.error('\ncould not talk to the chip. Is the port right, and is anything else holding it open?')
  process.exit(1)
}

const ident = parseIdentity(idRun.out)
if (!ident.mac) {
  console.error('\ncould not parse a MAC out of esptool output; aborting rather than guess.')
  process.exit(1)
}
const deviceId = ident.mac.replace(/[^0-9a-f]/gi, '').toLowerCase()
console.log(`\n  device ${deviceId}  (${ident.chip ?? '?'} ${ident.revision ?? ''})`)

/* ── 2. register device + build ────────────────────────────────────────────── */

console.log(`\n── register with ${CONSOLE} ───────────────────────────────────`)
await api('/api/devices/register', {
  mac: ident.mac,
  chip: ident.chip,
  revision: ident.revision,
  flashBytes: ident.flashBytes,
  psramBytes: ident.psramBytes,
  fwVersion: args.bin ? VERSION : undefined,
})
console.log(`  device ${deviceId} registered`)

let buildId
if (args.bin) {
  const buf = await readFile(args.bin)
  const sha256 = createHash('sha256').update(buf).digest('hex')
  const { size } = await stat(args.bin)
  const built = await api('/api/builds', {
    version: VERSION, channel: CHANNEL, features: FEATURES,
    sizeBytes: size, sha256, notes: typeof args.notes === 'string' ? args.notes : undefined,
  })
  buildId = built?.build?.id
  console.log(`  build ${VERSION} (${CHANNEL}) ${bytesHuman(size)} sha256=${sha256.slice(0, 12)}…`)
  if (FEATURES.length) console.log(`  features: ${FEATURES.join(', ')}`)
  else console.log('  features: none declared — pass --features so the matrix means something')
}

function bytesHuman(n) {
  return n < 1024 ** 2 ? `${(n / 1024).toFixed(1)} KB` : `${(n / 1024 ** 2).toFixed(2)} MB`
}

if (!args.bin) {
  console.log('\nno --bin given; identity registered, nothing written.\n')
  process.exit(0)
}

/* Refuse to record a version the firmware does not agree with.
 *
 * This has now bitten twice: --version said 0.6.1 and 0.9.0 while FW_VERSION in
 * the sketch still said 0.6.0 and 0.8.0, so the build record and the running
 * firmware disagreed. The console caught it both times via the shipped-vs-active
 * diff, which is the diff working — but a flash history that CAN be wrong
 * eventually is, and this whole project rests on it not being.
 *
 * The bin path is firmware/<sketch>/build/<sketch>.ino.bin, so the source sits
 * next to it. --force-version overrides for the rare deliberate mismatch.
 */
{
  const m = /^(.*[\\/])build[\\/]([^\\/]+)\.ino\.bin$/.exec(args.bin.replace(/\\/g, '/'))
  if (m && !args['force-version']) {
    try {
      const src = await readFile(`${m[1]}${m[2]}.ino`, 'utf8')
      const declared = /FW_VERSION\s*=\s*"([^"]+)"/.exec(src)?.[1]
      if (declared && declared !== VERSION) {
        console.error(`\nrefusing: --version ${VERSION} but the sketch declares FW_VERSION "${declared}".`)
        console.error('The device would report one version while the record claimed another.')
        console.error(`Fix the sketch, or pass --version ${declared}, or --force-version to override.\n`)
        process.exit(1)
      }
    } catch { /* no sketch beside the bin — nothing to check against */ }
  }
}

/* ── 3. flash ─────────────────────────────────────────────────────────────── */

console.log(`\n── write ${args.bin} → ${ADDR} ────────────────────────────────`)
if (ADDR === '0x10000') {
  console.warn('  ! 0x10000 is inside nvs on this board. The app slot is 0x110000.')
}

const started = Date.now()
const wrote = await esptool(['write-flash', ADDR, args.bin])
const durationMs = Date.now() - started

/* ── 4. record ────────────────────────────────────────────────────────────── */

const ok = wrote.code === 0
const errLine = ok
  ? undefined
  : (wrote.out.match(/A fatal error occurred:.*/)?.[0] ?? `esptool exited ${wrote.code}`).slice(0, 300)

await api('/api/flashes', {
  deviceId, buildId, version: VERSION,
  method: 'usb',
  result: ok ? 'ok' : 'failed',
  target: PART_NAME ? `${ADDR} (${PART_NAME})` : ADDR,
  durationMs, error: errLine,
})

console.log(`\n── ${ok ? 'flashed' : 'FAILED'} in ${(durationMs / 1000).toFixed(1)}s ──────────────────────────────`)
if (!ok) console.error(`  ${errLine}`)
console.log(`  recorded → ${CONSOLE}/devices/${deviceId}\n`)

// Set exitCode rather than calling process.exit(): an immediate exit here tears
// down stdio while the piped esptool output is still draining, which aborts the
// process with a libuv assertion instead of a clean status.
process.exitCode = ok ? 0 : 1
