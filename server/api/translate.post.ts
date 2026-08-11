/**
 * The product endpoint. Speech in, transcript and translation out.
 *
 *   POST /api/translate?pair=en-ja[&channels=2]
 *     body  raw 16 kHz 16-bit little-endian PCM
 *   →  JSON { transcript, translation, detected, pair, seconds, ms }
 *      also mirrored into X-Transcript / X-Translation / X-Detected headers, so
 *      firmware can read the result without a JSON parser if it prefers.
 *
 * WHY THE KEY LIVES HERE AND NOT ON THE DEVICE
 *
 * Firmware flash is not a secret store — a full chip dump is a six-second job on
 * this board, and we have one on disk to prove it. The device is a UI layer that
 * transmits audio and renders tokens; the host holds credentials and does the
 * work. Everything below is a consequence of that split.
 *
 * ?channels=2 exists because the ES8311 is a mono codec whose I2S frame still
 * carries two slots, so the device's natural buffer is interleaved with the
 * microphone on the left and silence on the right. Deinterleaving costs one pass
 * over up to 1.8 MB of PSRAM on a device that is trying to save battery, and
 * nothing here, so it happens here.
 *
 * Still not built: speech synthesis. The original contract promised PCM back in
 * the body for straight-to-I2S playback, and that stays the plan — but the shell
 * has no playback path yet and deliberately gates the amplifier, so returning
 * audio nothing can play would be a stub pretending to be a feature.
 */

/** Codes the model reads better as names. Anything unlisted passes through. */
const LANGUAGE_NAMES: Record<string, string> = {
  en: 'English', ja: 'Japanese', zh: 'Chinese', ko: 'Korean', es: 'Spanish',
  fr: 'French', de: 'German', it: 'Italian', pt: 'Portuguese', ru: 'Russian',
  ar: 'Arabic', hi: 'Hindi', th: 'Thai', vi: 'Vietnamese', id: 'Indonesian',
  ms: 'Malay', ta: 'Tamil', nl: 'Dutch', tr: 'Turkish', pl: 'Polish',
}
const languageName = (c: string) => LANGUAGE_NAMES[c] ?? c

/** Keep only the left slot of an interleaved 2-slot frame — the microphone. */
function leftChannel(buf: Buffer): Buffer {
  const frames = Math.floor(buf.length / 4)          // 2 slots x 16-bit
  const out = Buffer.allocUnsafe(frames * 2)
  for (let i = 0; i < frames; i++) out.writeInt16LE(buf.readInt16LE(i * 4), i * 2)
  return out
}

