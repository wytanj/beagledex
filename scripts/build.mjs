#!/usr/bin/env node
/**
 * Compile a sketch with arduino-cli and print the resulting .bin path.
 *
 *   node scripts/build.mjs firmware/translator-p01
 *   node scripts/build.mjs firmware/translator-p01 --quiet   # path only, for piping
 *
 * The FQBN lives here rather than in your shell history because the board
 * options are not optional on this hardware:
 *   PSRAM=opi           8 MB in-package PSRAM; without it ps_malloc fails
 *   FlashSize=16M       matches the actual chip
 *   PartitionScheme     huge_app, whose app0 is 0x300000 — the same size as
 *                       this board's ota_0, so the compile-time size check is
 *                       the right one. The table arduino-cli generates from it
 *                       is never flashed: the real layout is
 *                       firmware/partitions-translator.csv, already on the
 *                       device. App images are offset-independent, so living
 *                       at 0x110000 rather than huge_app's 0x10000 is fine.
 *   CDCOnBoot=cdc       REQUIRED. Defaults to Disabled, which routes Serial to
 *                       the UART0 pins — nothing reaches USB and the board
 *                       looks dead over the very cable you flashed it with.
 *   USBMode=hwcdc       native USB-Serial/JTAG (the default, stated explicitly)
 */

import { spawn } from 'node:child_process'
import { readdir, stat, readFile, writeFile } from 'node:fs/promises'
import { existsSync } from 'node:fs'
import { join, resolve, basename, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { homedir } from 'node:os'
import process from 'node:process'

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..')

const FQBN = 'esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=huge_app'
           + ',USBMode=hwcdc,CDCOnBoot=cdc'

const args = process.argv.slice(2)
const quiet = args.includes('--quiet')
const sketchDir = resolve(args.find(a => !a.startsWith('--')) ?? 'firmware/translator-p01')

/** Prefer a local copy over PATH — that's how it got installed here. */
function findCli() {
  const candidates = [
    process.env.ARDUINO_CLI,
    join(homedir(), '.tools', 'arduino-cli.exe'),
    join(homedir(), '.tools', 'arduino-cli'),
  ].filter(Boolean)
  return candidates.find(p => existsSync(p)) ?? 'arduino-cli'
}

const CLI = findCli()

function run(cmd, argv) {
  return new Promise(resolve => {
    const p = spawn(cmd, argv, { stdio: ['ignore', 'pipe', 'pipe'] })
    let out = ''
    const tee = c => { const s = c.toString(); out += s; if (!quiet) process.stderr.write(s) }
    p.stdout.on('data', tee)
    p.stderr.on('data', tee)
    p.on('error', e => resolve({ code: -1, out: out + String(e) }))
    p.on('close', code => resolve({ code, out }))
  })
}

try {
  await stat(sketchDir)
} catch {
  console.error(`no such sketch directory: ${sketchDir}`)
  process.exit(1)
}

/* pin_config.h has exactly one source of truth: firmware/pin_config.h. Arduino
 * only compiles files that sit inside the sketch directory, so every sketch needs
 * its own copy — and hand-maintained copies of a reconstructed header are a drift
 * hazard, which is the last thing you want in the file that says which pin the
 * microphone is on. Copy it in before each compile, so the copies are build
 * outputs rather than duplicates anyone has to remember. */
async function syncPinConfig(dir) {
  const src = join(REPO, 'firmware', 'pin_config.h')
  const dst = join(dir, 'pin_config.h')
  if (!existsSync(src) || resolve(dst) === resolve(src)) return
  const want = await readFile(src, 'utf8')
  const have = existsSync(dst) ? await readFile(dst, 'utf8') : null
  if (want === have) return
  await writeFile(dst, want)
  if (!quiet) console.error(`  pin_config.h → ${basename(dir)}/ (synced from firmware/)`)
}

await syncPinConfig(sketchDir)

if (!quiet) console.error(`\n── compile ${basename(sketchDir)} ─────────────────────────────────\n  ${FQBN}\n`)

const buildDir = join(sketchDir, 'build')
const res = await run(CLI, ['compile', '--fqbn', FQBN, '--build-path', buildDir, sketchDir])

if (res.code !== 0) {
  console.error('\ncompile failed.')
  if (/platform not installed|Please install/i.test(res.out)) {
    console.error(`run:  "${CLI}" core install esp32:esp32`)
  }
  process.exit(1)
}

// arduino-cli names the artifact <sketch>.ino.bin inside the build path.
const wanted = `${basename(sketchDir)}.ino.bin`
const found = (await readdir(buildDir)).find(f => f === wanted)
if (!found) {
  console.error(`compiled, but ${wanted} is not in ${buildDir}`)
  process.exit(1)
}

const binPath = join(buildDir, found)
const { size } = await stat(binPath)
if (!quiet) console.error(`\n  ${(size / 1024).toFixed(1)} KB → ${binPath}\n`)

// stdout is the path alone, so this composes:  npm run flash -- --bin "$(node scripts/build.mjs … --quiet)"
console.log(binPath)
