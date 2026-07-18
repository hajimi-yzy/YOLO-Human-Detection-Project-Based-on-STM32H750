<template>
  <Transition name="window">
    <div
      v-show="visible"
      class="glass-window"
      :class="{ 'pointer-events-none': dragging }"
      :style="windowStyle"
    >
      <div
        class="glass-window-header"
        :class="{ 'cursor-default': !draggable }"
        :data-drag-handle="draggable ? 'true' : undefined"
        @pointerdown="draggable && $emit('dragStart', $event)"
      >
        <span class="flex-1 text-center text-xs font-medium truncate px-3" style="color: var(--text-secondary)">{{ title }}</span>
      </div>

      <div class="glass-window-body">
        <slot></slot>
      </div>

      <template v-if="resizable">
        <div
          v-for="edge in resizeEdges"
          :key="edge"
          class="resize-handle"
          :class="`resize-handle-${edge}`"
          :style="getResizeStyle(edge)"
          @pointerdown="(e) => $emit('resizeStart', e, edge)"
        ></div>
      </template>
    </div>
  </Transition>
</template>

<script setup>
import { computed } from 'vue'

const props = defineProps({
  visible: { type: Boolean, default: true },
  title: { type: String, default: '窗口' },
  x: { type: [Number, String], default: 100 },
  y: { type: [Number, String], default: 100 },
  w: { type: [Number, String], default: 520 },
  h: { type: [Number, String], default: 400 },
  minW: { type: Number, default: 320 },
  minH: { type: Number, default: 240 },
  resizable: { type: Boolean, default: true },
  draggable: { type: Boolean, default: true },
  closable: { type: Boolean, default: true },
  dragging: { type: Boolean, default: false },
})

defineEmits(['close', 'minimize', 'maximize', 'dragStart', 'resizeStart'])

const resizeEdges = ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w']

const EDGE_CURSORS = {
  nw: 'nwse-resize', n: 'ns-resize', ne: 'nesw-resize',
  e: 'ew-resize', se: 'nwse-resize', s: 'ns-resize',
  sw: 'nesw-resize', w: 'ew-resize',
}

function px(v) { return typeof v === 'number' ? `${v}px` : v }

const windowStyle = computed(() => ({
  left: px(props.x),
  top: px(props.y),
  width: px(props.w),
  height: px(props.h),
  minWidth: `${props.minW}px`,
  minHeight: `${props.minH}px`,
}))

function getResizeStyle(edge) {
  const s = { cursor: EDGE_CURSORS[edge] || 'default' }
  const o = {
    nw: { top:'0', left:'0', width:'16px', height:'16px' },
    n:  { top:'0', left:'16px', right:'16px', height:'8px' },
    ne: { top:'0', right:'0', width:'16px', height:'16px' },
    e:  { top:'16px', right:'0', bottom:'16px', width:'8px' },
    se: { bottom:'0', right:'0', width:'20px', height:'20px' },
    s:  { bottom:'0', left:'16px', right:'16px', height:'8px' },
    sw: { bottom:'0', left:'0', width:'16px', height:'16px' },
    w:  { top:'16px', left:'0', bottom:'16px', width:'8px' },
  }
  return { ...s, ...o[edge] }
}
</script>
