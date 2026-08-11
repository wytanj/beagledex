export default defineEventHandler(async () => {
  return { devices: await listDevices() }
})
