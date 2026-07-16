/**
 * WebSocket 数据接收 composable
 * 用法: const { data, connected, send } = useWebSocket('sensor', cfg.WS_SENSOR)
 */
import { ref, onUnmounted, watch } from 'vue'
import { getWsClient } from '@/api/websocket'

export function useWebSocket(key, url, { enabled = true, parseJSON = true } = {}) {
  const data = ref(null)
  const connected = ref(false)
  const rawMessage = ref(null)
  let client = null

  function connect() {
    if (!url) return
    client = getWsClient(key, url, {
      onMessage(raw, _e) {
        rawMessage.value = raw
        try {
          data.value = parseJSON ? JSON.parse(raw) : raw
        } catch {
          data.value = raw
        }
      },
      onOpen() {
        connected.value = true
      },
      onClose() {
        connected.value = false
      },
      onError() {
        connected.value = false
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
  })

  return { data, connected, rawMessage, send, reconnect: connect }
}
