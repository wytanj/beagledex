/**
 * Device-facing routes carry a shared bearer token.
 *
 * If DEVICE_TOKEN is unset we allow the request and warn loudly — that keeps
 * `npm run dev` frictionless on localhost while making the gap impossible to
 * miss in logs. Set it before anything is reachable from outside your LAN.
 */
let warned = false

export function requireDevice(event: any): void {
  const expected = useRuntimeConfig(event).deviceToken || process.env.DEVICE_TOKEN

  if (!expected) {
    if (!warned) {
      console.warn('[auth] DEVICE_TOKEN is unset — device routes are UNAUTHENTICATED')
      warned = true
    }
    return
  }

  const header = getRequestHeader(event, 'authorization') ?? ''
  const token = header.startsWith('Bearer ') ? header.slice(7) : ''

  if (token !== expected) {
    throw createError({ statusCode: 401, statusMessage: 'bad device token' })
  }
}
