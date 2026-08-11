/**
 * Render translated text to a bitmap the device can blit.
 *
 * The built-in GFX font is ASCII, so the device cannot draw Japanese, Chinese,
 * Arabic or Devanagari — the reply was spoken but not legible on screen. This
 * fixes that the same way the watch face works: the host produces pixels, the
 * device blits them. No font data and no text engine ship to the watch.
 *
 * Skia (via @napi-rs/canvas) is doing the real work, and it matters that it is
 * Skia rather than Pillow: it carries its own HarfBuzz, so Arabic joins and
 * shapes and Devanagari forms conjuncts correctly — the exact things Pillow could
 * not do here (its raqm support is absent). One renderer covers every script.
 *
 * Output is RGB565, green on black, so the device draws it with the same
 * draw16bitRGBBitmap path as the face — zero compositing on the device side.
 */
import { existsSync } from 'node:fs'
import { createCanvas, GlobalFonts } from '@napi-rs/canvas'

const FONT_DIR = 'C:/Windows/Fonts/'

/* One font per script family. These are what a stock Windows install ships, and
 * they were confirmed present before this file was written. If one is missing the
 * renderer falls back to Latin rather than throwing — a wrong-but-drawn glyph box
 * is better than a 500. */
const FAMILIES: Record<string, { family: string; file: string }> = {
  ja: { family: 'BDX-JA', file: 'YuGothM.ttc' },
  zh: { family: 'BDX-ZH', file: 'msyh.ttc' },
  nan: { family: 'BDX-TW', file: 'msjh.ttc' },   // Hokkien: Traditional Han (JhengHei)
  ko: { family: 'BDX-KO', file: 'malgun.ttf' },
  th: { family: 'BDX-TH', file: 'LeelawUI.ttf' },
  ta: { family: 'BDX-IN', file: 'Nirmala.ttc' },   // Nirmala covers Tamil + Devanagari
  hi: { family: 'BDX-IN', file: 'Nirmala.ttc' },
  ar: { family: 'BDX-AR', file: 'tahoma.ttf' },
  _:  { family: 'BDX-LA', file: 'segoeui.ttf' },   // Latin default (also Vietnamese, etc.)
}

const registered = new Set<string>()
function ensure(key: string): string {
  const spec = FAMILIES[key] ?? FAMILIES._
  if (registered.has(spec.family)) return spec.family
  const path = FONT_DIR + spec.file
  if (!existsSync(path)) return ensure('_')       // fall back to Latin
  GlobalFonts.registerFromPath(path, spec.family)
  registered.add(spec.family)
  return spec.family
}

/* Greedy wrap that works for both space-delimited scripts and CJK, which has no
 * spaces: break at the last space when there is one, otherwise mid-run (which is
 * correct for CJK and acceptable for a fallback). */
function wrap(ctx: any, text: string, maxW: number): string[] {
  const lines: string[] = []
  let line = ''
  for (const ch of text) {
    if (ch === '\n') { lines.push(line); line = ''; continue }
    const test = line + ch
    if (line && ctx.measureText(test).width > maxW) {
      const sp = line.lastIndexOf(' ')
      if (sp > 0) { lines.push(line.slice(0, sp)); line = line.slice(sp + 1) + ch }
      else { lines.push(line); line = ch }
    } else {
      line = test
    }
  }
  if (line) lines.push(line)
  return lines
}

export interface Rendered { rgb: Buffer; width: number; height: number }

export function renderText(
  text: string,
  lang: string,
  opts: { width?: number; maxH?: number; size?: number; color?: string } = {},
): Rendered {
  const width = opts.width ?? 344
  const maxH = opts.maxH ?? 156
  const size = opts.size ?? 30
  const pad = 4
  const lineH = Math.round(size * 1.35)
  const family = ensure(lang)
  const rtl = lang === 'ar'

  const meas = createCanvas(8, 8).getContext('2d')
  meas.font = `${size}px "${family}"`
  const lines = wrap(meas, text.trim(), width - 2 * pad)

  const height = Math.min(maxH, Math.max(lineH + pad * 2, pad * 2 + lines.length * lineH))
  const canvas = createCanvas(width, height)
  const ctx = canvas.getContext('2d')
  ctx.fillStyle = '#000000'
  ctx.fillRect(0, 0, width, height)
  ctx.fillStyle = opts.color ?? '#00e864'          // C_ACCENT, the console green
  ctx.font = `${size}px "${family}"`
  ctx.textBaseline = 'top'
  ctx.textAlign = rtl ? 'right' : 'left'

  let y = pad
  for (const ln of lines) {
    if (y > height - lineH) break                  // clip rather than overflow the strip
    ctx.fillText(ln, rtl ? width - pad : pad, y)
    y += lineH
  }

  // RGB565, little-endian — matches the device's draw16bitRGBBitmap, same as the face.
  const src = ctx.getImageData(0, 0, width, height).data
  const rgb = Buffer.allocUnsafe(width * height * 2)
  for (let i = 0, p = 0; i < src.length; i += 4, p += 2) {
    const v = ((src[i] & 0xF8) << 8) | ((src[i + 1] & 0xFC) << 3) | (src[i + 2] >> 3)
    rgb.writeUInt16LE(v, p)
  }
  return { rgb, width, height }
}
