<template>
  <div>
    <header class="topbar">
      <div class="inner">
        <span class="brand">translator<span class="dot">·</span>console</span>
        <nav>
          <NuxtLink to="/" :class="{ on: $route.path === '/' }">Fleet</NuxtLink>
          <NuxtLink to="/builds" :class="{ on: $route.path === '/builds' }">Builds</NuxtLink>
        </nav>
      </div>
    </header>
    <div class="wrap">
      <NuxtPage />
    </div>
  </div>
</template>

<script setup lang="ts">
useHead({
  title: 'translator·console',
  meta: [{ name: 'color-scheme', content: 'light dark' }],
})
</script>
