/**
 * OTA manifest the firmware polls.
 *
 *   GET /api/ota/check?dev=<id>&ver=<current>&channel=dev
 *     → 204  already current
 *     → 200  { version, url, sha256, size, mandatory }
 *
 * Binary hosting is deliberately not implemented yet — see the TODO. Until it
 * is, this reports what a device *would* be offered, which is enough to build
 * and test the firmware's polling and version-compare logic.
 */
export default defineEventHandler(async (event) => {
  requireDevice(event)

  const { dev, ver, channel } = getQuery(event) as {
    dev?: string; ver?: string; channel?: Build['channel']
  }

  if (!dev) {
    throw createError({ statusCode: 400, statusMessage: 'dev is required' })
  }

  // A device pinned to a version never gets moved by a channel release.
  const device = await getDevice(dev)
  const target = await latestBuild(channel ?? 'dev')

  if (!target) {
    setResponseStatus(event, 204)
    return null
  }

  if (ver && ver === target.version) {
    setResponseStatus(event, 204)
    return null
  }

  return {
    version: target.version,
    buildId: target.id,
    features: target.features,
    sha256: target.sha256 ?? null,
    size: target.sizeBytes ?? null,
    mandatory: false,
    // TODO: serve the signed binary. Put it in blob storage and return a URL —
    // do NOT stream multi-MB images out of a serverless function.
    url: null,
    note: device
      ? 'binary hosting not wired yet — manifest only'
      : 'device not registered; manifest returned anyway',
  }
})
