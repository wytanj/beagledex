/**
 * Cheap periodic ping — battery, signal, uptime. Keep the firmware's interval
 * generous (60s+); this exists to answer "is it alive and how's the battery",
 * not to stream telemetry.
 */
export default defineEventHandler(async (event) => {
  requireDevice(event)

  const id = getRouterParam(event, 'id')!
  const device = await getDevice(id)
  if (!device) {
    throw createError({ statusCode: 404, statusMessage: 'unknown device — call /api/devices/register first' })
  }

  const body = await readBody<{
    batteryPct?: number
    rssi?: number
    uptimeSec?: number
    fwVersion?: string
    activeFeatures?: string[]
  }>(event)

  Object.assign(device, {
    batteryPct: body?.batteryPct ?? device.batteryPct,
    rssi: body?.rssi ?? device.rssi,
    uptimeSec: body?.uptimeSec ?? device.uptimeSec,
    fwVersion: body?.fwVersion ?? device.fwVersion,
    activeFeatures: body?.activeFeatures ? sanitiseFeatures(body.activeFeatures) : device.activeFeatures,
    lastSeen: new Date().toISOString(),
  })

  await putDevice(device)
  return { ok: true }
})
