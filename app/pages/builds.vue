<template>
  <div>
    <h1>Builds</h1>
    <p class="lede">
      Every firmware image the flash script has registered, and which features each one
      ships. Builds are keyed by version + channel, so reflashing the same version updates
      the record rather than duplicating it.
    </p>

    <div v-if="!builds.length" class="empty">
      No builds registered. <code>npm run flash</code> creates one automatically from its
      <code>--version</code> and <code>--features</code> arguments.
    </div>

    <template v-else>
      <div class="tw">
        <table>
          <thead>
            <tr><th>Version</th><th>Channel</th><th>Git</th><th>Size</th><th>sha256</th><th>Features</th><th>Created</th><th>On devices</th></tr>
          </thead>
          <tbody>
            <tr v-for="b in builds" :key="b.id">
              <td class="m nowrap" style="font-weight:600">{{ b.version }}</td>
              <td><span class="pill" :class="channelClass(b.channel)">{{ b.channel }}</span></td>
              <td class="m nowrap" style="color:var(--flux)">{{ b.gitSha?.slice(0, 7) ?? '—' }}</td>
              <td class="m nowrap">{{ bytes(b.sizeBytes) }}</td>
              <td class="m nowrap" style="color:var(--flux)">{{ b.sha256?.slice(0, 12) ?? '—' }}</td>
              <td class="m nowrap">{{ b.features.length }}</td>
              <td class="m nowrap" style="color:var(--flux)">{{ ago(b.createdAt) }}</td>
              <td class="m nowrap">{{ deviceCount(b.id) }}</td>
            </tr>
          </tbody>
        </table>
      </div>

      <h2>Feature matrix</h2>
      <p class="note">
        Rows are features in plan order, columns are builds. This is the view that answers
        "which image has the thing I'm testing".
      </p>
      <div class="tw">
        <table>
          <thead>
            <tr>
              <th>Phase</th>
              <th>Feature</th>
              <th v-for="b in builds" :key="b.id" class="m">{{ b.version }}</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="f in features" :key="f.id">
              <td class="phase-tag nowrap">P{{ String(f.phase).padStart(2, '0') }}</td>
              <td class="m nowrap" :title="f.desc">{{ f.label }}</td>
              <td
                v-for="b in builds"
                :key="b.id"
                class="cell"
                :class="b.features.includes(f.id) ? 'yes' : 'no'"
              >{{ b.features.includes(f.id) ? '●' : '○' }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </template>

    <h2>Feature registry</h2>
    <p class="note">
      Defined in <code>server/utils/features.ts</code>. Unknown ids are dropped on write, so
      a typo in a flash command can't invent a phantom feature.
    </p>
    <div class="tw">
      <table>
        <thead><tr><th>Phase</th><th>Id</th><th>Label</th><th>Description</th></tr></thead>
        <tbody>
          <tr v-for="f in features" :key="f.id">
            <td class="phase-tag nowrap">P{{ String(f.phase).padStart(2, '0') }}</td>
            <td class="m nowrap" style="color:var(--copper)">{{ f.id }}</td>
            <td class="nowrap">{{ f.label }}</td>
            <td style="color:var(--flux);font-size:0.75rem">{{ f.desc }}</td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<script setup lang="ts">
const { data: buildData } = await useFetch('/api/builds')
const { data: featData }  = await useFetch('/api/features')
const { data: devData }   = await useFetch('/api/devices')

const builds   = computed(() => buildData.value?.builds ?? [])
const features = computed(() => featData.value?.features ?? [])
const devices  = computed(() => devData.value?.devices ?? [])

const deviceCount = (buildId: string) =>
  devices.value.filter(d => d.buildId === buildId).length

const channelClass = (c: string) =>
  c === 'stable' ? 'ok' : c === 'beta' ? 'warn' : 'idle'
</script>
