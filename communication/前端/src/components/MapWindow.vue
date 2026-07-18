<template>
  <MacWindow
    :visible="visible"
    title="位置地图 · GPS / 基站定位"
    :x="x" :y="y" :w="w" :h="h"
    :minW="360" :minH="280"
    :dragging="dragState.dragging"
    :draggable="draggable"
    :resizable="resizable"
    @close="$emit('close')"
    @minimize="$emit('minimize')"
    @dragStart="handleDragStart"
    @resizeStart="(e, edge) => resizeHandlers['on' + edge.toUpperCase()]?.(e)"
  >
    <div class="flex flex-col gap-1.5 h-full min-h-0">
      <div class="location-toolbar flex-shrink-0">
        <div class="flex items-center gap-1.5 text-[11px]">
          <span class="inline-block w-1.5 h-1.5 rounded-full" :class="displayPosition ? 'bg-green-400' : 'bg-red-400'"></span>
          <span style="color: var(--text-secondary)">{{ locationStatus }}</span>
        </div>
        <div class="cell-location-control">
          <span>基站定位</span>
          <button
            class="ios-toggle"
            :class="{ active: cellEnabled }"
            type="button"
            role="switch"
            :aria-checked="cellEnabled"
            aria-label="开启或关闭基站定位"
            @click="toggleCellLocation"
          ><span></span></button>
        </div>
      </div>

      <div class="location-detail flex-shrink-0">
        <span>{{ sourceText }}</span>
        <span v-if="cellEnabled">{{ validationText }}</span>
        <span>卫星:{{ liveGpsPosition?.satellites ?? 'NA' }}</span>
      </div>

      <div ref="mapContainer" class="flex-1 min-h-0 rounded-lg overflow-hidden"></div>

      <div class="location-values flex-shrink-0">
        <span>经度:{{ formatCoordinate(displayPosition?.lng) }}</span>
        <span>纬度:{{ formatCoordinate(displayPosition?.lat) }}</span>
        <span>速度:{{ displayPosition?.speed == null ? 'NA' : `${displayPosition.speed}m/s` }}</span>
        <span v-if="displayPosition?.source === 'cell'">精度半径:{{ formatAccuracy(displayPosition.accuracy) }}</span>
      </div>
    </div>
  </MacWindow>
</template>

<script setup>
import { computed, ref, watch, onMounted, onUnmounted, nextTick } from 'vue'
import L from 'leaflet'
import MacWindow from './MacWindow.vue'
import cfg from '@/config/config'
import { fetchLocationLatest } from '@/api'
import { useDraggable } from '@/composables/useDraggable'
import { useResizable } from '@/composables/useResizable'
import { useGpsStore } from '@/composables/useGpsStore'
import { useControlChannel } from '@/composables/useControlChannel'
import markerIcon2x from 'leaflet/dist/images/marker-icon-2x.png'
import markerIcon from 'leaflet/dist/images/marker-icon.png'
import markerShadow from 'leaflet/dist/images/marker-shadow.png'

delete L.Icon.Default.prototype._getIconUrl
L.Icon.Default.mergeOptions({ iconRetinaUrl: markerIcon2x, iconUrl: markerIcon, shadowUrl: markerShadow })

const props = defineProps({
  visible: { type: Boolean, default: true },
  initialX: { type: Number, default: 280 },
  initialY: { type: Number, default: 10 },
  initialW: { type: Number, default: 700 },
  initialH: { type: Number, default: 500 },
  draggable: { type: Boolean, default: true },
  resizable: { type: Boolean, default: true },
})

const emit = defineEmits(['close', 'minimize', 'drag-end', 'resize-end'])

function onDragEnd(pos) { emit('drag-end', pos) }
function onResizeEnd(s) { emit('resize-end', s) }

const { state: dragState, x, y, onPointerDown, setPosition } = useDraggable(props.initialX, props.initialY, { onDragEnd })
function handleDragStart(e) { if (props.draggable) onPointerDown(e) }
const { w, h, resizeHandlers, setSize } = useResizable(360, 280, props.initialW, props.initialH, onResizeEnd, dragState)

