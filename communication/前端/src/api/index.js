/**
 * API 请求层
 * 所有后端 HTTP 接口统一在此封装
 */
import cfg from '@/config/config'

const WIFI_MANAGER_BASE = cfg.WIFI_MANAGER_BASE

async function request(path, options = {}) {
  const url = `${cfg.API_BASE}${path}`
  const config = { ...options }
  if (options.body != null) {
    config.headers = {
      'Content-Type': 'application/json',
      ...(options.headers || {}),
    }
  }
  try {
    const res = await fetch(url, config)
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    return await res.json()
  } catch (err) {
    console.error(`[API] ${url} 请求失败:`, err)
    throw err
  }
}

// ==================== 控制指令 ====================
export function sendCommand(cmd, params = {}) {
  return request('/command', {
    method: 'POST',
    body: JSON.stringify({ cmd, ...params }),
  })
}

// ==================== 传感器 ====================
export function fetchSensorData() {
  return request('/sensor')
}

export function fetchSensorHistory(range = '1h') {
  return request(`/sensor/history?range=${range}`)
}

// ==================== GPS ====================
export function fetchGpsPosition() {
  return request('/gps/position')
}

export function fetchGpsHistory(range = '1h') {
  return request(`/gps/history?range=${range}`)
}

export function fetchTelemetryLatest() {
  return request('/telemetry/latest', { cache: 'no-store' })
}

export function fetchCellLocation() {
  return request('/modem/cell-location', { cache: 'no-store' })
}

export function fetchLocationLatest(cellEnabled = false) {
  return request(`/location/latest?cell_enabled=${cellEnabled ? 1 : 0}`, { cache: 'no-store' })
}

async function wifiManagerRequest(path, options = {}) {
  const { timeoutMs = 6000, ...fetchOptions } = options
  const controller = new AbortController()
  const timeout = window.setTimeout(() => controller.abort(), timeoutMs)
  try {
    const response = await fetch(`${WIFI_MANAGER_BASE}${path}`, {
      headers: { 'Content-Type': 'application/json' },
      cache: 'no-store',
      signal: controller.signal,
      ...fetchOptions,
    })
    const payload = await response.json().catch(() => ({}))
    if (!response.ok) throw new Error(payload.error || `HTTP ${response.status}`)
    return payload
  } catch (error) {
    if (error?.name === 'AbortError') {
      throw new Error('ESP Wi-Fi 管理接口连接超时')
    }
    throw error
  } finally {
    window.clearTimeout(timeout)
  }
}

export function fetchWifiManagerStatus() {
  return wifiManagerRequest('/status', { timeoutMs: 2500 })
}

export function setWifiConnectionEnabled(enabled) {
  return wifiManagerRequest('/enable', {
    method: 'POST',
    body: JSON.stringify({ enabled }),
  })
}

export function scanWifiNetworks() {
  return wifiManagerRequest('/scan', { timeoutMs: 8000 })
}

export function connectWifiNetwork(ssid, password, security) {
  return wifiManagerRequest('/connect', {
    method: 'POST',
    body: JSON.stringify({ ssid, password, security }),
  })
}

export function selectWifiCloudUplink(useWifi) {
  return wifiManagerRequest('/uplink', {
    method: 'POST',
    body: JSON.stringify({ use_wifi: useWifi }),
  })
}

// ==================== 视频 ====================
export function getRtspUrl() {
  return cfg.RTSP_URL
}

export async function fetchVideoSnapshot() {
  const url = `${cfg.API_BASE}/video/snapshot`
  const response = await fetch(url, { cache: 'no-store' })
  if (!response.ok) throw new Error(`HTTP ${response.status}`)
  const blob = await response.blob()
  if (!blob.type.startsWith('image/')) throw new Error('Snapshot response is not an image')
  return {
    blob,
    frameId: response.headers.get('X-Frame-Id'),
    frameSequence: response.headers.get('X-Frame-Sequence'),
  }
}

// ==================== 系统 ====================
export function fetchSystemStatus() {
  return request('/system/status')
}

export function setConfig(key, value) {
  return request('/system/config', {
    method: 'POST',
    body: JSON.stringify({ key, value }),
  })
}
