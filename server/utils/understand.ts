/**
 * The "understand" leg of /api/translate: audio in, transcript + translation out.
 * Two interchangeable providers behind one shape, so the device can flip between
 * them (?provider=grok|gemini) and we can measure real-world latency rather than
 * argue about it. TTS is deliberately NOT here — it stays one implementation
 * (tts.ts) for both, so the only variable in a head-to-head is the understanding.
 *
 *   grok   — xAI: two calls, STT then chat. Proven.
 *   gemini — Google: ONE call. Gemini takes audio natively, so transcribe +
 *            detect + translate happen together, which is both cheaper per unit
 *            and a round trip shorter. Written to the documented format; verify it
 *            once a GEMINI_API_KEY exists.
 *
 * The device never sees any of this — provider choice is a pure host concern,
 * which is the whole reason it can be swapped freely.
 */

const LANGUAGE_NAMES: Record<string, string> = {
  en: 'English', ja: 'Japanese', zh: 'Chinese', ko: 'Korean', es: 'Spanish',
  fr: 'French', de: 'German', it: 'Italian', pt: 'Portuguese', ru: 'Russian',
  ar: 'Arabic', hi: 'Hindi', th: 'Thai', vi: 'Vietnamese', id: 'Indonesian',
  ms: 'Malay', ta: 'Tamil', nl: 'Dutch', tr: 'Turkish', pl: 'Polish',
  // Spell it out for the models — a bare "nan" is not something they translate to
  // reliably, and naming Tai-lo nudges toward usable romanisation.
  nan: 'Taiwanese Hokkien (Min Nan; Han characters with Tai-lo romanisation)',
}
export const languageName = (c: string) => LANGUAGE_NAMES[c] ?? c

export interface Audio {
  bytes: Buffer
  isPcm: boolean       // raw 16 kHz mono s16le (the device path) vs a container file
  mime: string         // for a container: its content-type
  ext: string          // for a container: file extension xAI keys off
}

export interface UnderstandOpts {
  fromCode: string
  toCode: string
  autoDirect: boolean  // detected language decides direction
  key: string
}

export interface Understanding {
  transcript: string
  translation: string  // '' when nothing was said
  detected: string
  target: string
  msStt: number        // grok: STT leg. gemini: 0 (one call)
  msChat: number       // grok: chat leg. gemini: the whole call
  seconds: number      // audio duration if the provider reports it, else 0
}

/* ── grok (xAI): STT then chat ─────────────────────────────────────────────── */

