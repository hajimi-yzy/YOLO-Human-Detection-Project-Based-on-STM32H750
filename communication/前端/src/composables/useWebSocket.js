/**
 * WebSocket 数据接收 composable
 * 用法: const { data, connected, send } = useWebSocket('sensor', cfg.WS_SENSOR)
 */
import { ref, onUnmounted, watch } from 'vue'
import { getWsClient } from '@/api/websocket'

export function useWebSocket(key, url, { enabled = true, parseJSON = true, staleAfterMs = 0 } = {}) {
  const data = ref(null)
  const connected = ref(false)
  const fresh = ref(false)
  const rawMessage = ref(null)
  let client = null
  let staleTimer = null
  let lastMessageAt = 0

  function clearData() {
    fresh.value = false
    data.value = null
  }

  if (staleAfterMs > 0) {
    staleTimer = setInterval(() => {
      if (lastMessageAt && Date.now() - lastMessageAt > staleAfterMs) clearData()
    }, 500)
  }

  function connect() {
    if (!url) return
    client = getWsClient(key, url, {
      onMessage(raw, _e) {
        rawMessage.value = raw
        lastMessageAt = Date.now()
        try {
          data.value = parseJSON ? JSON.parse(raw) : raw
        } catch {
          data.value = parseJSON ? null : raw
        }
        fresh.value = data.value != null
      },
      onOpen() {
        connected.value = true
      },
      onClose() {
        connected.value = false
        clearData()
      },
      onError() {
        connected.value = false
        clearData()
      },
    })
    client.connect()
  }

  function send(payload) {
    client?.send(payload)
  }

  watch(
    () => url,
    (newUrl) => {
      if (newUrl && enabled) connect()
    },
    { immediate: true }
  )

  onUnmounted(() => {
    client?.close()
    if (staleTimer) clearInterval(staleTimer)
  })

  return { data, connected, fresh, rawMessage, send, reconnect: connect }
}
