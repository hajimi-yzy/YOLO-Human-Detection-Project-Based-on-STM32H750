/** Shared live telemetry state. Never substitutes mock or stale values. */
import { ref, computed } from 'vue'
import cfg from '@/config/config'
import { getWsClient } from '@/api/websocket'

const data = ref(null)
const connected = ref(false) // WebSocket transport state
const online = ref(false) // Device telemetry state
const history = ref([])
const MAX_HISTORY = 50

let client = null
let staleTimer = null
let apiRefreshTimer = null
let lastMessageAt = 0
let started = false

const emptySensor = () => ({
  gas: null,
  temperature: null,
  humidity: null,
  altitude: null,
  pressure: null,
  person_detected: null,
})

function clearTelemetry() {
  online.value = false
  data.value = null
}

function pushHistory(point) {
  if (!point.online) return
  if (point.temperature == null && point.humidity == null) return
  history.value.push({
    time: new Date().toLocaleTimeString(),
    gas: point.gas?.concentration ?? null,
    temp: point.temperature ?? null,
    hum: point.humidity ?? null,
  })
  if (history.value.length > MAX_HISTORY) history.value.shift()
}

function applyTelemetry(point) {
  if (!point || typeof point !== 'object') {
    clearTelemetry()
    return
  }
  lastMessageAt = Date.now()
  online.value = point.online === true
  data.value = online.value ? point : null
  if (online.value) pushHistory(point)
}

async function refreshTelemetryFromApi() {
  if (!cfg.API_BASE) return
  try {
    const response = await fetch(`${cfg.API_BASE}/telemetry/latest`, { cache: 'no-store' })
    if (!response.ok) return
    const body = await response.json()
    // The API groups sensor and GPS; this store owns the sensor half.
    applyTelemetry(body?.data?.sensor)
  } catch {
    // WebSocket remains the primary path. A transient HTTP failure must not
    // erase a still-fresh WebSocket state.
  }
}

function startFreshnessWatchdog() {
  if (staleTimer) return
  staleTimer = setInterval(() => {
    if (lastMessageAt && Date.now() - lastMessageAt > cfg.TELEMETRY_STALE_MS) {
      clearTelemetry()
    }
  }, 500)
}

function startApiFreshnessSync() {
  if (apiRefreshTimer) return
  refreshTelemetryFromApi()
  apiRefreshTimer = setInterval(refreshTelemetryFromApi, 1000)
}

export function useSensorStore() {
  if (!started) {
    started = true
    connect()
  }

  const sensorData = computed(() => online.value && data.value ? {
    gas: data.value.gas ?? null,
    temperature: data.value.temperature ?? null,
    humidity: data.value.humidity ?? null,
    altitude: data.value.altitude ?? null,
    pressure: data.value.pressure ?? null,
    person_detected: data.value.person_detected ?? null,
  } : emptySensor())

  return { data, connected, online, history, sensorData }
}

function connect() {
  startFreshnessWatchdog()
  startApiFreshnessSync()
  if (!cfg.WS_SENSOR) {
    clearTelemetry()
    return
  }

  client = getWsClient('sensor', cfg.WS_SENSOR, {
    onMessage(raw) {
      try {
        const parsed = JSON.parse(raw)
        applyTelemetry(parsed)
      } catch {
        clearTelemetry()
      }
    },
    onOpen() {
      connected.value = true
      // A WebSocket connection is not proof that the USART device is online.
      clearTelemetry()
    },
    onClose() {
      connected.value = false
      clearTelemetry()
    },
    onError() {
      connected.value = false
      clearTelemetry()
    },
  })
  client.connect()
}
