<template>
  <button
    class="fixed top-4 right-4 z-[10000] w-9 h-9 flex items-center justify-center rounded-full text-sm transition-all duration-200"
    :class="isDark ? 'bg-white/10 text-white hover:bg-white/20' : 'bg-black/5 text-gray-600 hover:bg-black/10'"
    :title="isDark ? '切换浅色主题' : '切换深色主题'"
    @click="toggle"
  >
    {{ isDark ? '☀️' : '🌙' }}
  </button>
</template>

<script setup>
import { ref, watch } from 'vue'

const isDark = ref(false)

function toggle() {
  isDark.value = !isDark.value
}

watch(isDark, (val) => {
  document.documentElement.classList.toggle('dark', val)
  localStorage.setItem('theme', val ? 'dark' : 'light')
}, { immediate: true })

// 初始化主题
const saved = localStorage.getItem('theme')
if (saved === 'dark') {
  isDark.value = true
} else if (!saved && window.matchMedia('(prefers-color-scheme: dark)').matches) {
  isDark.value = true
}
</script>
