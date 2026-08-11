/**
 * Ask Grok — a general voice assistant. Speech question in, spoken + written
 * answer out.
 *
 *   POST /api/ask   body: raw 16 kHz PCM (or a container, for host tests)
 *   → headers: X-Question, X-Audio-Format, X-Audio-Seconds, X-Text-Len
 *     body:    [answer text (ASCII)][audio PCM]
 *
 * The device shows the answer text and reads it aloud, highlighting each word as
 * it is spoken. There are no per-word timestamps from the TTS, so the device
 * estimates word timing from the audio duration — good enough for karaoke, and it
 * costs no extra round trip. The answer is kept short and plain (no markdown, no
 * non-ASCII) precisely because it is both read aloud and drawn with the device's
 * ASCII font.
 *
 * grok-4.5, not the non-reasoning model translation uses: this is the task where
 * reasoning earns its keep, and 4.5's time-to-first-token (~1.1 s measured) is
 * fine for voice. Same host-holds-the-key split as everything else.
 */
import { transcribeXai, type Audio } from '../utils/understand'

function leftChannel(buf: Buffer): Buffer {
  const frames = Math.floor(buf.length / 4)
  const out = Buffer.allocUnsafe(frames * 2)
  for (let i = 0; i < frames; i++) out.writeInt16LE(buf.readInt16LE(i * 4), i * 2)
  return out
}

/* The device font is ASCII/CP437, and the TTS reads plain text best. Fold the
 * unicode a chat model loves (smart quotes, dashes, ellipsis, bullets) down to
 * ASCII, and drop anything else, so what's drawn matches what's said. */
function toAscii(s: string): string {
  return s
    .replace(/[‘’′]/g, "'")
    .replace(/[“”″]/g, '"')
    .replace(/[–—]/g, '-')
    .replace(/…/g, '...')
    .replace(/[•·]/g, '-')
    .replace(/\s+/g, ' ')
    .replace(/[^\x20-\x7e]/g, '')
    .trim()
}

export default defineEventHandler(async (event) => {
  requireDevice(event)

  const cfg = useRuntimeConfig(event) as any
  const key = cfg.xaiApiKey || process.env.XAI_API_KEY
  const model = process.env.XAI_ASK_MODEL ?? 'grok-4.5'
  if (!key) throw createError({ statusCode: 501, statusMessage: 'XAI_API_KEY is not set' })

  const q = getQuery(event) as Record<string, string>
  const raw = (await readRawBody(event, false)) as Buffer | undefined
  if (!raw?.length) throw createError({ statusCode: 400, statusMessage: 'empty audio body' })

  const ctype = (getRequestHeader(event, 'content-type') ?? '').toLowerCase()
  const container = /audio\/(mpeg|mp3|wav|x-wav|ogg|opus|flac|aac|mp4|m4a|x-matroska)/.exec(ctype)
  const mono = !container && Number(q.channels ?? 1) === 2 ? leftChannel(raw) : raw
  const audio: Audio = container
    ? { bytes: raw, isPcm: false, mime: ctype, ext: container[1] === 'mpeg' ? 'mp3' : container[1] }
    : { bytes: mono, isPcm: true, mime: 'audio/wav', ext: 'pcm' }

  const fail = (msg: string, code = 502) => {
    console.error(`[ask] ${msg}`)
    setHeader(event, 'X-Error', encodeURIComponent(msg.slice(0, 120)))
    setHeader(event, 'X-Audio-Format', 'none')
    setResponseStatus(event, code)
    return { error: msg }
  }

  const t0 = Date.now()
  let question = ''
  try {
    question = (await transcribeXai(audio, key)).text
  } catch (e: any) { return fail(`stt: ${e?.message ?? e}`) }

  setHeader(event, 'X-Question', encodeURIComponent(question))
  if (!question) {
    console.info('[ask] silence')
    setHeader(event, 'X-Audio-Format', 'none')
    setHeader(event, 'X-Text-Len', '0')
    return ''
  }

  const t1 = Date.now()
  let answer = ''
  try {
    const res = await fetch('https://api.x.ai/v1/chat/completions', {
      method: 'POST',
      headers: { Authorization: `Bearer ${key}`, 'Content-Type': 'application/json' },
      body: JSON.stringify({
        model,
        messages: [
          {
            role: 'system',
            content: 'You are a concise voice assistant. Answer in plain spoken English, '
              + '2 to 4 short sentences, suitable for reading aloud. No markdown, no lists, '
              + 'no headings, no emoji, no special characters — just sentences.',
          },
          { role: 'user', content: question },
        ],
      }),
    })
    if (!res.ok) return fail(`chat ${res.status}: ${(await res.text()).slice(0, 200)}`)
    const j = await res.json() as any
    answer = toAscii(j?.choices?.[0]?.message?.content ?? '')
  } catch (e: any) { return fail(`chat: ${e?.message ?? e}`) }

  if (!answer) return fail('empty answer', 502)
  const msChat = Date.now() - t1

  // Text always, audio best-effort — a readable answer beats none if TTS fails.
  const textBuf = Buffer.from(answer, 'ascii')
  setHeader(event, 'X-Text-Len', String(textBuf.length))

  const t2 = Date.now()
  try {
    const { pcm, seconds } = await synthesize(answer, 'en', key)
    setHeader(event, 'Content-Type', 'application/octet-stream')
    setHeader(event, 'X-Audio-Format', 'pcm-s16le-16000-2ch')
    setHeader(event, 'X-Audio-Seconds', String(seconds))
    console.info(`[ask] "${question}" -> ${answer.length} chars, ${seconds}s `
      + `(stt ${t1 - t0}ms, chat ${msChat}ms, tts ${Date.now() - t2}ms)`)
    return Buffer.concat([textBuf, pcm])
  } catch (e: any) {
    setHeader(event, 'X-Audio-Format', 'none')
    setHeader(event, 'X-Audio-Error', encodeURIComponent(String(e?.message ?? e).slice(0, 120)))
    console.warn(`[ask] tts failed, text only: ${e?.message ?? e}`)
    return textBuf
  }
})
