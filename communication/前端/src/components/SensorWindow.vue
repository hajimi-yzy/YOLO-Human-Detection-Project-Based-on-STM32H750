<template>
  <MacWindow
    :visible="visible"
    title="传感数据 · 温湿度 / 海拔 / 气压"
    :x="x" :y="y" :w="w" :h="h"
    :minW="420" :minH="300"
    :dragging="dragState.dragging"
    :draggable="draggable"
    :resizable="resizable"
    @close="$emit('close')"
    @minimize="$emit('minimize')"
    @dragStart="handleDragStart"
    @resizeStart="(e, edge) => resizeHandlers['on' + edge.toUpperCase()]?.(e)"
  >
    <div class="sensor-content flex flex-col gap-2.5 h-full min-h-0">
      <div class="sensor-summary flex-shrink-0">
        <div class="data-card text-center">
          <div class="text-[11px] font-medium mb-0.5" style="color: var(--text-tertiary)">温度</div>
          <div class="sensor-value text-xl font-bold" style="color: var(--text-primary)">{{ formatValue(sensorData.temperature, '°C') }}</div>
        </div>
        <div class="data-card text-center">
          <div class="text-[11px] font-medium mb-0.5" style="color: var(--text-tertiary)">湿度</div>
          <div class="sensor-value text-xl font-bold" style="color: var(--text-primary)">{{ formatValue(sensorData.humidity, '%') }}</div>
        </div>
        <div class="data-card text-center">
          <div class="text-[11px] font-medium mb-0.5" style="color: var(--text-tertiary)">海拔</div>
          <div class="sensor-value text-xl font-bold" style="color: var(--text-primary)">{{ formatValue(sensorData.altitude, 'm') }}</div>
        </div>
        <div class="data-card text-center">
          <div class="text-[11px] font-medium mb-0.5" style="color: var(--text-tertiary)">气压</div>
          <div class="sensor-value text-xl font-bold" style="color: var(--text-primary)">{{ formatValue(sensorData.pressure, 'hPa') }}</div>
        </div>
      </div>
      <div class="flex-1 min-h-0">
        <v-chart v-if="deviceOnline && chartOption" :option="chartOption" :autoresize="true" style="width:100%;height:100%" />
        <div v-else class="h-full flex items-center justify-center text-sm" style="color: var(--text-tertiary)">
          {{ deviceOnline ? '等待有效数据' : '设备离线 · NA' }}
        </div>
      </div>
    </div>
  </MacWindow>
</template>

<script setup>
import { computed } from 'vue'
import VChart from 'vue-echarts'
import { use } from 'echarts/core'
import { LineChart } from 'echarts/charts'
import { TitleComponent, TooltipComponent, LegendComponent, GridComponent } from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'
import MacWindow from './MacWindow.vue'
import { useDraggable } from '@/composables/useDraggable'
import { useResizable } from '@/composables/useResizable'
import { useSensorStore } from '@/composables/useSensorStore'

use([LineChart, TitleComponent, TooltipComponent, LegendComponent, GridComponent, CanvasRenderer])

const props = defineProps({
  visible: { type: Boolean, default: true },
  initialX: { type: Number, default: 280 },
  initialY: { type: Number, default: 10 },
  initialW: { type: Number, default: 520 },
  initialH: { type: Number, default: 420 },
  draggable: { type: Boolean, default: true },
  resizable: { type: Boolean, default: true },
})

const emit = defineEmits(['close', 'minimize', 'drag-end', 'resize-end'])

function onDragEnd(pos) { emit('drag-end', pos) }
function onResizeEnd(s) { emit('resize-end', s) }

const { state: dragState, x, y, onPointerDown, setPosition } = useDraggable(props.initialX, props.initialY, { onDragEnd })
function handleDragStart(e) { if (props.draggable) onPointerDown(e) }
const { size, w, h, resizeHandlers, setSize } = useResizable(420, 300, props.initialW, props.initialH, onResizeEnd, dragState)

defineExpose({ setPosition, setSize, x, y, w, h })

const { online: deviceOnline, history, sensorData } = useSensorStore()

function formatValue(value, unit) {
  return value == null ? 'NA' : `${value}${unit}`
}

const chartOption = computed(() => {
  if (!history.value.length) return null
  return {
    grid: { top: 8, right: 16, bottom: 24, left: 40 },
    tooltip: { trigger: 'axis' },
    legend: { data: ['温度(°C)', '湿度(%)'], bottom: 0, textStyle: { fontSize: 10 } },
    xAxis: { type: 'category', data: history.value.map(p => p.time), axisLabel: { fontSize: 9, rotate: 30 } },
    yAxis: { type: 'value', axisLabel: { fontSize: 9 }, splitLine: { lineStyle: { color: 'rgba(0,0,0,0.05)' } } },
    series: [
      { name: '温度(°C)', type: 'line', data: history.value.map(p => p.temp), smooth: true, symbol: 'none', lineStyle: { color: '#ff3b30', width: 2 } },
      { name: '湿度(%)', type: 'line', data: history.value.map(p => p.hum), smooth: true, symbol: 'none', lineStyle: { color: '#007aff', width: 2 } },
    ],
  }
})
</script>

<style scoped>
.sensor-content { container-type: inline-size; }
.sensor-summary {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 8px;
}
@container (max-width: 320px) {
  .sensor-summary {
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 4px;
  }
  .sensor-summary .data-card { padding: 6px 4px; }
  .sensor-value {
    font-size: 14px;
    line-height: 18px;
    overflow-wrap: anywhere;
  }
}
</style>
