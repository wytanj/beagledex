/**
 * Canonical feature registry.
 *
 * A build declares which of these it *ships*; a device reports which it has
 * *active* at runtime. The console diffs the two, so "we flashed the OTA build
 * but rollback never armed" shows up as drift instead of a silent assumption.
 *
 * Phase numbers match the build plan, so the fleet view doubles as progress.
 */
export interface Feature {
  id: string
  phase: number
  label: string
  desc: string
}

export const FEATURES: Feature[] = [
  // Named 'loopback' before we learned this board cannot do one: mic and speaker
  // are centimetres apart with no echo cancellation, so a live loopback
  // saturates to full scale. The id is kept as a stable key; the meaning is now
  // capture-with-output-muted, measure, then play back.
  { id: 'audio.loopback',  phase: 0, label: 'Audio self-test',       desc: 'Muted capture → level check → playback. Half-duplex; a live loopback howls on this hardware.' },
  { id: 'audio.ptt',       phase: 1, label: 'Push-to-talk capture',  desc: 'BOOT button gates 16 kHz capture into PSRAM, release plays it back.' },
  { id: 'audio.sdcache',   phase: 1, label: 'SD record / replay',    desc: 'WAV write and playback from microSD. Not started — audio is PSRAM-only today.' },
  { id: 'net.provision',   phase: 3, label: 'WiFi provisioning',     desc: 'SoftAP or BLE provisioning. No hardcoded SSID.' },
  { id: 'net.keepalive',   phase: 3, label: 'HTTP keep-alive',       desc: 'One reused client handle so TLS is negotiated once per session.' },
  { id: 'translate.cloud', phase: 3, label: 'Cloud translate',       desc: 'POST PCM to /api/translate, stream the reply straight to I2S.' },
  { id: 'ui.display',      phase: 4, label: 'AMOLED transcript',     desc: 'Transcript, translation and battery on the 368×448 panel.' },
  { id: 'ui.touch',        phase: 4, label: 'Touch language select', desc: 'CST816/820 tap-to-choose language pair. The v1 FT3168 is not fitted on this unit — an I2C scan found 0x15, nothing at 0x38.' },
  { id: 'ui.shell',        phase: 4, label: 'App shell',             desc: 'Static app registry, swipe navigation, and one shared push-to-talk service every app reuses rather than reimplements.' },
  { id: 'app.clock',       phase: 4, label: 'Clock app',             desc: 'Time and date from the PCF85063. Reports an unset clock instead of confidently showing 00:00.' },
  { id: 'app.ask',         phase: 4, label: 'Ask app',               desc: 'The push-to-talk to LLM primitive: capture, then render a streamed token reply. Transport is phase 3; until then tokens arrive over serial.' },
  { id: 'app.settings',    phase: 4, label: 'Settings app',          desc: 'Brightness, sleep timeout, always-on and lock, plus a live device readout. Opened from the launcher; absorbs the old swipe-down quick panel.' },
  { id: 'ui.launcher',     phase: 4, label: 'App launcher',          desc: 'Swipe up for a grid of apps, tap to open, swipe down for the face. Replaces swiping through a ring of screens.' },
  { id: 'pwr.sleep',       phase: 4, label: 'Idle sleep',            desc: 'Dim then displayOff on an idle timer, waking on touch or button. displayOff matters: brightness 0 alone leaves the AMOLED controller self-refreshing and still drawing current.' },
  { id: 'ui.buttons',      phase: 4, label: 'Button mapping',        desc: 'BOOT: tap locks or wakes, double tap changes app, hold captures voice. Double tap is app-switching so a failed touch panel cannot strand you.' },
  { id: 'ui.quicksettings',phase: 4, label: 'Quick settings',        desc: 'Swipe down from the status strip for brightness, sleep timeout and lock. Vertical swipes elsewhere still belong to the app.' },
  { id: 'cmd.structured',  phase: 4, label: 'Voice commands',        desc: 'set_language / swap / repeat / slower / louder.' },
  { id: 'imu.wake',        phase: 4, label: 'Wrist-raise warmup',    desc: 'QMI8658 gesture pings the endpoint so it is warm on button press.' },
  { id: 'phrase.offline',  phase: 4, label: 'Offline phrase cache',  desc: 'Pre-rendered traveller phrases on microSD.' },
  { id: 'ota.rollback',    phase: 5, label: 'OTA + rollback',        desc: 'esp_https_ota, image marked valid only after the backend answers.' },
  { id: 'net.ble',         phase: 6, label: 'BLE phone relay',       desc: 'Companion app relays to the backend over cellular.' },
]

export const FEATURE_IDS = new Set(FEATURES.map(f => f.id))

/** Drop unknown ids so a typo in a build manifest can't poison the matrix. */
export function sanitiseFeatures(input: unknown): string[] {
  if (!Array.isArray(input)) return []
  return [...new Set(input.filter((f): f is string => typeof f === 'string' && FEATURE_IDS.has(f)))]
}
