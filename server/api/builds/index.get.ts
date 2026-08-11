export default defineEventHandler(async () => {
  return { builds: await listBuilds() }
})
