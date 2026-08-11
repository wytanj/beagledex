<template>
  <div v-if="device">
    <h1>{{ device.name }}</h1>
    <p class="lede m" style="font-size:0.8125rem">
      {{ device.mac }} · {{ device.chip ?? 'unknown chip' }}
      {{ device.revision ? 'rev ' + device.revision : '' }}
      · flash {{ bytes(device.flashBytes) }} · psram {{ bytes(device.psramBytes) }}
    </p>

    <dl class="stats">
      <div class="stat">
        <dt>State</dt>
        <dd style="font-size:0.875rem">
          <span class="pill" :class="isOnline(device.lastSeen) ? 'ok' : 'idle'">
            {{ isOnline(device.lastSeen) ? 'online' : 'offline' }}
          </span>
        </dd>
      </div>
      <div class="stat"><dt>Firmware</dt><dd style="font-size:0.9375rem">{{ device.fwVersion ?? '—' }}</dd></div>
      <div class="stat"><dt>Battery</dt><dd>{{ device.batteryPct != null ? device.batteryPct + '%' : '—' }}</dd></div>
      <div class="stat"><dt>RSSI</dt><dd style="font-size:0.9375rem">{{ device.rssi != null ? device.rssi : '—' }}</dd></div>
      <div class="stat"><dt>Uptime</dt><dd style="font-size:0.9375rem">{{ uptime(device.uptimeSec) }}</dd></div>
      <div class="stat"><dt>Last seen</dt><dd style="font-size:0.9375rem">{{ ago(device.lastSeen) }}</dd></div>
    </dl>

    <h2>Feature state</h2>
    <p class="note">
      <strong>Ships</strong> is what the flashed build claims. <strong>Active</strong> is what
      the firmware reported on boot. A mismatch is real information — it usually means the
      build went on but the feature failed to initialise.
    </p>
    <div class="tw">
      <table>
        <thead>
          <tr><th>Phase</th><th>Feature</th><th>Ships</th><th>Active</th><th>Detail</th></tr>
        </thead>
        <tbody>
          <tr v-for="f in features" :key="f.id">
            <td class="phase-tag nowrap">P{{ String(f.phase).padStart(2, '0') }}</td>
            <td class="m nowrap">{{ f.label }}</td>
            <td class="cell" :class="ships(f.id) ? 'yes' : 'no'">{{ ships(f.id) ? '●' : '○' }}</td>
            <td class="cell" :class="cellClass(f.id)">{{ active(f.id) ? '●' : ships(f.id) ? '✕' : '○' }}</td>
            <td style="color:var(--flux);font-size:0.75rem">{{ f.desc }}</td>
          </tr>
        </tbody>
      </table>
    </div>
    <p v-if="drift.length" class="note" style="color:var(--risk);margin-top:0.6rem">
      Drift on {{ drift.length }} feature{{ drift.length > 1 ? 's' : '' }}: shipped but not active.
    </p>

    <h2>Flash history</h2>
    <div v-if="!flashes.length" class="empty">No flashes recorded for this device.</div>
    <div v-else class="tw">
      <table>
        <thead>
          <tr><th>When</th><th>Version</th><th>Method</th><th>Target</th><th>Result</th><th>Took</th></tr>
        </thead>
        <tbody>
          <tr v-for="f in flashes" :key="f.id">
            <td class="m nowrap" style="color:var(--flux)">{{ new Date(f.at).toLocaleString() }}</td>
            <td class="m nowrap">{{ f.version ?? '—' }}</td>
            <td><span class="pill copper">{{ f.method }}</span></td>
            <td class="m nowrap" style="color:var(--flux)">{{ f.target ?? '—' }}</td>
            <td>
              <span class="pill" :class="resultClass(f.result)">{{ f.result }}</span>
              <div v-if="f.error" class="m" style="color:var(--risk);font-size:0.6875rem">{{ f.error }}</div>
            </td>
            <td class="m nowrap">{{ f.durationMs != null ? (f.durationMs / 1000).toFixed(1) + 's' : '—' }}</td>
          </tr>
        </tbody>
      </table>
    </div>

    <h2>
      <span class="row">
        Logs
        <button class="mini spacer" @click="refreshLogs()">Refresh</button>
        <button class="mini" @click="live = !live">{{ live ? 'Pause' : 'Live' }}</button>
      </span>
    </h2>
    <div v-if="!logs.length" class="empty">Nothing logged yet.</div>
    <div v-else class="logs">
      <div v-for="l in logs" :key="l.id" class="logline">
        <span class="t">{{ clock(l.at) }}</span>
        <span class="lv" :class="l.level">{{ l.level }}</span>
        <span class="tag">{{ l.tag ?? '' }}</span>
        <span class="msg">{{ l.msg }}</span>
      </div>
    </div>
  </div>

  <div v-else>
    <h1>Unknown device</h1>
    <p class="lede">No device with id <code>{{ id }}</code> has registered.</p>
    <NuxtLink to="/">← Fleet</NuxtLink>
  </div>
</template>

<script setup lang="ts">
const route = useRoute()
const id = route.params.id as string

const { data: devData }   = await useFetch('/api/devices')
const { data: flashData } = await useFetch('/api/flashes', { query: { deviceId: id } })
const { data: featData }  = await useFetch('/api/features')
const { data: buildData } = await useFetch('/api/builds')
const { data: logData, refresh: refreshLogs } =
  await useFetch('/api/logs', { query: { deviceId: id, limit: 300 } })

const device   = computed(() => devData.value?.devices?.find(d => d.id === id))
const flashes  = computed(() => flashData.value?.flashes ?? [])
const features = computed(() => featData.value?.features ?? [])
const logs     = computed(() => logData.value?.logs ?? [])

const build = computed(() =>
  buildData.value?.builds?.find(b => b.id === device.value?.buildId))

const ships  = (fid: string) => build.value?.features?.includes(fid) ?? false
const active = (fid: string) => device.value?.activeFeatures?.includes(fid) ?? false

const drift = computed(() =>
  features.value.filter(f => ships(f.id) && !active(f.id)))

function cellClass(fid: string): string {
  if (active(fid)) return 'yes'
  return ships(fid) ? 'drift' : 'no'
}

// Poll only while the tab is looking at it, and only when asked.
const live = ref(false)
let timer: ReturnType<typeof setInterval> | undefined
watch(live, on => {
  clearInterval(timer)
  if (on) timer = setInterval(() => refreshLogs(), 4000)
}, { immediate: true })
onUnmounted(() => clearInterval(timer))
</script>
