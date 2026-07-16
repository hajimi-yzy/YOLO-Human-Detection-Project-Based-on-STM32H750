<template>
  <div class="dashboard w-full h-full relative" :style="{ background: 'url(/2907bd408ff232736c4b2ab15c2a9ff2.jpg) center / cover no-repeat' }">
    <LeftPanel
      :window-items="windowItems"
      :is-dark="isDark"
      :layout-mode="layoutMode"
      @toggle-window="toggleWindow"
      @toggle-theme="toggleTheme"
      @toggle-layout="toggleLayoutMode"
    />

    <div class="work-area" :style="workAreaStyle">
      <SensorWindow
        v-if="windows.sensor.created"
        ref="sensorRef"
        :visible="windows.sensor.visible"
        :initial-x="defaultPos.sensor.x"
        :initial-y="defaultPos.sensor.y"
        :initial-w="defaultPos.sensor.w"
        :initial-h="defaultPos.sensor.h"
        :draggable="layoutMode !== 'split'"
        :resizable="false"
        @close="windows.sensor.visible = false"
        @minimize="windows.sensor.visible = false"
        @drag-end="(pos) => onDragEnd('sensor', pos)"
      />
      <VideoWindow
        v-if="windows.video.created"
        ref="videoRef"
        :visible="windows.video.visible"
        :initial-x="defaultPos.video.x"
        :initial-y="defaultPos.video.y"
        :initial-w="defaultPos.video.w"
        :initial-h="defaultPos.video.h"
        :draggable="layoutMode !== 'split'"
        :resizable="false"
        @close="windows.video.visible = false"
        @minimize="windows.video.visible = false"
        @drag-end="(pos) => onDragEnd('video', pos)"
      />
      <MapWindow
        v-if="windows.map.created"
        ref="mapRef"
        :visible="windows.map.visible"
        :initial-x="defaultPos.map.x"
        :initial-y="defaultPos.map.y"
        :initial-w="defaultPos.map.w"
        :initial-h="defaultPos.map.h"
        :draggable="layoutMode !== 'split'"
        :resizable="false"
        @close="windows.map.visible = false"
        @minimize="windows.map.visible = false"
        @drag-end="(pos) => onDragEnd('map', pos)"
      />

      <!-- 分屏模式：垂直分割条（video ↔ map） -->
      <div
        v-if="layoutMode === 'split' && windows.video.visible && windows.map.visible"
        class="splitter splitter-v"
        :class="{ active: splitterVDragging }"
        :style="splitterVStyle"
        @pointerdown.prevent="onSplitterVDown"
      ></div>

      <!-- 分屏模式：水平分割条（上方区域 ↔ sensor） -->
      <div
        v-if="layoutMode === 'split' && (windows.video.visible || windows.map.visible) && windows.sensor.visible"
        class="splitter splitter-h"
        :class="{ active: splitterHDragging }"
        :style="splitterHStyle"
        @pointerdown.prevent="onSplitterHDown"
      ></div>
    </div>

    <ControlPanel />
  </div>
</template>

<script setup>
import { reactive, ref, computed, watch, onMounted, onUnmounted, nextTick } from 'vue'
import LeftPanel from '@/components/LeftPanel.vue'
import SensorWindow from '@/components/SensorWindow.vue'
import VideoWindow from '@/components/VideoWindow.vue'
import MapWindow from '@/components/MapWindow.vue'
import ControlPanel from '@/components/ControlPanel.vue'

// ==================== 窗口可见性 ====================
const windows = reactive({
  sensor: { key: 'sensor', visible: true, created: true },
  video: { key: 'video', visible: true, created: true },
  map: { key: 'map', visible: true, created: true },
})

const windowItems = computed(() => [
  { key: 'sensor', label: '传感窗口', active: windows.sensor.visible },
  { key: 'video', label: '视频窗口', active: windows.video.visible },
  { key: 'map', label: '地图窗口', active: windows.map.visible },
])

function toggleWindow(key) {
  windows[key].visible = !windows[key].visible
}

// ==================== 主题 ====================
const isDark = ref(false)
function toggleTheme() {
  isDark.value = !isDark.value
}
watch(isDark, (val) => {
  document.documentElement.classList.toggle('dark', val)
  localStorage.setItem('theme', val ? 'dark' : 'light')
}, { immediate: true })
const saved = localStorage.getItem('theme')
if (saved === 'dark') isDark.value = true
else if (!saved && window.matchMedia('(prefers-color-scheme: dark)').matches) isDark.value = true

// ==================== 布局常量 ====================
const LEFT_PANEL_W = 190
const CTRL_PANEL_H = 90
const GAP = 8
const MIN_W = 360
const MIN_H = 280