defineExpose({ setPosition, setSize, x, y, w, h })

const { online: gpsOnline, position } = useGpsStore()
const { deviceOnline } = useControlChannel()
const cellEnabled = ref(false)
const cellLocating = ref(false)
const cellLocation = ref(null)
const cellError = ref('')
const validation = ref(null)
let cellRefreshTimer = null
let cellRefreshGeneration = 0

const liveGpsPosition = computed(() => {
  const point = position.value
  if (!gpsOnline.value || !validCoordinates(point)) return null
  return { ...point, source: 'gps', accuracy: null }
})
const displayPosition = computed(() => {
  if (liveGpsPosition.value) return liveGpsPosition.value
  if (deviceOnline.value === true && cellEnabled.value && validCoordinates(cellLocation.value)) {
    return {
      ...cellLocation.value,
      source: 'cell',
      speed: null,
      satellites: null,
    }
  }
  return null
})
const locationStatus = computed(() => {
  if (liveGpsPosition.value) return 'GPS 定位在线'
  if (displayPosition.value?.source === 'cell') return '基站定位在线'
  return '暂无定位'
})
const sourceText = computed(() => {
  if (liveGpsPosition.value) return cellEnabled.value ? '位置来源：GPS（基站校验）' : '位置来源：GPS'
  if (displayPosition.value?.source === 'cell') return '位置来源：基站定位'
  return '位置来源：NA'
})
const validationText = computed(() => {
  if (cellLocating.value) return '正在获取基站位置…'
  if (cellError.value) return `基站定位失败：${cellError.value}`
  if (!cellLocation.value) return '尚无基站定位结果'
  if (!liveGpsPosition.value) return `基站精度半径约 ${formatAccuracy(cellLocation.value.accuracy)}`
  if (!validation.value) return 'GPS 为主，等待基站校验'
  if (typeof validation.value.within_cell_accuracy !== 'boolean') return 'GPS 为主，基站校验结果待确认'
  const distance = Number.isFinite(validation.value.distance_m)
    ? `${Math.round(validation.value.distance_m)}m`
    : 'NA'
  return validation.value.within_cell_accuracy
    ? `GPS 为主 · 基站校验一致（相距 ${distance}）`
    : `GPS 为主 · 与基站结果相差 ${distance}`
})

function validCoordinates(point) {
  return point
    && Number.isFinite(point.lat)
    && Number.isFinite(point.lng)
    && point.lat >= -90 && point.lat <= 90
    && point.lng >= -180 && point.lng <= 180
}

function formatCoordinate(value) {
  return Number.isFinite(value) ? value.toFixed(6) : 'NA'
}

function formatAccuracy(value) {
  return Number.isFinite(value) ? `${Math.round(value)}m` : 'NA'
}

function clearCellRefreshTimer() {
  if (!cellRefreshTimer) return
  clearInterval(cellRefreshTimer)
  cellRefreshTimer = null
}

async function refreshCellLocation() {
  if (!cellEnabled.value || deviceOnline.value !== true || cellLocating.value) return
  const generation = cellRefreshGeneration
  cellLocating.value = true
  try {
    const response = await fetchLocationLatest(true)
    if (generation !== cellRefreshGeneration || !cellEnabled.value || deviceOnline.value !== true) return
    const payload = response?.data || {}
    cellLocation.value = validCoordinates(payload.cell) ? payload.cell : null
    validation.value = payload.validation && typeof payload.validation === 'object'
      ? payload.validation
      : null
    cellError.value = payload.cell_error || ''
  } catch (error) {
    if (generation === cellRefreshGeneration && cellEnabled.value && deviceOnline.value === true) {
      cellLocation.value = null
      cellError.value = error?.message || '请求失败'
      validation.value = null
    }
  } finally {
    const retryCurrentGeneration = generation !== cellRefreshGeneration
      && cellEnabled.value
      && deviceOnline.value === true
    cellLocating.value = false
    if (retryCurrentGeneration) queueMicrotask(() => { void refreshCellLocation() })
  }
}

