<template>
  <aside class="fixed left-0 top-0 z-[300] flex flex-col gap-2.5 p-2.5 overflow-y-auto" :style="panelStyle">
    <!-- 标题 -->
    <div class="flex items-center gap-1.5 pb-2" style="border-bottom: 1px solid var(--border-color)">
      <img src="/retouch_2026060418314442.png" class="w-5 h-5 object-contain" />
      <span class="text-xs font-semibold" style="color: var(--text-primary)">机器人控制</span>
    </div>

    <!-- 窗口启停开关 -->
    <div class="flex flex-col gap-1.5">
      <div class="text-[10px] font-medium" style="color: var(--text-tertiary)">窗口控制</div>
      <label
        v-for="item in windowItems"
        :key="item.key"
        class="flex items-center justify-between py-0.5 px-1 rounded-md cursor-pointer hover:bg-black/5 dark:hover:bg-white/5"
      >
        <span class="text-[11px]" style="color: var(--text-secondary)">{{ item.label }}</span>
        <!-- iOS 风格开关 -->
        <span class="ios-toggle" :class="{ active: item.active }" @click="$emit('toggleWindow', item.key)">
          <span class="ios-toggle-knob"></span>
        </span>
      </label>
    </div>

    <!-- 布局模式 -->
    <div class="flex flex-col gap-1.5">
      <div class="text-[10px] font-medium" style="color: var(--text-tertiary)">布局模式</div>
      <label
        class="flex items-center justify-between py-0.5 px-1 rounded-md cursor-pointer hover:bg-black/5 dark:hover:bg-white/5"
        @click="$emit('toggleLayout')"
      >
        <span class="text-[11px]" style="color: var(--text-secondary)">{{ layoutMode === 'split' ? '分屏模式' : '小窗模式' }}</span>
        <span class="ios-toggle" :class="{ active: layoutMode === 'split' }">
          <span class="ios-toggle-knob"></span>
        </span>
      </label>
    </div>

    <div style="border-bottom: 1px solid var(--border-color)"></div>

    <!-- 实时数据 -->
    <div class="flex flex-col gap-1.5">
      <div class="text-[10px] font-medium" style="color: var(--text-tertiary)">实时数据</div>

      <!-- 温度/湿度/海拔 -->
      <div class="data-card flex justify-between items-center !p-2">
        <span class="text-[11px]" style="color: var(--text-secondary)">温度</span>
        <span class="text-xs font-bold" style="color: var(--text-primary)">
          {{ formatValue(sensorData.temperature, '°C') }}
        </span>
      </div>
      <div class="data-card flex justify-between items-center !p-2">
        <span class="text-[11px]" style="color: var(--text-secondary)">湿度</span>
        <span class="text-xs font-bold" style="color: var(--text-primary)">
          {{ formatValue(sensorData.humidity, '%') }}
        </span>
      </div>
      <div class="data-card flex justify-between items-center !p-2">
        <span class="text-[11px]" style="color: var(--text-secondary)">海拔</span>
        <span class="text-xs font-bold" style="color: var(--text-primary)">
          {{ formatValue(sensorData.altitude, 'm') }}
        </span>
      </div>
      <div class="data-card flex justify-between items-center !p-2">
        <span class="text-[11px]" style="color: var(--text-secondary)">气压</span>
        <span class="text-xs font-bold" style="color: var(--text-primary)">
          {{ formatValue(sensorData.pressure, 'hPa') }}
        </span>
      </div>
      <div class="data-card flex items-center justify-between gap-2 !p-2">
        <div class="text-[11px]" style="color: var(--text-secondary)">可燃气体</div>
        <div class="flex items-center gap-1.5 flex-shrink-0">
          <span class="gas-led" :class="gasLedClass"></span>
          <span class="text-[10px] font-medium" :class="gasTextClass">{{ gasStateText }}</span>
        </div>
      </div>
    </div>

    <div style="border-bottom: 1px solid var(--border-color)"></div>

    <!-- 机器人配置 -->
    <div class="flex flex-col gap-2">
      <div class="text-[10px] font-medium" style="color: var(--text-tertiary)">机器人配置</div>
      <div class="flex flex-col gap-1.5">
        <div class="data-card flex flex-col gap-1.5 !p-2">
          <div class="flex items-center justify-between gap-2">
            <div class="min-w-0">
              <div class="text-[11px]" style="color: var(--text-secondary)">AP 热点串流</div>
              <div class="flex items-center gap-1 mt-0.5">
                <span class="ap-state-led" :class="apLedClass"></span>
                <span class="text-[9px] truncate" :class="apTextClass">{{ apStateText }}</span>
              </div>
            </div>
            <button
              class="ios-toggle"
              :class="{ active: apStreamEnabled, disabled: !canToggleAp }"
              type="button"
              role="switch"
              :aria-checked="apStreamEnabled"
              :aria-label="apStreamEnabled ? '关闭 AP 热点串流' : '开启 AP 热点串流'"
              :disabled="!canToggleAp"
              @click="toggleApStream"
            >
              <span class="ios-toggle-knob"></span>
            </button>
          </div>
          <a
            v-if="localApStreamAvailable"
            class="local-stream-link"
            :href="LOCAL_AP_STREAM_URL"
            target="_blank"
            rel="noopener noreferrer"
            title="仅连接 ESP-L610-4G 热点时可访问"
          >
            <span>本地视频</span>
            <span>192.168.4.1:8080/live/mjpeg</span>
          </a>
          <div v-else class="local-stream-link local-stream-link-disabled">
            <span>本地视频</span>
            <span>192.168.4.1:8080/live/mjpeg</span>
          </div>
        </div>
      </div>
    </div>

    <!-- 气体报警弹窗 -->
    <Teleport to="body">
      <div v-if="gasAlarm && showGasPopup && !(personDetected && showPersonPopup)" class="popup-overlay" @click="dismissGasAlarm">
        <div class="popup-card popup-gas" @click.stop>
          <div class="text-3xl mb-2">⚠️</div>
          <div class="text-lg font-bold text-red-500 mb-1">可燃气体报警!</div>
          <button class="mt-3 px-4 py-1 rounded-full text-xs font-medium bg-red-500 text-white hover:bg-red-600 transition-colors" @click="dismissGasAlarm">确 认</button>
        </div>
      </div>
    </Teleport>

    <!-- 人体识别弹窗 -->
    <Teleport to="body">
      <div v-if="personDetected && showPersonPopup" class="popup-overlay" @click="dismissPersonAlarm">
        <div class="popup-card popup-person" @click.stop>
          <div class="text-3xl mb-2">🧑</div>
          <div class="text-lg font-bold text-blue-500 mb-1">识别到疑似幸存者!</div>
          <button class="mt-3 px-4 py-1 rounded-full text-xs font-medium bg-blue-500 text-white hover:bg-blue-600 transition-colors" @click="dismissPersonAlarm">确 认</button>
        </div>
      </div>
    </Teleport>

    <div style="border-bottom: 1px solid var(--border-color)"></div>

    <!-- 底部状态 -->
    <div class="flex flex-col gap-1.5">
      <div class="flex items-center gap-1.5 text-[10px]" style="color: var(--text-tertiary)">
        <span class="w-1.5 h-1.5 rounded-full" :class="deviceOnline ? 'bg-green-400' : 'bg-red-400'"></span>
        {{ deviceOnline ? '设备在线' : (wsConnected ? '设备离线' : '服务器未连接') }}
      </div>
      <button
        class="flex items-center gap-1.5 text-[10px] px-1.5 py-0.5 rounded-md transition-colors hover:bg-black/5 dark:hover:bg-white/5"
        style="color: var(--text-secondary)"
        @click="$emit('toggleTheme')"
      >
        <span>{{ isDark ? '☀️ 浅色' : '🌙 深色' }}</span>
      </button>
    </div>
  </aside>
