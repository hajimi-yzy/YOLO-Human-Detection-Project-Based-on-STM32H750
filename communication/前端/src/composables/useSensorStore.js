/**
 * 共享传感器数据 Store（模块级单例）
 * LeftPanel、SensorWindow 等组件共享同一份数据和 WS 连接
 */
import { ref, computed } from 'vue'
import cfg from '@/config/config'
import { getWsClient } from '@/api/websocket'
import { mockSensorData } from '@/api/mock'

// ==================== 模块级单例状态 ====================
const data = ref(null)
const connected = ref(false)
const history = ref([])
const MAX_HISTORY = 50

let client = null
let mockTimer = null
let started = false

function pushHistory(point) {
  history.value.push({
    time: new Date().toLocaleTimeString(),
    gas: point.gas?.concentration ?? 0,
    temp: point.temperature ?? 0,
    hum: point.humidity ?? 0,
  })
  if (history.value.length > MAX_HISTORY) history.value.shift()
}

export function useSensorStore() {
  if (!started) {
    started = true
    _connect()
  }

  const sensorData = computed(() => {
    if (data.value) {
      return {
        gas: data.value.gas || { concentration: 0, unit: '%LEL', alarm: false },
        temperature: data.value.temperature ?? null,
        humidity: data.value.humidity ?? null,
        altitude: data.value.altitude ?? null,
        pressure: data.value.pressure ?? null,
        person_detected: data.value.person_detected ?? 0,
      }
    }
    return mockSensorData()
  })

  return { data, connected, history, sensorData }
}

function _connect() {
  if (!cfg.WS_SENSOR) {
    // 无 WS 地址时启动 mock 定时器
    _startMock()
    return
  }

  client = getWsClient('sensor', cfg.WS_SENSOR, {
    onMessage(raw) {
      try {
        const parsed = JSON.parse(raw)
        data.value = parsed
        pushHistory(parsed)
      } catch {
        data.value = null
      }
    },
    onOpen() {
      connected.value = true
    },
    onClose() {
      connected.value = false
      _startMock()
    },
    onError() {
      connected.value = false
      _startMock()
    },
  })
  client.connect()
  _startMock() // mock 作为 fallback，WS 未连接时使用
}

function _startMock() {
  if (mockTimer) return
  mockTimer = setInterval(() => {
    if (!connected.value) {
      const mock = mockSensorData()
      data.value = mock
      pushHistory(mock)
    }
  }, cfg.SENSOR_INTERVAL)
}
