/**
 * Record a flash. Posted by scripts/flash.mjs after esptool exits, and by the
 * firmware itself after an OTA — so a USB flash and an OTA land in the same
 * history and you can see which one a device actually ended up running.
 *
 * Failures are recorded too. A history that only contains successes is the one
 * you can't debug from.
 */
export default defineEventHandler(async (event) => {
  // Guarded for the same reason as builds: the flash timeline is the one thing
  // this console is supposed to be trustworthy about.
  requireDevice(event)

  const body = await readBody<{
    deviceId?: string
    buildId?: string
    version?: string
    method?: FlashEvent['method']
    result?: FlashEvent['result']
    target?: string
    durationMs?: number
    error?: string
  }>(event)

  if (!body?.deviceId) {
    throw createError({ statusCode: 400, statusMessage: 'deviceId is required' })
  }
  const method = body.method ?? 'usb'
  const result = body.result ?? 'ok'
  if (!['usb', 'ota'].includes(method)) {
    throw createError({ statusCode: 400, statusMessage: 'method must be usb or ota' })
  }
  if (!['ok', 'failed', 'rolledback'].includes(result)) {
    throw createError({ statusCode: 400, statusMessage: 'result must be ok, failed or rolledback' })
  }

  const flash = await addFlash({
    deviceId: body.deviceId,
    buildId: body.buildId,
    version: body.version,
    method,
    result,
    target: body.target,
    durationMs: body.durationMs,
    error: body.error,
    at: new Date().toISOString(),
  })

  // A successful flash is the authoritative statement of what a device runs.
  if (result === 'ok') {
    const device = await getDevice(body.deviceId)
    if (device) {
      device.fwVersion = body.version ?? device.fwVersion
      device.buildId = body.buildId ?? device.buildId
      // Expected features come from the build; the device confirms them on next boot.
      if (body.buildId) {
        const build = await getBuild(body.buildId)
        if (build) device.activeFeatures = build.features
      }
      await putDevice(device)
    }
  }

  return { ok: true, flash }
})
