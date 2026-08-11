/**
 * Log sink. Accepts one line or a batch — batch from the firmware, because one
 * HTTP round trip per log line will wreck both your latency and your quota.
 *
 * POST { deviceId, lines: [{ level, tag, msg }] }
 */
export default defineEventHandler(async (event) => {
  requireDevice(event)

  const body = await readBody<{
    deviceId?: string
    level?: LogLine['level']
    tag?: string
    msg?: string
    lines?: Array<{ level?: LogLine['level']; tag?: string; msg?: string }>
  }>(event)

  if (!body?.deviceId) {
    throw createError({ statusCode: 400, statusMessage: 'deviceId is required' })
  }

  const incoming = body.lines?.length
    ? body.lines
    : [{ level: body.level, tag: body.tag, msg: body.msg }]

  const at = new Date().toISOString()
  let written = 0

  for (const line of incoming) {
    if (!line?.msg) continue
    const level = line.level && ['debug', 'info', 'warn', 'error'].includes(line.level)
      ? line.level
      : 'info'
    await addLog({
      deviceId: body.deviceId,
      level,
      tag: line.tag,
      msg: String(line.msg).slice(0, 2000),
      at,
    })
    written++
  }

  return { ok: true, written }
})
