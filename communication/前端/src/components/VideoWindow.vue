<template>
  <MacWindow
    :visible="visible"
    :title="'远程监控'"
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
    <div class="flex flex-col gap-2 h-full min-h-0">
      <div class="flex-1 min-h-0 rounded-lg overflow-hidden relative" style="background: #000">
        <img v-if="mjpegUrl" :src="mjpegUrl" class="absolute inset-0 w-full h-full object-contain" />
        <div v-else class="text-gray-400 text-sm">请在 config.js 配置 MJPEG_URL</div>
      </div>
    </div>
  </MacWindow>
</template>

<script setup>
import { computed } from 'vue'
import MacWindow from './MacWindow.vue'
import cfg from '@/config/config'
import { useDraggable } from '@/composables/useDraggable'
import { useResizable } from '@/composables/useResizable'

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

const mjpegUrl = computed(() => cfg.MJPEG_URL)
</script>
