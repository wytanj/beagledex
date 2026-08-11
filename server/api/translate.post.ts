/**
 * The product endpoint. Speech in, transcript + translation out (spoken + drawn).
 *
 *   POST /api/translate?pair=en-ja[&channels=2][&speak=1][&provider=grok|gemini]
 *     body  raw 16 kHz 16-bit little-endian PCM  (or an audio container, for host tests)
 *   →  speak: body = [text bitmap][pcm], words in X-Transcript/X-Translation headers
 *      else:  JSON { transcript, translation, detected, target, pair, seconds, ms }
 *
 * The credential and all the work live here, never on the device — a chip dump is
 * a six-second job, so firmware is no place for a key. The device transmits audio
 * and renders what comes back.
 *
 * ?provider selects the understanding engine (grok = xAI, gemini = Google native
 * audio). It exists to measure real-world latency between them; the choice is a
 * pure host concern because the device never sees it. TTS is the same either way,
 * so a head-to-head isolates the understanding leg.
 */
import { understandXai, understandGemini, type Audio } from '../utils/understand'

/** Keep only the left slot of an interleaved 2-slot frame — the microphone. */
function leftChannel(buf: Buffer): Buffer {
  const frames = Math.floor(buf.length / 4)
  const out = Buffer.allocUnsafe(frames * 2)
  for (let i = 0; i < frames; i++) out.writeInt16LE(buf.readInt16LE(i * 4), i * 2)
  return out
}

export default defineEventHandler(async (event) => {
  requireDevice(event)

  const cfg = useRuntimeConfig(event) as any
  const q = getQuery(event) as Record<string, string>
  const pair = q.pair ?? 'en-ja'
  const [fromCode = 'en', toCode = 'ja'] = pair.split('-')
  const autoDirect = (q.auto ?? '1') !== '0'
  const speak = (q.speak ?? '0') === '1'
  const provider = (q.provider ?? 'grok').toLowerCase()   // grok | gemini
  const useGemini = provider === 'gemini'

  const raw = (await readRawBody(event, false)) as Buffer | undefined
  if (!raw?.length) throw createError({ statusCode: 400, statusMessage: 'empty audio body' })

  // The device sends raw PCM. A container file is accepted for host-side testing.
  const ctype = (getRequestHeader(event, 'content-type') ?? '').toLowerCase()
  const container = /audio\/(mpeg|mp3|wav|x-wav|ogg|opus|flac|aac|mp4|m4a|x-matroska)/.exec(ctype)
  const mono = !container && Number(q.channels ?? 1) === 2 ? leftChannel(raw) : raw
  const audio: Audio = container
    ? { bytes: raw, isPcm: false, mime: ctype, ext: container[1] === 'mpeg' ? 'mp3' : container[1] }
    : { bytes: mono, isPcm: true, mime: 'audio/wav', ext: 'pcm' }
  const pcmSeconds = container ? 0 : +(mono.length / 32000).toFixed(2)

  const xaiKey = cfg.xaiApiKey || process.env.XAI_API_KEY
  // Accept GEMINI_KEY as well as the documented GEMINI_API_KEY — forgiving beats a
  // silent "why is it still 501".
  const geminiKey = cfg.geminiApiKey || process.env.GEMINI_API_KEY || process.env.GEMINI_KEY

  /* One clear failure path for a missing key or a provider error, surfaced to the
   * device in an X-Error header it can show — far better than the old "SILENT",
   * which made a missing key look like a dead microphone. */
  const fail = (msg: string, code = 502) => {
    console.error(`[translate] ${provider}: ${msg}`)
    setHeader(event, 'X-Error', encodeURIComponent(msg.slice(0, 120)))
    setHeader(event, 'X-Audio-Format', 'none')
    setResponseStatus(event, code)
    return { error: msg, provider }
  }

  const key = useGemini ? geminiKey : xaiKey
  if (!key) return fail(`${provider.toUpperCase()}_API_KEY is not set`, 501)

  let u
  try {
    u = useGemini
      ? await understandGemini(audio, { fromCode, toCode, autoDirect, key })
      : await understandXai(audio, { fromCode, toCode, autoDirect, key })
  } catch (e: any) {
    return fail(String(e?.message ?? e))
  }

  const seconds = u.seconds || pcmSeconds
  const { transcript, translation, detected, target } = u

  setHeader(event, 'X-Transcript', encodeURIComponent(transcript))
  setHeader(event, 'X-Translation', encodeURIComponent(translation))
  setHeader(event, 'X-Detected', detected)
  setHeader(event, 'X-Target', target)
  setHeader(event, 'X-Provider', provider)

  const jsonBody = () => ({
    transcript, translation, detected, target, provider, pair, seconds,
    ms: { stt: u.msStt, chat: u.msChat, total: u.msStt + u.msChat },
  })

  if (!transcript) {
    console.info(`[translate] ${provider} ${pair} ${seconds}s -> silence (${u.msStt + u.msChat}ms)`)
    return jsonBody()
  }

  console.info(`[translate] ${provider} ${pair} "${transcript}" -> "${translation}" `
    + `(stt ${u.msStt}ms, chat ${u.msChat}ms)`)

  if (!speak) return jsonBody()

  /* Speak path: synthesise (xAI TTS for both providers, so latency comparisons
   * isolate the understanding leg) and render the reply to pixels. Body is
   * [text bitmap][audio]. TTS/render failures fall back to the JSON body. */
  const ttsKey = xaiKey
  if (!ttsKey) { setHeader(event, 'X-Audio-Format', 'none'); setHeader(event, 'X-Audio-Error', encodeURIComponent('XAI_API_KEY needed for TTS')); return jsonBody() }
  try {
    const t2 = Date.now()
    const { pcm, seconds: spokenS } = await synthesize(translation, target, ttsKey)
    let textBuf = Buffer.alloc(0), tw = 0, th = 0
    try {
      const r = renderText(translation, target, { width: 344, maxH: 156, size: 30 })
      textBuf = r.rgb; tw = r.width; th = r.height
    } catch (re: any) { console.warn(`[translate] render failed: ${re?.message ?? re}`) }

    setHeader(event, 'Content-Type', 'application/octet-stream')
    setHeader(event, 'X-Audio-Format', 'pcm-s16le-16000-2ch')
    setHeader(event, 'X-Audio-Seconds', String(spokenS))
    setHeader(event, 'X-Text-W', String(tw))
    setHeader(event, 'X-Text-H', String(th))
    console.info(`[translate] ${provider} spoke ${spokenS}s, text ${tw}x${th} (tts ${Date.now() - t2}ms)`)
    return Buffer.concat([textBuf, pcm])
  } catch (e: any) {
    setHeader(event, 'X-Audio-Format', 'none')
    setHeader(event, 'X-Audio-Error', encodeURIComponent(String(e?.message ?? e).slice(0, 120)))
    console.error(`[translate] tts failed: ${e?.message ?? e}`)
    return jsonBody()
  }
})
