<template>
  <div class="control-bar" :style="barStyle">
    <!-- PS2 十字键 -->
    <div class="dpad-grid">
      <button
        v-for="btn in buttons.dpad"
        :key="btn.key"
        class="ps2-btn dpad-btn"
        :class="[btn.class, { active: activeKeys.has(btn.key) }]"
        @pointerdown.prevent="onBtnPress(btn)"
        @pointerup.prevent="onBtnRelease(btn.key)"
        @pointerleave.prevent="onBtnRelease(btn.key)"
        @pointercancel.prevent="onBtnRelease(btn.key)"
      >
        {{ btn.label }}
      </button>
    </div>

    <!-- Select / Start + 状态 -->
    <div class="flex flex-col items-center gap-0.5 mx-1">
      <div class="flex gap-1">
        <button
          v-for="btn in buttons.center"
          :key="btn.key"
          class="ps2-btn px-1.5 py-0.5 text-[9px] rounded-full"
          :class="[btn.color || '', { active: activeKeys.has(btn.key) }]"
          @pointerdown.prevent="onBtnPress(btn)"
          @pointerup.prevent="onBtnRelease(btn.key)"
          @pointerleave.prevent="onBtnRelease(btn.key)"
          @pointercancel.prevent="onBtnRelease(btn.key)"
        >
          {{ btn.label }}
        </button>
      </div>
      <div class="flex items-center gap-1 text-[9px]" style="color: var(--text-tertiary)">
        <span class="w-1.5 h-1.5 rounded-full" :class="deviceOnline ? 'bg-green-400' : 'bg-red-400'"></span>
        {{ deviceOnline ? '设备在线' : (wsConnected ? '设备离线' : '服务器未连接') }}
      </div>
    </div>

    <!-- 功能键 + 肩键 -->
    <div class="flex flex-col gap-0.5">
      <div class="flex justify-between px-0.5">
        <button
          v-for="btn in buttons.shoulders"
          :key="btn.key"
          class="ps2-btn px-1.5 py-px text-[9px] rounded"
          :class="{ active: activeKeys.has(btn.key) }"
          @pointerdown.prevent="onBtnPress(btn)"
          @pointerup.prevent="onBtnRelease(btn.key)"
          @pointerleave.prevent="onBtnRelease(btn.key)"
          @pointercancel.prevent="onBtnRelease(btn.key)"
        >
          {{ btn.label }}
        </button>
      </div>
      <div class="action-grid">
        <button
          v-for="btn in buttons.actions"
          :key="btn.key"
          class="ps2-btn action-btn text-xs"
          :class="[btn.color, { active: activeKeys.has(btn.key) }]"
          @pointerdown.prevent="onBtnPress(btn)"
          @pointerup.prevent="onBtnRelease(btn.key)"
          @pointerleave.prevent="onBtnRelease(btn.key)"
          @pointercancel.prevent="onBtnRelease(btn.key)"
        >
          {{ btn.label }}
        </button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { reactive, computed, onMounted, onUnmounted, watch } from 'vue'
import cfg from '@/config/config'
import { useControlChannel } from '@/composables/useControlChannel'
import { useSensorStore } from '@/composables/useSensorStore'

const props = defineProps({
  leftInset: { type: Number, default: 0 },
  rightInset: { type: Number, default: 0 },
})

const barStyle = computed(() => ({
  paddingLeft: `${props.leftInset + 14}px`,
  paddingRight: `${props.rightInset + 14}px`,
}))

const { sendCommand, connected: wsConnected } = useControlChannel()
const { online: deviceOnline } = useSensorStore()

const buttons = cfg.PS2_BUTTONS
const activeKeys = reactive(new Set())
const repeatTimers = new Map()
const flatButtons = Object.values(buttons).flat()
const buttonByName = new Map(
  flatButtons.filter(btn => btn.button).map(btn => [btn.button, btn]),
)
const directionButtons = new Set(['PAD_UP', 'PAD_RIGHT', 'PAD_DOWN', 'PAD_LEFT'])

function sendButtonState(button, state) {
  sendCommand('ps2_button', { button, state })
}

