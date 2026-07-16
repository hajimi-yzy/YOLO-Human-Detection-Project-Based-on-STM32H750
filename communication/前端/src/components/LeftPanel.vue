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
          {{ sensorData.temperature ?? '--' }}<span class="text-[10px] font-normal" style="color: var(--text-tertiary)">°C</span>
        </span>
      </div>
      <div class="data-card flex justify-between items-center !p-2">
        <span class="text-[11px]" style="color: var(--text-secondary)">湿度</span>
        <span class="text-xs font-bold" style="color: var(--text-primary)">
          {{ sensorData.humidity ?? '--' }}<span class="text-[10px] font-normal" style="color: var(--text-tertiary)">%</span>
        </span>
      </div>
      <div class="data-card flex justify-between items-center !p-2">
        <span class="text-[11px]" style="color: var(--text-secondary)">海拔</span>
        <span class="text-xs font-bold" style="color: var(--text-primary)">
          {{ sensorData.altitude ?? '--' }}<span class="text-[10px] font-normal" style="color: var(--text-tertiary)">m</span>
        </span>
      </div>
      <div class="data-card flex justify-between items-center !p-2">
        <span class="text-[11px]" style="color: var(--text-secondary)">气压</span>
        <span class="text-xs font-bold" style="color: var(--text-primary)">
          {{ sensorData.pressure ?? '--' }}<span class="text-[10px] font-normal" style="color: var(--text-tertiary)">hPa</span>
        </span>
      </div>
    </div>

    <div style="border-bottom: 1px solid var(--border-color)"></div>

    <!-- 机器人配置 -->
    <div class="flex flex-col gap-2">
      <div class="text-[10px] font-medium" style="color: var(--text-tertiary)">机器人配置</div>
      <div class="flex flex-col gap-1.5">
        <!-- 气体状态指示 -->
        <div class="data-card flex items-center justify-between !p-2">
          <span class="text-[11px]" style="color: var(--text-secondary)">气体状态</span>
          <div class="flex items-center gap-1.5">
            <span class="gas-led" :class="gasAlarm ? 'led-red' : 'led-green'"></span>
            <span class="text-[10px] font-medium" :class="gasAlarm ? 'text-red-500' : 'text-green-500'">
              {{ gasAlarm ? '异常!' : '正常' }}
            </span>
          </div>
        </div>
      </div>
    </div>

    <!-- 气体报警弹窗 -->
    <Teleport to="body">
      <div v-if="gasAlarm && showGasPopup" class="popup-overlay" @click="showGasPopup = false; gasDismissed = true">
        <div class="popup-card popup-gas" @click.stop>
          <div class="text-3xl mb-2">⚠️</div>
          <div class="text-lg font-bold text-red-500 mb-1">可燃气体报警!</div>
          <button class="mt-3 px-4 py-1 rounded-full text-xs font-medium bg-red-500 text-white hover:bg-red-600 transition-colors" @click="showGasPopup = false; gasDismissed = true">确 认</button>
        </div>
      </div>
    </Teleport>

    <!-- 人体识别弹窗 -->
    <Teleport to="body">
      <div v-if="personDetected && showPersonPopup" class="popup-overlay" @click="showPersonPopup = false; personDismissed = true">
        <div class="popup-card popup-person" @click.stop>
          <div class="text-3xl mb-2">🧑</div>
          <div class="text-lg font-bold text-blue-500 mb-1">识别到疑似幸存者!</div>
          <button class="mt-3 px-4 py-1 rounded-full text-xs font-medium bg-blue-500 text-white hover:bg-blue-600 transition-colors" @click="showPersonPopup = false; personDismissed = true">确 认</button>
        </div>
      </div>
    </Teleport>

    <div style="border-bottom: 1px solid var(--border-color)"></div>

    <!-- 底部状态 -->
    <div class="flex flex-col gap-1.5">
      <div class="flex items-center gap-1.5 text-[10px]" style="color: var(--text-tertiary)">
        <span class="w-1.5 h-1.5 rounded-full" :class="wsConnected ? 'bg-green-400' : 'bg-red-400'"></span>
        {{ wsConnected ? 'WS 已连接' : 'WS 未连接' }}
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

defineEmits(['toggleWindow', 'toggleTheme', 'toggleLayout'])
defineProps({
  windowItems: { type: Array, required: true },
  isDark: { type: Boolean, default: false },
  layoutMode: { type: String, default: 'free' },
})

const { connected: wsConnected, sensorData } = useSensorStore()

const showGasPopup = ref(false)
const showPersonPopup = ref(false)
const gasDismissed = ref(false)
const personDismissed = ref(false)

const gasAlarm = computed(() => sensorData.value?.gas?.alarm || false)
const personDetected = computed(() => sensorData.value?.person_detected === 1)

// 每次收到新传感器数据都检查弹窗 (确认后不再重复弹, 等报警消除才重置)
watch(sensorData, (data) => {
  if (!data?.gas?.alarm) gasDismissed.value = false
  if (data?.person_detected !== 1) personDismissed.value = false

  if (data?.gas?.alarm && !showGasPopup.value && !gasDismissed.value) showGasPopup.value = true
  if (data?.person_detected === 1 && !showPersonPopup.value && !personDismissed.value) showPersonPopup.value = true
})

const panelStyle = {
  width: '190px',
  height: 'calc(100vh - 90px)',
  background: 'var(--glass-bg)',
  backdropFilter: 'blur(20px) saturate(180%)',
  WebkitBackdropFilter: 'blur(20px) saturate(180%)',
  borderRight: '1px solid var(--glass-border)',
}
</script>

<style scoped>
/* iOS 风格开关 */
.ios-toggle {
  position: relative;
  display: inline-block;
  width: 40px;
  height: 22px;
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
