/**
 * WebSocket 统一管理
 * 支持多路连接（控制、传感器、GPS），自动重连
 */

const MAX_RECONNECT = 10
const RECONNECT_BASE_MS = 1000

class WsClient {
  constructor(url, { onMessage, onOpen, onClose, onError } = {}) {
    this.url = url
    this.ws = null
    this.reconnectCount = 0
    this.reconnectTimer = null
    this.manualClose = false
    this.handlers = { onMessage, onOpen, onClose, onError }
  }

  connect() {
    if (this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) {
      return
    }
    this.manualClose = false
    try {
      this.ws = new WebSocket(this.url)
    } catch (e) {
      console.error(`[WS] 连接失败: ${this.url}`, e)
      this._scheduleReconnect()
      return
    }

    this.ws.onopen = (e) => {
      this.reconnectCount = 0
      console.log(`[WS] 已连接: ${this.url}`)
      this.handlers.onOpen?.(e)
    }

    this.ws.onmessage = (e) => {
      this.handlers.onMessage?.(e.data, e)
    }

    this.ws.onclose = (e) => {
      console.log(`[WS] 已断开: ${this.url}`, e.code)
      this.handlers.onClose?.(e)
      if (!this.manualClose) {
        this._scheduleReconnect()
      }
    }

    this.ws.onerror = (e) => {
      console.error(`[WS] 错误: ${this.url}`, e)
      this.handlers.onError?.(e)
    }
  }

  send(data) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      console.warn('[WS] 未连接，无法发送')
      return false
    }
    const payload = typeof data === 'string' ? data : JSON.stringify(data)
    this.ws.send(payload)
    return true
  }

  close() {
    this.manualClose = true
    this._clearReconnect()
    if (this.ws) {
      this.ws.close()
      this.ws = null
    }
  }

  _scheduleReconnect() {
    if (this.reconnectCount >= MAX_RECONNECT) {
      console.warn(`[WS] 已达最大重连次数: ${this.url}`)
      return
    }
    const delay = Math.min(RECONNECT_BASE_MS * 2 ** this.reconnectCount, 30000)
    this.reconnectCount++
    console.log(`[WS] ${delay / 1000}s 后第 ${this.reconnectCount} 次重连: ${this.url}`)
    this.reconnectTimer = setTimeout(() => this.connect(), delay)
  }

  _clearReconnect() {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
  }

  get readyState() {
    return this.ws ? this.ws.readyState : WebSocket.CLOSED
  }
}

// ==================== 全局 WS 实例工厂 ====================
const clients = {}

export function getWsClient(key, url, handlers) {
  // 关闭旧连接
  if (clients[key]) {
    clients[key].close()
  }
  const client = new WsClient(url, handlers)
  clients[key] = client
  return client
}

export function closeAllWs() {
  Object.values(clients).forEach((c) => c.close())
}

export default WsClient
