/** Auto-imported by Nuxt. Presentation only — no fetching, no state. */

const ONLINE_WINDOW_MS = 5 * 60 * 1000

export function isOnline(lastSeen?: string): boolean {
  if (!lastSeen) return false
  return Date.now() - Date.parse(lastSeen) < ONLINE_WINDOW_MS
}

export function ago(iso?: string): string {
  if (!iso) return 'never'
  const s = Math.max(0, Math.floor((Date.now() - Date.parse(iso)) / 1000))
  if (s < 45) return `${s}s ago`
  const m = Math.floor(s / 60)
  if (m < 60) return `${m}m ago`
  const h = Math.floor(m / 60)
  if (h < 24) return `${h}h ago`
  return `${Math.floor(h / 24)}d ago`
}

export function clock(iso?: string): string {
  if (!iso) return '—'
  return new Date(iso).toLocaleTimeString(undefined, { hour12: false })
}

export function bytes(n?: number | null): string {
  if (n == null) return '—'
  if (n < 1024) return `${n} B`
  if (n < 1024 ** 2) return `${(n / 1024).toFixed(1)} KB`
  return `${(n / 1024 ** 2).toFixed(2)} MB`
}

export function uptime(sec?: number): string {
  if (sec == null) return '—'
  const d = Math.floor(sec / 86400)
  const h = Math.floor((sec % 86400) / 3600)
  const m = Math.floor((sec % 3600) / 60)
  if (d) return `${d}d ${h}h`
  if (h) return `${h}h ${m}m`
  return `${m}m`
}

export function resultClass(result: string): string {
  return result === 'ok' ? 'ok' : result === 'rolledback' ? 'warn' : 'risk'
}
