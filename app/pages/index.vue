<template>
  <div>
    <h1>Fleet</h1>
    <p class="lede">
      Every device that has ever registered, what firmware it is running, and every
      flash that put it there — USB and OTA in one history.
    </p>

    <dl class="stats">
      <div class="stat"><dt>Devices</dt><dd>{{ devices.length }}</dd></div>
      <div class="stat"><dt>Online</dt><dd>{{ onlineCount }}</dd></div>
      <div class="stat"><dt>Builds</dt><dd>{{ builds.length }}</dd></div>
      <div class="stat"><dt>Flashes 24h</dt><dd>{{ flashes24h }}</dd></div>
      <div class="stat">
        <dt>Failed flashes</dt>
        <dd :class="{ alert: failedFlashes > 0 }">{{ failedFlashes }}</dd>
      </div>
      <div class="stat">
        <dt>Errors 24h</dt>
        <dd :class="{ alert: errors24h > 0 }">{{ errors24h }}</dd>
      </div>
    </dl>

    <h2>Devices</h2>
    <div v-if="!devices.length" class="empty">
      No device has registered yet.<br>
      Flash firmware that <code>POST</code>s to <code>/api/devices/register</code>, or
      seed one with <code>npm run flash -- --port COM3 --bin &lt;file&gt;</code>.
    </div>
    <div v-else class="tw">
      <table>
        <thead>
          <tr>
            <th>Device</th><th>State</th><th>Firmware</th><th>Features</th>
            <th>Battery</th><th>RSSI</th><th>Uptime</th><th>Last seen</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="d in devices" :key="d.id">
            <td>
              <NuxtLink :to="`/devices/${d.id}`" class="m">{{ d.name }}</NuxtLink>
              <div class="m" style="color:var(--flux);font-size:0.6875rem">{{ d.mac }}</div>
            </td>
            <td>
              <span class="pill" :class="isOnline(d.lastSeen) ? 'ok' : 'idle'">
                {{ isOnline(d.lastSeen) ? 'online' : 'offline' }}
              </span>
            </td>
            <td class="m nowrap">{{ d.fwVersion ?? '—' }}</td>
            <td>
              <div class="row" style="gap:0.4rem">
                <div class="bar" style="width:4rem">
                  <span :style="{ width: pct(d.activeFeatures?.length ?? 0) }"></span>
                </div>
                <span class="m" style="font-size:0.6875rem">
                  {{ d.activeFeatures?.length ?? 0 }}/{{ totalFeatures }}
                </span>
              </div>
            </td>
            <td class="m nowrap">{{ d.batteryPct != null ? d.batteryPct + '%' : '—' }}</td>
            <td class="m nowrap">{{ d.rssi != null ? d.rssi + ' dBm' : '—' }}</td>
            <td class="m nowrap">{{ uptime(d.uptimeSec) }}</td>
            <td class="m nowrap" style="color:var(--flux)">{{ ago(d.lastSeen) }}</td>
          </tr>
        </tbody>
      </table>
    </div>

    <h2>Recent flashes</h2>
    <div v-if="!flashes.length" class="empty">Nothing flashed through the console yet.</div>
    <div v-else class="tw">
      <table>
        <thead>
          <tr><th>When</th><th>Device</th><th>Version</th><th>Method</th><th>Target</th><th>Result</th><th>Took</th></tr>
        </thead>
        <tbody>
          <tr v-for="f in flashes.slice(0, 15)" :key="f.id">
            <td class="m nowrap" style="color:var(--flux)">{{ ago(f.at) }}</td>
            <td class="m nowrap">{{ nameOf(f.deviceId) }}</td>
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

    <h2>Recent device logs</h2>
    <div v-if="!logs.length" class="empty">
      No logs. Devices batch lines to <code>POST /api/logs</code>.
    </div>
    <div v-else class="logs">
      <div v-for="l in logs.slice(0, 60)" :key="l.id" class="logline">
        <span class="t">{{ clock(l.at) }}</span>
        <span class="lv" :class="l.level">{{ l.level }}</span>
        <span class="tag">{{ l.tag ?? '' }}</span>
        <span class="msg">{{ l.msg }}</span>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const { data: devData }   = await useFetch('/api/devices')
const { data: buildData } = await useFetch('/api/builds')
const { data: flashData } = await useFetch('/api/flashes')
const { data: logData }   = await useFetch('/api/logs', { query: { limit: 60 } })
const { data: featData }  = await useFetch('/api/features')

const devices = computed(() => devData.value?.devices ?? [])
const builds  = computed(() => buildData.value?.builds ?? [])
const flashes = computed(() => flashData.value?.flashes ?? [])
const logs    = computed(() => logData.value?.logs ?? [])
const totalFeatures = computed(() => featData.value?.features?.length ?? 1)

const onlineCount = computed(() => devices.value.filter(d => isOnline(d.lastSeen)).length)

const DAY = 86_400_000
const flashes24h = computed(() =>
  flashes.value.filter(f => Date.now() - Date.parse(f.at) < DAY).length)
const failedFlashes = computed(() =>
  flashes.value.filter(f => f.result !== 'ok').length)
const errors24h = computed(() =>
  logs.value.filter(l => l.level === 'error' && Date.now() - Date.parse(l.at) < DAY).length)

const pct = (n: number) => `${Math.round((n / Math.max(1, totalFeatures.value)) * 100)}%`
const nameOf = (id: string) => devices.value.find(d => d.id === id)?.name ?? id
</script>
