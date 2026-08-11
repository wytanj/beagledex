/**
 * The product endpoint. Contract is fixed (the firmware can be written against
 * it today) but the three providers are not wired up yet — that is phase 02.
 *
 *   POST /api/translate?pair=en-ja
 *     body     raw 16 kHz 16-bit mono PCM
 *   →  body    raw 16 kHz 16-bit mono PCM, straight to I2S
 *      headers X-Transcript, X-Translation, X-Detected, X-Kind, X-Action
 *
 * Returns 501 rather than a plausible-looking fake, so firmware work can't
 * accidentally be validated against a stub.
 */
export default defineEventHandler(async (event) => {
  requireDevice(event)

  const { pair = 'en-ja' } = getQuery(event) as { pair?: string }
  const body = await readRawBody(event, false)
  const bytes = body?.length ?? 0

  // 16 kHz · 16-bit · mono = 32000 bytes/sec
  const seconds = +(bytes / 32000).toFixed(2)

  console.info(`[translate] pair=${pair} bytes=${bytes} (${seconds}s of audio) — not implemented`)

  throw createError({
    statusCode: 501,
    statusMessage: 'translate pipeline not implemented',
    data: {
      received: { pair, bytes, seconds },
      todo: [
        'speech-to-text with language auto-detect across the pair',
        'Claude translate + command parse (structured output, effort: low)',
        'speech synthesis requested as 16 kHz 16-bit mono PCM',
        'return PCM as the body, text in X-Transcript / X-Translation headers',
      ],
    },
  })
})