</template>

<script setup>
import { ref, computed, watch } from 'vue'
import { useSensorStore } from '@/composables/useSensorStore'
import { useControlChannel } from '@/composables/useControlChannel'

defineEmits(['toggleWindow', 'toggleTheme', 'toggleLayout'])
const props = defineProps({
  width: { type: Number, default: 190 },
  windowItems: { type: Array, required: true },
  isDark: { type: Boolean, default: false },
  layoutMode: { type: String, default: 'free' },
})

const LOCAL_AP_STREAM_URL = 'http://192.168.4.1:8080/live/mjpeg'

const { connected: wsConnected, online: deviceOnline, sensorData } = useSensorStore()
const {
  connected: controlConnected,
  deviceOnline: apDeviceOnline,
  apStream,
  apRequest,
  apStreamEnabled,
  apStreamBusy,
  requestApStream,
} = useControlChannel()
const localApStreamAvailable = computed(() => (
  controlConnected.value
  && apStream.value.supported === true
  && apStreamEnabled.value
))

function formatValue(value, unit) {
  return value == null ? 'NA' : `${value}${unit}`
}

const showGasPopup = ref(false)
const showPersonPopup = ref(false)
const gasDismissed = ref(false)
const personDismissed = ref(false)

const gas = computed(() => sensorData.value?.gas ?? null)
function normalizeGasAlarm(value) {
  const alarm = value && typeof value === 'object' ? value.alarm : value
  if (alarm === true || alarm === 1) return true
  if (alarm === false || alarm === 0) return false
  return null
}