const screenW = ref(window.innerWidth)
const screenH = ref(window.innerHeight)
function onResize() { screenW.value = window.innerWidth; screenH.value = window.innerHeight }
onMounted(() => window.addEventListener('resize', onResize))
onUnmounted(() => window.removeEventListener('resize', onResize))

const workAreaStyle = computed(() => ({
  position: 'absolute',
  left: `${LEFT_PANEL_W}px`,
  top: 0,
  width: `${screenW.value - LEFT_PANEL_W}px`,
  height: `${screenH.value - CTRL_PANEL_H}px`,
  pointerEvents: 'none',
}))

const availW = computed(() => screenW.value - LEFT_PANEL_W)
const availH = computed(() => screenH.value - CTRL_PANEL_H)

// ==================== 布局模式 ====================
const layoutMode = ref('split')

function toggleLayoutMode() {
  layoutMode.value = layoutMode.value === 'free' ? 'split' : 'free'
}

// ==================== 默认初始位置 ====================
const defaultPos = computed(() => {
  const waW = screenW.value - LEFT_PANEL_W
  const waH = screenH.value - CTRL_PANEL_H
  const vw = Math.max(360, Math.round(waW * 0.4))
  const vh = Math.max(280, Math.round(waH * 0.5))
  const mw = Math.max(360, Math.round(waW * 0.4))
  const mh = Math.max(280, Math.round(waH * 0.5))
  const sw = Math.max(420, Math.round(waW * 0.55))
  const sh = Math.max(300, Math.round(waH * 0.38))
  return {
    video: { x: 20, y: 16, w: vw, h: vh },
    map: { x: waW - mw - 20, y: 50, w: mw, h: mh },
    sensor: { x: Math.round((waW - sw) / 2), y: Math.round(waH * 0.55), w: sw, h: sh },
  }
})

// ==================== 分屏布局 ====================
function computeSplitLayout() {
  const aw = availW.value
  const ah = availH.value
  const vv = windows.video.visible
  const mv = windows.map.visible
  const sv = windows.sensor.visible
  const hasTop = vv || mv
  const result = {}
  if (!hasTop && !sv) return result

  if (hasTop && sv) {
    const topH = Math.round(ah * 0.55)
    const botH = ah - topH - GAP
    if (vv && mv) {
      const halfW = Math.round((aw - GAP) / 2)
      result.video = { x: 0, y: 0, w: halfW, h: topH }
      result.map = { x: halfW + GAP, y: 0, w: aw - halfW - GAP, h: topH }
    } else if (vv) {
      result.video = { x: 0, y: 0, w: aw, h: topH }
    } else if (mv) {
      result.map = { x: 0, y: 0, w: aw, h: topH }
    }
    result.sensor = { x: 0, y: topH + GAP, w: aw, h: botH }
  } else if (hasTop && !sv) {
    if (vv && mv) {
      const halfW = Math.round((aw - GAP) / 2)
      result.video = { x: 0, y: 0, w: halfW, h: ah }
      result.map = { x: halfW + GAP, y: 0, w: aw - halfW - GAP, h: ah }
    } else if (vv) {
      result.video = { x: 0, y: 0, w: aw, h: ah }
    } else if (mv) {
      result.map = { x: 0, y: 0, w: aw, h: ah }
    }
  } else if (sv && !hasTop) {
    result.sensor = { x: 0, y: 0, w: aw, h: ah }
  }
  return result
}

function applySplitLayout() {
  const layout = computeSplitLayout()
  for (const key of ['video', 'map', 'sensor']) {
    const l = layout[key]
    const ref = getRef(key)
    if (!ref || !windows[key].visible) continue
    if (l) {
      ref.setPosition(l.x, l.y)
      ref.setSize(l.w, l.h)
      winState[key].x = l.x
      winState[key].y = l.y
      winState[key].w = l.w
      winState[key].h = l.h
    }
  }
}

// ==================== 窗口状态追踪 ====================
const winState = reactive({
  video: { x: 0, y: 0, w: 0, h: 0 },
  map: { x: 0, y: 0, w: 0, h: 0 },
  sensor: { x: 0, y: 0, w: 0, h: 0 },
})

watch(defaultPos, (dp) => {
  for (const key of ['video', 'map', 'sensor']) {
    if (winState[key].w === 0) {
      winState[key].x = dp[key].x
      winState[key].y = dp[key].y
      winState[key].w = dp[key].w
      winState[key].h = dp[key].h
    }
  }
}, { immediate: true })

// ==================== 窗口引用 ====================
const videoRef = ref(null)
const mapRef = ref(null)
const sensorRef = ref(null)
const refs = { video: videoRef, map: mapRef, sensor: sensorRef }