export async function understandXai(a: Audio, o: UnderstandOpts): Promise<Understanding> {
  const t0 = Date.now()
  const fd = new FormData()
  if (a.isPcm) { fd.append('audio_format', 'pcm'); fd.append('sample_rate', '16000') }
  // Auto-direction: no language hint, because the hint biases the detection that
  // decides which way to translate.
  if (!o.autoDirect && o.fromCode !== 'auto') fd.append('language', o.fromCode)
  fd.append('file', new Blob([new Uint8Array(a.bytes)]), a.isPcm ? 'capture.pcm' : `capture.${a.ext}`)

  const sttRes = await fetch('https://api.x.ai/v1/stt', {
    method: 'POST', headers: { Authorization: `Bearer ${o.key}` }, body: fd,
  })
  if (!sttRes.ok) throw new Error(`stt ${sttRes.status}: ${(await sttRes.text()).slice(0, 200)}`)
  const stt = await sttRes.json() as { text?: string; language?: string; duration?: number }
  const transcript = (stt.text ?? '').trim()
  const msStt = Date.now() - t0
  const seconds = stt.duration ? +stt.duration.toFixed(2) : 0

  const spoke = (stt.language || o.fromCode).toLowerCase()
  const target = o.autoDirect && spoke === o.toCode.toLowerCase() ? o.fromCode : o.toCode
  if (!transcript) return { transcript: '', translation: '', detected: spoke, target, msStt, msChat: 0, seconds }

  const model = process.env.XAI_CHAT_MODEL ?? 'grok-4.20-0309-non-reasoning'
  const t1 = Date.now()
  const chatRes = await fetch('https://api.x.ai/v1/chat/completions', {
    method: 'POST',
    headers: { Authorization: `Bearer ${o.key}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({
      model,
      messages: [
        { role: 'system', content: translatePrompt(o.fromCode, o.toCode, target) },
        { role: 'user', content: transcript },
      ],
    }),
  })
  if (!chatRes.ok) throw new Error(`chat ${chatRes.status}: ${(await chatRes.text()).slice(0, 200)}`)
  const chat = await chatRes.json() as any
  const translation = (chat?.choices?.[0]?.message?.content ?? '').trim()
  return { transcript, translation, detected: spoke, target, msStt, msChat: Date.now() - t1, seconds }
}

/* ── gemini (Google): one native-audio call ────────────────────────────────── */

function wavMono16k(pcm: Buffer): Buffer {
  const sr = 16000, ch = 1, bps = 16
  const h = Buffer.alloc(44)
  h.write('RIFF', 0); h.writeUInt32LE(36 + pcm.length, 4); h.write('WAVE', 8)
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(ch, 22)
  h.writeUInt32LE(sr, 24); h.writeUInt32LE(sr * ch * bps / 8, 28)
  h.writeUInt16LE(ch * bps / 8, 32); h.writeUInt16LE(bps, 34)
  h.write('data', 36); h.writeUInt32LE(pcm.length, 40)
  return Buffer.concat([h, pcm])
}

export async function understandGemini(a: Audio, o: UnderstandOpts): Promise<Understanding> {
  // An alias, not a pinned version: the cheapest current tier, and it won't 404
  // when Google retires a numbered model for new keys (which 2.5-flash-lite did).
  const model = process.env.GEMINI_MODEL ?? 'gemini-flash-lite-latest'
  const audio = a.isPcm ? wavMono16k(a.bytes) : a.bytes
  const mime = a.isPcm ? 'audio/wav' : a.mime

  const dir = o.autoDirect
    ? `The speaker is speaking one of two languages: ${languageName(o.fromCode)} (${o.fromCode}) `
      + `or ${languageName(o.toCode)} (${o.toCode}). Detect which, and translate into the OTHER one.`
    : `Translate from ${languageName(o.fromCode)} (${o.fromCode}) into ${languageName(o.toCode)} (${o.toCode}).`

  const t0 = Date.now()
  const res = await fetch(
    `https://generativelanguage.googleapis.com/v1beta/models/${model}:generateContent`,
    {
      method: 'POST',
      headers: { 'x-goog-api-key': o.key, 'Content-Type': 'application/json' },
      body: JSON.stringify({
        systemInstruction: {
          parts: [{
            text: 'You are a two-way conversation translator. ' + dir
              + ' Transcribe the speech verbatim, then give a natural, spoken translation — '
              + 'no notes, no romanisation, no commentary. If nothing intelligible was said, '
              + 'return empty strings.',
          }],
        },
        contents: [{ role: 'user', parts: [{ inlineData: { mimeType: mime, data: audio.toString('base64') } }] }],
        generationConfig: {
          responseMimeType: 'application/json',
          responseSchema: {
            type: 'object',
            properties: {
              transcript:  { type: 'string' },
              translation: { type: 'string' },
              detected:    { type: 'string' },   // language code of the speech
              target:      { type: 'string' },   // language code of the translation
            },
            required: ['transcript', 'translation', 'detected', 'target'],
          },
        },
      }),
    },
  )
  if (!res.ok) throw new Error(`gemini ${res.status}: ${(await res.text()).slice(0, 200)}`)
  const j = await res.json() as any
  const text = j?.candidates?.[0]?.content?.parts?.[0]?.text ?? '{}'
  let out: any = {}
  try { out = JSON.parse(text) } catch { throw new Error('gemini returned non-JSON') }

  const detected = (out.detected || o.fromCode).toLowerCase()
  const target = (out.target || (o.autoDirect && detected === o.toCode.toLowerCase() ? o.fromCode : o.toCode)).toLowerCase()
  return {
    transcript: (out.transcript ?? '').trim(),
    translation: (out.translation ?? '').trim(),
    detected, target,
    msStt: 0, msChat: Date.now() - t0, seconds: 0,
  }
}

/* Shared system prompt for the chat-based (grok) path. */
function translatePrompt(fromCode: string, toCode: string, targetCode: string): string {
  return `You translate speech for a two-way conversation between a ${languageName(fromCode)} `
    + `speaker and a ${languageName(toCode)} speaker. Translate the user's words into natural, `
    + `spoken ${languageName(targetCode)}. Reply with the translation only — no notes, no `
    + `romanisation, no quotation marks, no commentary. Always translate what you were given, `
    + `even a single word or fragment. Never answer the speaker, never explain, and never reply `
    + `with nothing: an empty reply looks like the microphone failed.`
}