const gasFlag = computed(() => {
  const value = normalizeGasAlarm(gas.value)
  if (value === true || value === 1) return true
  if (value === false || value === 0) return false
  return null
})
const gasAlarm = computed(() => gasFlag.value === true)
const personDetected = computed(() => sensorData.value?.person_detected === 1)
const gasStateKnown = computed(() => (
  deviceOnline.value && gasFlag.value !== null
))
const gasStateText = computed(() => {
  if (!gasStateKnown.value) return 'NA'
  return gasAlarm.value ? '报警' : '正常'
})
const gasLedClass = computed(() => {
  if (!gasStateKnown.value) return 'led-offline'
  return gasAlarm.value ? 'led-red' : 'led-green'
})
const gasTextClass = computed(() => {
  if (!gasStateKnown.value) return 'text-gray-400'
  return gasAlarm.value ? 'text-red-500' : 'text-green-500'
})

const canToggleAp = computed(() => (
  controlConnected.value
  && apDeviceOnline.value === true
  && apStream.value.supported === true
  && !apStreamBusy.value
))
const apStateText = computed(() => {
  if (!controlConnected.value) return '服务器未连接'
  if (apDeviceOnline.value === false) return '设备离线'
  if (apDeviceOnline.value !== true) return '等待设备状态'
  if (apRequest.value.status === 'rejected') return apRequest.value.error || '请求被拒绝'
  if (apRequest.value.status === 'timeout') return apRequest.value.error || '设备确认超时'
  if (apRequest.value.pending) {
    return apRequest.value.status === 'waiting' ? '等待设备确认' : '请求已排队'
  }
  if (apStream.value.supported === false) return '固件不支持'
  if (apStream.value.supported !== true) return '状态未报告'
  const labels = {
    starting: '正在开启',
    stopping: '正在关闭',
    enabled: '串流已开启',
    disabled: '串流已关闭',
    error: apStream.value.error ? `失败：${apStream.value.error}` : '操作失败',
  }
  return labels[apStream.value.state] ?? '状态未报告'
})
const apLedClass = computed(() => {
  if (apDeviceOnline.value === false) return 'ap-led-red'
  if (!controlConnected.value || apDeviceOnline.value !== true) return 'ap-led-offline'
  if (apRequest.value.status === 'rejected' || apRequest.value.status === 'timeout') return 'ap-led-red'
  if (apRequest.value.pending) return 'ap-led-amber'
  if (apStream.value.state === 'enabled') return 'ap-led-green'
  if (apStreamBusy.value) return 'ap-led-amber'
  if (apStream.value.state === 'error') return 'ap-led-red'
  return 'ap-led-offline'
})
const apTextClass = computed(() => {
  if (
    apDeviceOnline.value === false
    || apStream.value.state === 'error'
    || apRequest.value.status === 'rejected'
    || apRequest.value.status === 'timeout'
  ) return 'text-red-500'
  if (apRequest.value.pending) return 'text-amber-500'
  if (apStream.value.state === 'enabled') return 'text-green-500'
  if (apStreamBusy.value) return 'text-amber-500'
  return 'text-gray-400'
})

function toggleApStream() {
  if (!canToggleAp.value) return
  requestApStream(!apStreamEnabled.value)
}

function dismissGasAlarm() {
  showGasPopup.value = false
  gasDismissed.value = true
}

function dismissPersonAlarm() {
  showPersonPopup.value = false
  personDismissed.value = true
}

// 每次收到新传感器数据都检查弹窗 (确认后不再重复弹, 等报警消除才重置)
watch(sensorData, (data) => {
  const gasAlarmValue = normalizeGasAlarm(data?.gas)
  if (gasAlarmValue === false) gasDismissed.value = false
  if (data?.person_detected === 0) personDismissed.value = false

  if (gasAlarmValue === true && !showGasPopup.value && !gasDismissed.value) showGasPopup.value = true
  if (data?.person_detected === 1 && !showPersonPopup.value && !personDismissed.value) showPersonPopup.value = true
})

