/**
 * Text to speech, for the return leg. The device asked to be spoken to rather
 * than shown foreign text — which is the honest answer to "how do I display
 * Japanese on an ASCII font": you don't, you play it.
 *
 * xAI returns MP3 (it ignores every format hint), so this decodes it to the one
 * format the ES8311 already plays: 16 kHz, 16-bit, two slots. All of that happens
 * HERE, not on the device — same reason the API key and the image pipeline live
 * on the host. The watch receives PCM it can push straight to I2S and nothing
 * else. No decoder, no resampler, no working buffer on a device counting
 * milliamps.
 *
 * No storage is involved: the reply is synthesised, streamed, played once and
 * dropped. A microSD card would only let us CACHE repeats to skip the ~1 s
 * synthesis — a later optimisation, never a requirement.
 */
import { MPEGDecoder } from 'mpg123-decoder'

/* The device's I2S is opened STEREO with both slots, because that is what the
 * ES8311 capture path proved out. So the reply is emitted as 2-slot 16-bit with
 * the mono voice duplicated into both — the device writes it to I2S unchanged,
 * exactly as it plays back a capture. */
export const DEVICE_RATE = 16000

export interface Speech {
  pcm: Buffer          // 16 kHz, s16le, 2 interleaved slots
  seconds: number
}

export async function synthesize(text: string, lang: string, key: string, voice = 'eve'): Promise<Speech> {
  const res = await fetch('https://api.x.ai/v1/tts', {
    method: 'POST',
    headers: { Authorization: `Bearer ${key}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({ text, voice, language: lang }),
  })
  if (!res.ok) {
    const detail = (await res.text()).slice(0, 200)
    throw new Error(`tts ${res.status}: ${detail}`)
  }
  const mp3 = Buffer.from(await res.arrayBuffer())

  const dec = new MPEGDecoder()
  await dec.ready
  let channelData: Float32Array[], sampleRate: number, samplesDecoded: number
  try {
    ;({ channelData, sampleRate, samplesDecoded } = dec.decode(mp3))
  } finally {
    dec.free()
  }
  if (!samplesDecoded) throw new Error('tts decoded to zero samples')

  // Downmix to mono. xAI hands back stereo, but it is one voice in both channels.
  const n = samplesDecoded
  const mono = new Float32Array(n)
  if (channelData.length > 1) {
    for (let i = 0; i < n; i++) mono[i] = (channelData[0][i] + channelData[1][i]) * 0.5
  } else {
    mono.set(channelData[0].subarray(0, n))
  }

  /* Peak-normalise, but leave real headroom. Targeting near full scale (-0.3 dBFS)
   * made speech buzz: this drives a tiny speaker through a fixed-gain class-D amp,
   * and speech has a high peak-to-average ratio, so the loud peaks overdrive the
   * speaker mechanically long before digital clipping. -6 dBFS keeps the peaks
   * clean while still lifting xAI's quiet ~-10 dBFS output. Tune with TTS_PEAK
   * (0..1) without a rebuild — the dev server hot-reloads this file. */
  const target = Math.min(Math.max(Number(process.env.TTS_PEAK ?? 0.5), 0.05), 0.95)
  let peak = 0
  for (let i = 0; i < n; i++) { const a = Math.abs(mono[i]); if (a > peak) peak = a }
  const norm = peak > 0.01 ? Math.min(target / peak, 8) : 1   // at most +18 dB, so silence stays silent

  // Resample whatever xAI emitted (24 kHz observed) to the device's 16 kHz, and
  // interleave into two slots as we go. Linear interpolation is plenty for speech.
  const outN = Math.max(1, Math.floor((n * DEVICE_RATE) / sampleRate))
  const pcm = Buffer.allocUnsafe(outN * 4)          // 2 slots * 2 bytes
  const step = sampleRate / DEVICE_RATE
  for (let i = 0; i < outN; i++) {
    const src = i * step
    const i0 = Math.floor(src)
    const frac = src - i0
    const a = mono[i0] ?? 0
    const b = i0 + 1 < n ? mono[i0 + 1] : a
    let v = Math.round((a * (1 - frac) + b * frac) * norm * 32767)
    if (v > 32767) v = 32767
    else if (v < -32768) v = -32768
    pcm.writeInt16LE(v, i * 4)
    pcm.writeInt16LE(v, i * 4 + 2)
  }

  return { pcm, seconds: +(outN / DEVICE_RATE).toFixed(2) }
}