function getRef(key) { return refs[key]?.value }

// ==================== 拖拽结束（仅自由模式） ====================
function onDragEnd(key, pos) {
  winState[key].x = pos.x
  winState[key].y = pos.y
}

// ==================== 分割条：垂直（video ↔ map） ====================
const splitterVDragging = ref(false)

const splitterVStyle = computed(() => {
  if (!windows.video.visible || !windows.map.visible) return { display: 'none' }
  const v = winState.video
  return {
    left: `${v.x + v.w}px`,
    top: `${v.y}px`,
    width: `${GAP}px`,
    height: `${v.h}px`,
    pointerEvents: 'auto',
  }
})

function onSplitterVDown(e) {
  splitterVDragging.value = true
  const startX = e.clientX
  const vw0 = winState.video.w
  const mx0 = winState.map.x
  const mw0 = winState.map.w
  const aw = availW.value

  const onMove = (ev) => {
    let dx = ev.clientX - startX
    dx = Math.max(MIN_W - vw0, Math.min(aw - vw0 - GAP - MIN_W, dx))
    const newVW = vw0 + dx
    const newMX = vw0 + dx + GAP
    const newMW = aw - newMX

    winState.video.w = newVW
    winState.map.x = newMX
    winState.map.w = newMW

    const vRef = getRef('video')
    if (vRef) vRef.setSize(newVW, winState.video.h)
    const mRef = getRef('map')
    if (mRef) { mRef.setPosition(newMX, winState.map.y); mRef.setSize(newMW, winState.map.h) }
  }
  const onUp = () => {
    splitterVDragging.value = false
    document.removeEventListener('pointermove', onMove)
    document.removeEventListener('pointerup', onUp)
  }
  document.addEventListener('pointermove', onMove)
  document.addEventListener('pointerup', onUp)
}

// ==================== 分割条：水平（上方 ↔ sensor） ====================
const splitterHDragging = ref(false)

const splitterHStyle = computed(() => {
  if (!windows.sensor.visible) return { display: 'none' }
  const topKey = windows.video.visible ? 'video' : (windows.map.visible ? 'map' : null)
  if (!topKey) return { display: 'none' }
  const top = winState[topKey]
  return {
    left: '0px',
    top: `${top.y + top.h}px`,
    width: `${availW.value}px`,
    height: `${GAP}px`,
    pointerEvents: 'auto',
  }
})

function onSplitterHDown(e) {
  splitterHDragging.value = true
  const startY = e.clientY
  const topKey = windows.video.visible ? 'video' : 'map'
  const topH0 = winState[topKey].h
  const sY0 = winState.sensor.y
  const sH0 = winState.sensor.h
  const ah = availH.value

  const onMove = (ev) => {
    let dy = ev.clientY - startY
    dy = Math.max(MIN_H - topH0, Math.min(ah - topH0 - GAP - MIN_H, dy))
    const newTopH = topH0 + dy
    const newSY = topH0 + dy + GAP
    const newSH = ah - newSY

    for (const k of ['video', 'map']) {
      if (!windows[k].visible) continue
      winState[k].h = newTopH
      const ref = getRef(k)
      if (ref) ref.setSize(winState[k].w, newTopH)
    }
    winState.sensor.y = newSY
    winState.sensor.h = newSH
    const sRef = getRef('sensor')
    if (sRef) { sRef.setPosition(winState.sensor.x, newSY); sRef.setSize(winState.sensor.w, newSH) }
  }
  const onUp = () => {
    splitterHDragging.value = false
    document.removeEventListener('pointermove', onMove)
    document.removeEventListener('pointerup', onUp)
  }
  document.addEventListener('pointermove', onMove)
  document.addEventListener('pointerup', onUp)
}

// ==================== 挂载 → 应用分屏布局 ====================
onMounted(async () => {
  await nextTick()
  await nextTick()
  if (layoutMode.value === 'split') applySplitLayout()
})

// ==================== 模式切换 ====================
watch(layoutMode, async (mode) => {
  await nextTick()
  if (mode === 'split') applySplitLayout()
})

// ==================== 可见性变化 → 重排 ====================
watch(
  () => [windows.video.visible, windows.map.visible, windows.sensor.visible],
  async () => {
    if (layoutMode.value === 'split') {
      await nextTick()
      applySplitLayout()
    }
  }
)
</script>

<style scoped>
.splitter {
  position: absolute;
  z-index: 250;
  pointer-events: auto;
  transition: background 0.15s;
}
.splitter-v {
  cursor: col-resize;
}
.splitter-h {
  cursor: row-resize;
}
.splitter:hover,
.splitter.active {
  background: rgba(0, 122, 255, 0.25);
  border-radius: 2px;
}
</style>
