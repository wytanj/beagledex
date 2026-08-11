export default defineEventHandler(async (event) => {
  const { deviceId, limit } = getQuery(event) as { deviceId?: string; limit?: string }
  return { flashes: await listFlashes(deviceId, limit ? Number(limit) : 100) }
})
