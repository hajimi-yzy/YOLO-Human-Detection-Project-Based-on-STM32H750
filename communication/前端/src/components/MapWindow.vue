<template>
  <MacWindow
    :visible="visible"
    title="GPS 地图 · 实时定位"
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
      <div class="flex items-center justify-between text-[11px] flex-shrink-0">
        <div class="flex items-center gap-1.5">
          <span class="inline-block w-1.5 h-1.5 rounded-full" :class="wsConnected ? 'bg-green-400' : 'bg-red-400'"></span>
          <span style="color: var(--text-secondary)">{{ wsConnected ? 'GPS 已连接' : 'GPS 未连接' }}</span>
        </div>
        <span v-if="position" style="color: var(--text-tertiary)">卫星:{{ position.satellites ?? '-' }}</span>
      </div>
      <div ref="mapContainer" class="flex-1 min-h-0 rounded-lg overflow-hidden"></div>
      <div class="flex gap-4 text-[10px] flex-shrink-0" style="color: var(--text-tertiary)">
        <span>经度:{{ position?.lng ?? '--' }}</span>
        <span>纬度:{{ position?.lat ?? '--' }}</span>
        <span>速度:{{ position?.speed ?? '--' }}m/s</span>
      </div>
    </div>
  </MacWindow>
</template>

<script setup>
import { ref, computed, watch, onMounted, onUnmounted, nextTick } from 'vue'
import L from 'leaflet'
import MacWindow from './MacWindow.vue'
import cfg from '@/config/config'
import { useDraggable } from '@/composables/useDraggable'
import { useResizable } from '@/composables/useResizable'
import { useWebSocket } from '@/composables/useWebSocket'
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
const { size, w, h, resizeHandlers, setSize } = useResizable(360, 280, props.initialW, props.initialH, onResizeEnd, dragState)

defineExpose({ setPosition, setSize, x, y, w, h })

const { data: wsData, connected: wsConnected } = useWebSocket('gps', cfg.WS_GPS)
const position = computed(() => {
  if (wsData.value && wsData.value.lat != null && wsData.value.lng != null) {
    return { lat: wsData.value.lat, lng: wsData.value.lng, heading: wsData.value.heading, speed: wsData.value.speed, satellites: wsData.value.satellites }
  }
  return null  // 无真实GPS → 显示"--"
})

const mapContainer = ref(null)
let map = null, marker = null, pathLine = [], pathPolyline = null, ro = null

onMounted(async () => {
  await nextTick()
  if (!mapContainer.value) return
  map = L.map(mapContainer.value, { center: cfg.MAP_CENTER, zoom: cfg.MAP_ZOOM, zoomControl: true, attributionControl: false })
  L.tileLayer('https://{s}.basemaps.cartocdn.com/light_nolabels/{z}/{x}/{y}{r}.png', { maxZoom: 19 }).addTo(map)
  pathPolyline = L.polyline([], { color: '#007aff', weight: 3, opacity: 0.6 }).addTo(map)
  // 不预创建 marker，等真实 GPS 数据到了再创建
  ro = new ResizeObserver(() => map?.invalidateSize())
  ro.observe(mapContainer.value)
})

onUnmounted(() => { ro?.disconnect(); map?.remove() })

watch(position, (pos) => { if (pos) updateMarker(pos) })

let robotIcon = null
function updateMarker(pos) {
  if (!map || !pos) return
  // 懒创建: 首次收到真实GPS数据才放标记
  if (!robotIcon) {
    robotIcon = L.icon({
      iconUrl: '/retouch_2026060418314442.png',
      iconSize: [24, 32], iconAnchor: [12, 32],
      popupAnchor: [0, -32],
    })
  }
  if (!marker) {
    marker = L.marker([pos.lat, pos.lng], { icon: robotIcon }).addTo(map)
    marker.bindPopup('').openPopup()
  }
  const ll = [pos.lat, pos.lng]
  marker.setLatLng(ll); pathLine.push(ll)
  if (pathLine.length > 100) pathLine.shift()
  if (pathPolyline) pathPolyline.setLatLngs(pathLine)
  marker.setPopupContent(`<b>🕷️</b> ${pos.lat.toFixed(6)}, ${pos.lng.toFixed(6)}`)
  map.panTo(ll, { animate: true, duration: 0.5 })
}

</script>
