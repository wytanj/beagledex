/**
 * Called by the firmware on every boot. Idempotent: creates the device on first
 * contact, otherwise refreshes the mutable fields and leaves firstSeen/name/notes
 * alone so anything you typed in the console survives a reflash.
 */
export default defineEventHandler(async (event) => {
  requireDevice(event)

  const body = await readBody<{
    mac?: string
    chip?: string
    revision?: string
    flashBytes?: number
    psramBytes?: number
    fwVersion?: string
    buildId?: string
    activeFeatures?: string[]
  }>(event)

  if (!body?.mac) {
    throw createError({ statusCode: 400, statusMessage: 'mac is required' })
  }

  const id = body.mac.replace(/[^0-9a-fA-F]/g, '').toLowerCase()
  if (id.length !== 12) {
    throw createError({ statusCode: 400, statusMessage: 'mac must be 6 bytes' })
  }

  const now = new Date().toISOString()
  const existing = await getDevice(id)

  const device: Device = {
    id,
    mac: body.mac,
    name: existing?.name ?? `esp32s3-${id.slice(-6)}`,
    notes: existing?.notes,
    firstSeen: existing?.firstSeen ?? now,
    chip: body.chip ?? existing?.chip,
    revision: body.revision ?? existing?.revision,
    flashBytes: body.flashBytes ?? existing?.flashBytes,
    psramBytes: body.psramBytes ?? existing?.psramBytes,
    fwVersion: body.fwVersion ?? existing?.fwVersion,
    buildId: body.buildId ?? existing?.buildId,
    activeFeatures: sanitiseFeatures(body.activeFeatures ?? existing?.activeFeatures),
    batteryPct: existing?.batteryPct,
    rssi: existing?.rssi,
    uptimeSec: existing?.uptimeSec,
    lastSeen: now,
  }

  await putDevice(device)
  return { ok: true, device, isNew: !existing }
})
