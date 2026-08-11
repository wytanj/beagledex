/**
 * Persistence. Deliberately boring: Nitro's own storage layer over plain files,
 * so there are no native modules to compile and nothing to install.
 *
 * To deploy on Vercel (ephemeral filesystem) change only the driver in
 * nuxt.config.ts — every call below is driver-agnostic.
 */

export interface Device {
  id: string                  // MAC without separators
  name: string
  mac: string
  chip?: string
  revision?: string
  flashBytes?: number
  psramBytes?: number
  fwVersion?: string
  buildId?: string
  activeFeatures: string[]    // what the firmware says it is actually running
  batteryPct?: number
  rssi?: number
  uptimeSec?: number
  lastSeen?: string
  firstSeen: string
  notes?: string
}

export interface Build {
  id: string
  version: string
  gitSha?: string
  channel: 'dev' | 'beta' | 'stable'
  features: string[]          // what this build ships
  sizeBytes?: number
  sha256?: string
  notes?: string
  createdAt: string
}

export interface FlashEvent {
  id: string
  deviceId: string
  buildId?: string
  version?: string
  method: 'usb' | 'ota'
  result: 'ok' | 'failed' | 'rolledback'
  target?: string             // e.g. "0x110000 factory" — which partition took it
  durationMs?: number
  error?: string
  at: string
}

export interface LogLine {
  id: string
  deviceId: string
  level: 'debug' | 'info' | 'warn' | 'error'
  tag?: string
  msg: string
  at: string
}

const db = () => useStorage('db')

/** Zero-padded so lexical key order is chronological. */
function seqKey(at: string): string {
  return `${String(Date.parse(at)).padStart(15, '0')}-${Math.random().toString(36).slice(2, 8)}`
}

async function readAll<T>(prefix: string): Promise<T[]> {
  const keys = await db().getKeys(prefix)
  if (!keys.length) return []
  const rows = await Promise.all(keys.map(k => db().getItem<T>(k)))
  return rows.filter((r): r is T => r !== null)
}

/* ── devices ──────────────────────────────────────────────────────────────── */

export const getDevice = (id: string) => db().getItem<Device>(`device:${id}`)
export const putDevice = (d: Device) => db().setItem(`device:${d.id}`, d)

export async function listDevices(): Promise<Device[]> {
  const rows = await readAll<Device>('device')
  return rows.sort((a, b) => (b.lastSeen ?? '').localeCompare(a.lastSeen ?? ''))
}

/* ── builds ───────────────────────────────────────────────────────────────── */

export const getBuild = (id: string) => db().getItem<Build>(`build:${id}`)
export const putBuild = (b: Build) => db().setItem(`build:${b.id}`, b)

export async function listBuilds(): Promise<Build[]> {
  const rows = await readAll<Build>('build')
  return rows.sort((a, b) => b.createdAt.localeCompare(a.createdAt))
}

/** Newest build on a channel — what OTA hands out. */
export async function latestBuild(channel: Build['channel']): Promise<Build | undefined> {
  return (await listBuilds()).find(b => b.channel === channel)
}

/* ── flash events ─────────────────────────────────────────────────────────── */

export async function addFlash(e: Omit<FlashEvent, 'id'>): Promise<FlashEvent> {
  const row: FlashEvent = { ...e, id: seqKey(e.at) }
  await db().setItem(`flash:${row.id}`, row)
  return row
}

export async function listFlashes(deviceId?: string, limit = 100): Promise<FlashEvent[]> {
  const rows = await readAll<FlashEvent>('flash')
  return rows
    .filter(r => !deviceId || r.deviceId === deviceId)
    .sort((a, b) => b.id.localeCompare(a.id))
    .slice(0, limit)
}

/* ── logs ─────────────────────────────────────────────────────────────────── */

export async function addLog(e: Omit<LogLine, 'id'>): Promise<LogLine> {
  const row: LogLine = { ...e, id: seqKey(e.at) }
  await db().setItem(`log:${row.deviceId}:${row.id}`, row)
  return row
}

export async function listLogs(deviceId?: string, limit = 300): Promise<LogLine[]> {
  const rows = await readAll<LogLine>(deviceId ? `log:${deviceId}` : 'log')
  return rows.sort((a, b) => b.id.localeCompare(a.id)).slice(0, limit)
}
