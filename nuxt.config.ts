export default defineNuxtConfig({
  compatibilityDate: '2026-08-10',

  css: ['~/assets/main.css'],

  nitro: {
    storage: {
      // Local dev + self-hosted: plain files under .data/db
      // On Vercel the filesystem is ephemeral — swap this driver for
      // 'vercel-kv' or 'upstash' and nothing else changes.
      db: { driver: 'fs', base: './.data/db' },
    },
  },

  runtimeConfig: {
    // Set DEVICE_TOKEN in .env. Device-facing routes reject requests without it.
    deviceToken: '',
  },

  devServer: { host: '0.0.0.0', port: 3000 },
})
