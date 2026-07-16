<template>
  <div class="control-bar">
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
        >
          {{ btn.label }}
        </button>
      </div>
      <div class="flex items-center gap-1 text-[9px]" style="color: var(--text-tertiary)">
        <span class="w-1.5 h-1.5 rounded-full" :class="wsConnected ? 'bg-green-400' : 'bg-red-400'"></span>
        {{ wsConnected ? '已连接' : '未连接' }}
      </div>
      <div class="text-[9px]" style="color: var(--text-tertiary)">速度: {{ speed }}%</div>
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
        >
          {{ btn.label }}
        </button>
      </div>
    </div>

    <!-- 分隔 + 键盘提示 -->
    <div class="h-7 w-px mx-1" style="background: var(--border-color)"></div>
    <div class="flex flex-col gap-0.5 text-[9px] leading-tight" style="color: var(--text-tertiary)">
      <span>WASD/方向键 移动</span>
      <span>Q/E 旋转 | R/F 调速</span>
      <span>Space 停止</span>
    </div>
  </div>
</template>

<script setup>
import { ref, reactive, onMounted, onUnmounted } from 'vue'
import cfg from '@/config/config'
import { useWebSocket } from '@/composables/useWebSocket'

const { send, connected: wsConnected } = useWebSocket('control', cfg.WS_MAIN)

const buttons = cfg.PS2_BUTTONS
const activeKeys = reactive(new Set())
const speed = ref(100)

// 按住时每200ms重复发送 (保持BW21心跳不超时)
const repeatTimers = {}

function sendCmd(command, extra = {}) {
  const payload = { type: 'command_request', cmd: command, params: { speed: speed.value, ...extra } }
  send(payload)
}

function isMoveCmd(cmd) {
  return cmd === 'move'
}

function onBtnPress(btn) {
  if (!btn.cmd) return
  activeKeys.add(btn.key)

  // 立即发送一次
  sendCmd(btn.cmd, btn.params || {})

  // 按住时每200ms重复发送 (维持BW21自动停止计时器)
  if (isMoveCmd(btn.cmd)) {
    clearInterval(repeatTimers[btn.key])
    repeatTimers[btn.key] = setInterval(() => {
      sendCmd(btn.cmd, btn.params || {})
    }, 200)
  }
}

function onBtnRelease(key) {
  activeKeys.delete(key)

  // 清除重复发送定时器
  if (repeatTimers[key]) {
    clearInterval(repeatTimers[key])
    delete repeatTimers[key]
  }

  // 方向键松开 → 发stop
  const flat = Object.values(buttons).flat()
  const btn = flat.find(b => b.key === key)
  if (btn && isMoveCmd(btn.cmd)) {
    sendCmd('stop')
  }
}

// 键盘按键映射到PS2按钮 (复用重复发送逻辑)
const keyToBtnKey = {}
function onKeyDown(e) {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return
  if (e.repeat) return  // 忽略OS自带重复事件，用我们自己的定时器
  const m = cfg.KEY_MAP[e.key]
  if (!m) return
  e.preventDefault()
  const flat = Object.values(buttons).flat()
  const btn = flat.find(b => {
    if (b.cmd !== m.cmd) return false
    if (m.params?.direction) return b.params?.direction === m.params.direction
    return !b.params?.direction
  })
  if (btn) {
    keyToBtnKey[e.key] = btn.key
    onBtnPress(btn)
  } else {
    // 非PS2键 (如q/e/r/f/space) 直接发一次
    sendCmd(m.cmd, m.params || {})
  }
}
function onKeyUp(e) {
  const btnKey = keyToBtnKey[e.key]
  if (btnKey) {
    onBtnRelease(btnKey)
    delete keyToBtnKey[e.key]
    return
  }
  // WASD/方向键直接映射 → 发stop
  const m = cfg.KEY_MAP[e.key]
  if (m && m.cmd === 'move') {
    sendCmd('stop')
  }
}

onMounted(() => {
  window.addEventListener('keydown', onKeyDown)
  window.addEventListener('keyup', onKeyUp)
})
onUnmounted(() => {
  window.removeEventListener('keydown', onKeyDown)
  window.removeEventListener('keyup', onKeyUp)
})
</script>

<style scoped>
.control-bar {
  position: fixed;
  bottom: 0; left: 0; right: 0;
  z-index: 5000;
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
</style>
