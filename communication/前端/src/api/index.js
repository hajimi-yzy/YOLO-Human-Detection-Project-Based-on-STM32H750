/**
 * API 请求层
 * 所有后端 HTTP 接口统一在此封装
 */
import cfg from '@/config/config'

async function request(path, options = {}) {
  const url = `${cfg.API_BASE}${path}`
  const config = {
    headers: { 'Content-Type': 'application/json' },
    ...options,
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

// ==================== 登录 ====================
export function login(password) {
  return request('/auth/login', {
    method: 'POST',
    body: JSON.stringify({ username: 'admin', password }),
  })
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

// ==================== 视频 ====================
export function getRtspUrl() {
  return cfg.RTSP_URL
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