function onBtnPress(btn) {
  if (!btn.button || activeKeys.has(btn.key)) return

  if (directionButtons.has(btn.button)) {
    for (const activeKey of [...activeKeys]) {
      if (activeKey !== btn.key && directionButtons.has(activeKey)) {
        onBtnRelease(activeKey)
      }
    }
  }

  activeKeys.add(btn.key)
  sendButtonState(btn.button, 'down')

  const timer = setInterval(() => {
    sendButtonState(btn.button, 'down')
  }, 200)
  repeatTimers.set(btn.key, timer)
}

function onBtnRelease(key) {
  const timer = repeatTimers.get(key)
  if (timer) clearInterval(timer)
  repeatTimers.delete(key)

  if (!activeKeys.has(key)) return
  activeKeys.delete(key)

  const btn = buttonByName.get(key)
  if (btn) sendButtonState(btn.button, 'up')
}

const keyToBtnKey = {}
function keyboardKeyId(key) {
  return key.length === 1 ? key.toLowerCase() : key
}

function onKeyDown(e) {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return
  if (e.repeat) return
  const keyId = keyboardKeyId(e.key)
  const button = cfg.KEY_MAP[keyId]
  if (!button) return
  e.preventDefault()
  const btn = buttonByName.get(button)
  if (!btn) return
  keyToBtnKey[keyId] = btn.key
  onBtnPress(btn)
}

function onKeyUp(e) {
  const keyId = keyboardKeyId(e.key)
  const btnKey = keyToBtnKey[keyId]
  if (!btnKey) return
  e.preventDefault()
  onBtnRelease(btnKey)
  delete keyToBtnKey[keyId]
}

function releaseAllButtons() {
  for (const key of [...activeKeys]) onBtnRelease(key)
  for (const key of Object.keys(keyToBtnKey)) delete keyToBtnKey[key]
}

function onVisibilityChange() {
  if (document.visibilityState === 'hidden') releaseAllButtons()
}

watch(wsConnected, (connected) => {
  if (!connected) releaseAllButtons()
})

onMounted(() => {
  window.addEventListener('keydown', onKeyDown)
  window.addEventListener('keyup', onKeyUp)
  window.addEventListener('blur', releaseAllButtons)
  window.addEventListener('pagehide', releaseAllButtons)
  window.addEventListener('beforeunload', releaseAllButtons)
  document.addEventListener('visibilitychange', onVisibilityChange)
})
onUnmounted(() => {
  releaseAllButtons()
  window.removeEventListener('keydown', onKeyDown)
  window.removeEventListener('keyup', onKeyUp)
  window.removeEventListener('blur', releaseAllButtons)
  window.removeEventListener('pagehide', releaseAllButtons)
  window.removeEventListener('beforeunload', releaseAllButtons)
  document.removeEventListener('visibilitychange', onVisibilityChange)
})
</script>

<style scoped>
.control-bar {
  position: fixed;
  bottom: 0; left: 0; right: 0;
  z-index: 500;
  height: 90px;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  padding: 6px 14px;
  background: var(--control-bg);
  backdrop-filter: blur(24px) saturate(180%);
  -webkit-backdrop-filter: blur(24px) saturate(180%);
  border-top: 1px solid var(--glass-border);
  box-shadow: 0 -4px 24px rgba(0,0,0,0.08);
}

.dpad-grid {
  display: grid;
  grid-template-columns: 24px 24px 24px;
  grid-template-rows: 24px 24px 24px;
  gap: 1px;
  grid-template-areas: '. up .' 'left center right' '. down .';
}
.dpad-up { grid-area: up; }
.dpad-left { grid-area: left; }
.dpad-center { grid-area: center; background: transparent!important; border: none!important; cursor: default; }
.dpad-right { grid-area: right; }
.dpad-down { grid-area: down; }
.dpad-btn {
  width: 24px; height: 24px;
  font-size: 10px;
  border-radius: 6px;
}

.action-grid {
  display: grid;
  grid-template-columns: 26px 26px;
  grid-template-rows: 26px 26px;
  gap: 2px;
}
.action-btn {
  width: 26px; height: 26px;
  border-radius: 50% !important;
  font-size: 11px;
}
@media (max-width: 600px) {
  .control-bar {
    gap: 3px;
    padding: 4px 6px !important;
  }
}
</style>