export default defineEventHandler(async (event) => {
  requireDevice(event)

  const key = (useRuntimeConfig(event) as any).xaiApiKey || process.env.XAI_API_KEY
  const q = getQuery(event) as { pair?: string; channels?: string; model?: string }
  const pair = q.pair ?? 'en-ja'
  const [fromCode = 'en', toCode = 'ja'] = pair.split('-')
  /* Non-reasoning by default, and measured rather than assumed: on the same
   * sentence grok-4.5 and grok-4.3 spent 3.0 s and 3.9 s in chat, burning
   * reasoning tokens to translate one line, while the non-reasoning model took
   * 1.0 s and returned a byte-identical translation. Translation is not a task
   * that benefits from deliberation, and this device is held in someone's hand
   * mid-conversation. Override per request with ?model= or globally with
   * XAI_CHAT_MODEL. */
  const model = q.model ?? process.env.XAI_CHAT_MODEL ?? 'grok-4.20-0309-non-reasoning'

  const raw = (await readRawBody(event, false)) as Buffer | undefined
  if (!raw?.length) throw createError({ statusCode: 400, statusMessage: 'empty audio body' })

  /* The device always sends raw PCM. A container file is accepted too, purely so
   * the pipeline can be exercised from the host with an ordinary audio file and no
   * hardware in the loop — xAI auto-detects WAV/MP3/OGG/FLAC/AAC/MP4/M4A/MKV. The
   * device path is unaffected by its existence. */
  const ctype = (getRequestHeader(event, 'content-type') ?? '').toLowerCase()
  const container = /audio\/(mpeg|mp3|wav|x-wav|ogg|opus|flac|aac|mp4|m4a|x-matroska)/.exec(ctype)

  const mono = !container && Number(q.channels ?? 1) === 2 ? leftChannel(raw) : raw
  // Only raw PCM has a duration we can derive from its length; for a container we
  // take xAI's word for it below.
  let seconds = container ? 0 : +(mono.length / 32000).toFixed(2)

  if (!key) {
    // The 501 stays, because a missing key is not something to paper over.
    console.warn('[translate] XAI_API_KEY unset — refusing rather than faking')
    throw createError({
      statusCode: 501,
      statusMessage: 'XAI_API_KEY is not set',
      data: { received: { pair, bytes: raw.length, seconds } },
    })
  }

  const t0 = Date.now()

  /* ── 1. transcribe ─────────────────────────────────────────────────────────
   * Raw PCM goes up as-is: xAI accepts audio_format=pcm at 16 kHz, which is
   * exactly what the codec produces, so there is no wrapping or resampling
   * anywhere in this path. Their docs require the option fields to precede
   * `file`, and FormData preserves insertion order — hence the ordering below. */
  const fd = new FormData()
  if (!container) {
    fd.append('audio_format', 'pcm')
    fd.append('sample_rate', '16000')
  }
  /* With auto-direction on (the default) we deliberately do NOT hint the language,
   * because the hint biases detection and detection is what decides which way to
   * translate. Pass &auto=0 to force the stated direction. */
  const autoDirect = (q.auto ?? '1') !== '0'
  if (!autoDirect && fromCode && fromCode !== 'auto') fd.append('language', fromCode)
  fd.append('file', new Blob([new Uint8Array(mono)]),
            container ? `capture.${container[1] === 'mpeg' ? 'mp3' : container[1]}` : 'capture.pcm')

  const sttRes = await fetch('https://api.x.ai/v1/stt', {
    method: 'POST',
    headers: { Authorization: `Bearer ${key}` },
    body: fd,
  })
  if (!sttRes.ok) {
    const detail = (await sttRes.text()).slice(0, 300)
    console.error(`[translate] stt ${sttRes.status} ${detail}`)
    throw createError({ statusCode: 502, statusMessage: `stt failed: ${sttRes.status}`, data: { detail } })
  }
  const stt = await sttRes.json() as { text?: string; language?: string; duration?: number }
  const transcript = (stt.text ?? '').trim()
  const msStt = Date.now() - t0
  if (container && stt.duration) seconds = +stt.duration.toFixed(2)

  /* Which way round? In a two-way conversation the speaker changes every few
   * seconds, and a direction toggle is one more thing to fumble mid-sentence. So
   * the detected language decides: say the far-side language and it comes back in
   * yours. Two selections and one button, with no third control to get wrong. */
  const spoke = (stt.language || fromCode).toLowerCase()
  const targetCode = autoDirect && spoke === toCode.toLowerCase() ? fromCode : toCode

  const reply = (translation: string, msChat: number) => {
    setHeader(event, 'X-Transcript', encodeURIComponent(transcript))
    setHeader(event, 'X-Translation', encodeURIComponent(translation))
    setHeader(event, 'X-Detected', spoke)
    setHeader(event, 'X-Target', targetCode)
    return {
      transcript, translation,
      detected: spoke,
      target: targetCode,
      pair, seconds,
      ms: { stt: msStt, chat: msChat, total: msStt + msChat },
    }
  }

  // Nothing said. Don't spend a chat call proving it.
  if (!transcript) {
    console.info(`[translate] ${pair} ${seconds}s → silence (stt ${msStt}ms)`)
    return reply('', 0)
  }

  /* ── 2. translate ─────────────────────────────────────────────────────────── */
  const t1 = Date.now()
  const chatRes = await fetch('https://api.x.ai/v1/chat/completions', {
    method: 'POST',
    headers: { Authorization: `Bearer ${key}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({
      model,
      messages: [
        {
          role: 'system',
          // Spoken translation, not literary: this is read aloud to a stranger,
          // and a model that editorialises makes the device look broken.
          content: `You translate speech for a two-way conversation between a `
            + `${languageName(fromCode)} speaker and a ${languageName(toCode)} speaker. `
            + `Translate the user's words into natural, spoken `
            + `${languageName(targetCode)}. Reply with the translation only — no notes, `
            + `no romanisation, no quotation marks. If the input is not meaningful `
            + `speech, reply with an empty string.`,
        },
        { role: 'user', content: transcript },
      ],
    }),
  })
  if (!chatRes.ok) {
    const detail = (await chatRes.text()).slice(0, 300)
    console.error(`[translate] chat ${chatRes.status} ${detail}`)
    throw createError({ statusCode: 502, statusMessage: `translate failed: ${chatRes.status}`, data: { transcript, detail } })
  }
  const chat = await chatRes.json() as any
  const translation = (chat?.choices?.[0]?.message?.content ?? '').trim()
  const msChat = Date.now() - t1

  console.info(`[translate] ${pair} ${seconds}s "${transcript}" → "${translation}" `
    + `(stt ${msStt}ms, chat ${msChat}ms)`)

  return reply(translation, msChat)
})
