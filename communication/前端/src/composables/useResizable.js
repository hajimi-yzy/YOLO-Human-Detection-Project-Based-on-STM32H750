import { reactive, computed, onUnmounted } from 'vue'

const EDGES = ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w']

export function useResizable(minW = 320, minH = 240, initialW = 480, initialH = 360, onEnd = null, posState = null) {
  const size = reactive({ w: initialW, h: initialH, resizing: false })

  let edge = '', startX = 0, startY = 0, startW = 0, startH = 0

  function onResizeStart(e, _edge) {
    e.preventDefault(); e.stopPropagation()
    size.resizing = true
    edge = _edge; startX = e.clientX; startY = e.clientY
    startW = size.w; startH = size.h

    const onMove = (ev) => {
      if (!size.resizing) return
      const dx = ev.clientX - startX
      const dy = ev.clientY - startY
      if (edge.includes('e')) size.w = Math.max(minW, startW + dx)
      if (edge.includes('w')) {
        const desiredW = startW - dx
        const newW = Math.max(minW, desiredW)
        const actualDw = startW - newW
        size.w = newW
        if (posState) posState.x += actualDw
      }
      if (edge.includes('s')) size.h = Math.max(minH, startH + dy)
      if (edge.includes('n')) {
        const desiredH = startH - dy
        const newH = Math.max(minH, desiredH)
        const actualDh = startH - newH
        size.h = newH
        if (posState) posState.y += actualDh
      }
    }
    const onUp = () => {
      size.resizing = false
      document.removeEventListener('pointermove', onMove)
      document.removeEventListener('pointerup', onUp)
      if (onEnd) onEnd({ w: size.w, h: size.h, edge: _edge })
    }
    document.addEventListener('pointermove', onMove)
    document.addEventListener('pointerup', onUp)
  }

  onUnmounted(() => {})

  const resizeHandles = {}
  EDGES.forEach((e) => { resizeHandles[`on${e.toUpperCase()}`] = (ev) => onResizeStart(ev, e) })

  const w = computed(() => `${size.w}px`)
  const h = computed(() => `${size.h}px`)

  function setSize(nw, nh) { size.w = nw; size.h = nh }

  return { size, w, h, resizeHandles, EDGES, setSize }
}
