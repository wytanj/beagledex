export default defineEventHandler(async (event) => {
  const { deviceId, limit } = getQuery(event) as { deviceId?: string; limit?: string }
  return { logs: await listLogs(deviceId, limit ? Number(limit) : 300) }
})