function toggleCellLocation() {
  cellEnabled.value = !cellEnabled.value
  cellRefreshGeneration += 1
  clearCellRefreshTimer()
  if (cellEnabled.value) {
    void refreshCellLocation()
    cellRefreshTimer = setInterval(refreshCellLocation, 30000)
  } else {
    cellLocation.value = null
    validation.value = null
    cellError.value = ''
  }
}

const mapContainer = ref(null)
let map = null
let marker = null
let pathLine = []
let pathPolyline = null
let resizeObserver = null
let robotIcon = null

onMounted(async () => {
  await nextTick()
  if (!mapContainer.value) return
  map = L.map(mapContainer.value, {
    center: cfg.MAP_CENTER,
    zoom: cfg.MAP_ZOOM,
    zoomControl: true,
    attributionControl: false,
  })
  L.tileLayer('https://{s}.basemaps.cartocdn.com/light_nolabels/{z}/{x}/{y}{r}.png', { maxZoom: 19 }).addTo(map)
  pathPolyline = L.polyline([], { color: '#007aff', weight: 3, opacity: 0.6 }).addTo(map)
  resizeObserver = new ResizeObserver(() => map?.invalidateSize())
  resizeObserver.observe(mapContainer.value)
  if (displayPosition.value) updateMarker(displayPosition.value)
})

onUnmounted(() => {
  clearCellRefreshTimer()
  resizeObserver?.disconnect()
  map?.remove()
})

watch(displayPosition, (point) => {
  if (point) {
    updateMarker(point)
  } else {
    removeLocation()
  }
})

watch(deviceOnline, (online) => {
  if (online === true) {
    if (cellEnabled.value) void refreshCellLocation()
    return
  }
  cellRefreshGeneration += 1
  cellLocation.value = null
  validation.value = null
  cellError.value = ''
})

function removeLocation() {
  marker?.remove()
  marker = null
  pathLine = []
  pathPolyline?.setLatLngs([])
}

function updateMarker(point) {
  if (!map || !validCoordinates(point)) return
  if (!robotIcon) {
    robotIcon = L.icon({
      iconUrl: '/retouch_2026060418314442.png',
      iconSize: [24, 32],
      iconAnchor: [12, 32],
      popupAnchor: [0, -32],
    })
  }
  if (!marker) {
    marker = L.marker([point.lat, point.lng], { icon: robotIcon }).addTo(map)
    marker.bindPopup('').openPopup()
  }

  const latLng = [point.lat, point.lng]
  marker.setLatLng(latLng)
  const lastPoint = pathLine.at(-1)
  const duplicateCellFallback = point.source === 'cell'
    && lastPoint
    && lastPoint[0] === latLng[0]
    && lastPoint[1] === latLng[1]
  if (!duplicateCellFallback) {
    pathLine.push(latLng)
    if (pathLine.length > 100) pathLine.shift()
    pathPolyline?.setLatLngs(pathLine)
  }
  const source = point.source === 'cell' ? '基站定位' : 'GPS'
  marker.setPopupContent(`<b>🕷️</b> ${point.lat.toFixed(6)}, ${point.lng.toFixed(6)}<br>${source}`)
  map.panTo(latLng, { animate: true, duration: 0.5 })
}
</script>

<style scoped>
.location-toolbar { min-height: 24px; display: flex; align-items: center; justify-content: space-between; gap: 10px; }
.cell-location-control { display: flex; align-items: center; gap: 7px; color: var(--text-secondary); font-size: 10px; }
.ios-toggle { position: relative; width: 36px; height: 20px; flex: 0 0 36px; padding: 0; border: 0; border-radius: 10px; background: #9ca3af; cursor: pointer; }
.ios-toggle span { position: absolute; top: 2px; left: 2px; width: 16px; height: 16px; border-radius: 50%; background: #fff; box-shadow: 0 1px 3px rgba(0,0,0,.25); transition: transform .2s ease; }
.ios-toggle.active { background: #34c759; }
.ios-toggle.active span { transform: translateX(16px); }
.location-detail, .location-values { display: flex; align-items: center; flex-wrap: wrap; gap: 6px 14px; color: var(--text-tertiary); font-size: 10px; }
.location-detail { min-height: 18px; }
</style>
