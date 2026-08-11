/**
 * Register a build. Called by scripts/flash.mjs (and later by CI) so the record
 * is created by the thing that produced the binary rather than typed by hand.
 */
export default defineEventHandler(async (event) => {
  const body = await readBody<{
    version?: string
    gitSha?: string
    channel?: Build['channel']
    features?: string[]
    sizeBytes?: number
    sha256?: string
    notes?: string
  }>(event)

  if (!body?.version) {
    throw createError({ statusCode: 400, statusMessage: 'version is required' })
  }

  const channel = body.channel ?? 'dev'
  if (!['dev', 'beta', 'stable'].includes(channel)) {
    throw createError({ statusCode: 400, statusMessage: 'channel must be dev, beta or stable' })
  }

  // Version + channel identifies a build, so reflashing the same version
  // updates the record instead of piling up duplicates.
  const id = `${body.version}-${channel}`.replace(/[^a-zA-Z0-9._-]/g, '_')
  const existing = await getBuild(id)

  const build: Build = {
    id,
    version: body.version,
    gitSha: body.gitSha ?? existing?.gitSha,
    channel,
    features: sanitiseFeatures(body.features ?? existing?.features),
    sizeBytes: body.sizeBytes ?? existing?.sizeBytes,
    sha256: body.sha256 ?? existing?.sha256,
    notes: body.notes ?? existing?.notes,
    createdAt: existing?.createdAt ?? new Date().toISOString(),
  }

  await putBuild(build)
  return { ok: true, build, isNew: !existing }
})
