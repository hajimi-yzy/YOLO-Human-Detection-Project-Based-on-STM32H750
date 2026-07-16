/** Shared live GPS state. Keeps a single WebSocket owner for all consumers. */
import { ref, computed } from 'vue'
import cfg from '@/config/config'
import { getWsClient } from '@/api/websocket'

const data = ref(null)
const connected = ref(false)
const online = ref(false)

let client = null
let staleTimer = null
let apiRefreshTimer = null
let lastMessageAt = 0
let started = false

function clearGps() {
  online.value = false
  data.value = null
}

function applyGps(point) {
  if (!point || typeof point !== 'object') {
    clearGps()
    return
  }
  lastMessageAt = Date.now()
  online.value = point.online === true
  data.value = online.value ? point : null
}

async function refreshGpsFromApi() {
  if (!cfg.API_BASE) return
  try {
    const response = await fetch(`${cfg.API_BASE}/telemetry/latest`, { cache: 'no-store' })
    if (!response.ok) return
    const body = await response.json()
    applyGps(body?.data?.gps)
  } catch {
    // A transient HTTP failure must not erase a fresh WebSocket position.
  }
}

function startFreshnessWatchdog() {
  if (staleTimer) return
  staleTimer = setInterval(() => {
    if (lastMessageAt && Date.now() - lastMessageAt > cfg.TELEMETRY_STALE_MS) clearGps()
  }, 500)
}

function startApiFreshnessSync() {
  if (apiRefreshTimer) return
  refreshGpsFromApi()
  apiRefreshTimer = setInterval(refreshGpsFromApi, 1000)
}

function connect() {
  startFreshnessWatchdog()
  startApiFreshnessSync()
  if (!cfg.WS_GPS) {
    clearGps()
    return
  }

  client = getWsClient('gps', cfg.WS_GPS, {
    onMessage(raw) {
      try {
        applyGps(JSON.parse(raw))
      } catch {
        clearGps()
      }
    },
    onOpen() {
      connected.value = true
      clearGps()
    },
    onClose() {
      connected.value = false
      clearGps()
    },
    onError() {
      connected.value = false
      clearGps()
    },
  })
  client.connect()
}

export function useGpsStore() {
  if (!started) {
    started = true
    connect()
  }

  const position = computed(() => {
    const point = data.value
    if (!online.value || point?.lat == null || point?.lng == null) return null
    return {
      lat: point.lat,
      lng: point.lng,
      heading: point.heading ?? null,
      speed: point.speed ?? null,
      satellites: point.satellites ?? null,
    }
  })

  return { data, connected, online, position }
}
