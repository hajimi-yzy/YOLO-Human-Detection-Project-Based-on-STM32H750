import { reactive, computed, onUnmounted } from 'vue'

export function useDraggable(initialX = 100, initialY = 100, callbacks = null) {
  const state = reactive({ x: initialX, y: initialY, dragging: false })

  let offsetX = 0, offsetY = 0, cleanup = null

  function onPointerDown(e) {
    if (!e.target.closest('[data-drag-handle]')) return
    e.preventDefault()
    state.dragging = true
    offsetX = e.clientX - state.x
    offsetY = e.clientY - state.y

    const onMove = (ev) => {
      if (!state.dragging) return
      state.x = ev.clientX - offsetX
      state.y = ev.clientY - offsetY
    }
    const onUp = () => {
      state.dragging = false
      document.removeEventListener('pointermove', onMove)
      document.removeEventListener('pointerup', onUp)
      callbacks?.onDragEnd?.({ x: state.x, y: state.y })
    }
    document.addEventListener('pointermove', onMove)
    document.addEventListener('pointerup', onUp)
    cleanup = () => {
      document.removeEventListener('pointermove', onMove)
      document.removeEventListener('pointerup', onUp)
    }
  }

  onUnmounted(() => cleanup?.())

  const x = computed(() => state.x)
  const y = computed(() => state.y)

  function setPosition(nx, ny) { state.x = nx; state.y = ny }

  return { state, x, y, onPointerDown, setPosition }
}