const panelStyle = computed(() => ({
  width: `${props.width}px`,
  height: 'calc(100vh - 90px)',
  background: 'var(--glass-bg)',
  backdropFilter: 'blur(20px) saturate(180%)',
  WebkitBackdropFilter: 'blur(20px) saturate(180%)',
  borderRight: '1px solid var(--glass-border)',
}))
</script>

<style scoped>
/* iOS 风格开关 */
.ios-toggle {
  position: relative;
  display: inline-block;
  width: 40px;
  height: 22px;
  padding: 0;
  border: 0;
  border-radius: 22px;
  background: #e0e0e0;
  cursor: pointer;
  transition: background 0.2s ease;
  flex-shrink: 0;
}
.ios-toggle.active {
  background: #34C759;
}
.ios-toggle-knob {
  position: absolute;
  top: 2px;
  left: 2px;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: #fff;
  box-shadow: 0 1px 3px rgba(0,0,0,0.2);
  transition: transform 0.2s ease;
}
.ios-toggle.active .ios-toggle-knob {
  transform: translateX(18px);
}
.ios-toggle.disabled {
  opacity: 0.45;
  cursor: not-allowed;
}
.dark .ios-toggle { background: #48484a; }
.dark .ios-toggle.active { background: #34C759; }

/* 气体状态 LED */
.gas-led {
  display: inline-block;
  width: 10px; height: 10px;
  border-radius: 50%;
}
.led-green {
  background: #34C759;
  box-shadow: 0 0 6px #34C759;
  animation: led-blink-slow 1.5s ease-in-out infinite;
}
.led-red {
  background: #ff3b30;
  box-shadow: 0 0 8px #ff3b30;
  animation: led-blink-fast 0.3s ease-in-out infinite;
}
.led-offline {
  background: #9ca3af;
  box-shadow: none;
}
.ap-state-led {
  display: inline-block;
  width: 8px;
  height: 8px;
  flex: 0 0 8px;
  border-radius: 50%;
}
.ap-led-green {
  background: #34c759;
  box-shadow: 0 0 6px #34c759;
  animation: led-blink-slow 1.8s ease-in-out infinite;
}
.ap-led-amber {
  background: #f59e0b;
  box-shadow: 0 0 6px #f59e0b;
  animation: led-blink-slow 0.8s ease-in-out infinite;
}
.ap-led-red {
  background: #ff3b30;
  box-shadow: 0 0 6px #ff3b30;
}
.ap-led-offline {
  background: #9ca3af;
  box-shadow: none;
}
.local-stream-link {
  display: flex;
  flex-direction: column;
  gap: 1px;
  width: 100%;
  color: #007aff;
  font-size: 9px;
  line-height: 1.25;
  text-decoration: none;
  overflow-wrap: anywhere;
}
.local-stream-link:hover {
  text-decoration: underline;
}
.local-stream-link-disabled {
  color: var(--text-tertiary);
  cursor: default;
}
.local-stream-link-disabled:hover {
  text-decoration: none;
}
@keyframes led-blink-slow {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.3; }
}
@keyframes led-blink-fast {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.15; }
}

/* 弹窗遮罩 (共用) */
.popup-overlay {
  position: fixed; inset: 0;
  z-index: 9999;
  display: flex; align-items: center; justify-content: center;
  background: rgba(0,0,0,0.5);
  backdrop-filter: blur(4px);
  animation: fade-in 0.2s ease;
}
.popup-card {
  background: var(--glass-bg);
  backdrop-filter: blur(20px);
  border: 1px solid var(--glass-border);
  border-radius: 16px;
  padding: 28px 36px;
  text-align: center;
  animation: pop-in 0.25s ease;
}
.popup-gas {
  box-shadow: 0 12px 40px rgba(255,59,48,0.25);
}
.popup-person {
  box-shadow: 0 12px 40px rgba(0,122,255,0.25);
}
@keyframes fade-in {
  from { opacity: 0; }
  to { opacity: 1; }
}
@keyframes pop-in {
  from { transform: scale(0.85); opacity: 0; }
  to { transform: scale(1); opacity: 1; }
}
</style>
