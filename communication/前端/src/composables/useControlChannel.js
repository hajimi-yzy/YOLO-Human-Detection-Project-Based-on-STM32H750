/** Shared control channel. A single WebSocket serves controls and device state. */
import { ref, computed } from 'vue'
import cfg from '@/config/config'
import { getWsClient } from '@/api/websocket'

const connected = ref(false)
const deviceOnline = ref(null)
const lastAck = ref(null)
const apRequest = ref({
  pending: false,
  request_id: null,
  desired_enabled: null,
  status: 'idle',
  error: null,
})
const apStream = ref({
  supported: null,
  state: 'unknown',
  request_id: null,
  error: null,
})

let client = null
let started = false
let requestCounter = 0
let apRequestTimer = null

function clearApRequestTimer() {
  if (!apRequestTimer) return
  clearTimeout(apRequestTimer)
  apRequestTimer = null
}

function setApRequest(value) {
  apRequest.value = {
    pending: value.pending ?? false,
    request_id: value.request_id ?? null,
    desired_enabled: value.desired_enabled ?? null,
    status: value.status ?? 'idle',
    error: value.error ?? null,
  }
}

function resetApRequest() {
  clearApRequestTimer()
  setApRequest({})
}

function failApRequest(status, error) {
  clearApRequestTimer()
  setApRequest({
    pending: false,
    request_id: apRequest.value.request_id,
    desired_enabled: apRequest.value.desired_enabled,
    status,
    error,
  })
}

function unknownApState() {
  apStream.value = {
    supported: null,
    state: 'unknown',
    request_id: null,
    error: null,
  }
}

function normalizeApState(value) {
  if (!value || typeof value !== 'object') return null
  const allowedStates = new Set(['starting', 'stopping', 'enabled', 'disabled', 'error'])
  return {
    supported: typeof value.supported === 'boolean' ? value.supported : null,
    state: allowedStates.has(value.state) ? value.state : 'unknown',
    request_id: value.request_id ?? null,
    error: value.error ?? null,
    received_at: value.received_at ?? null,
  }
}

function handleMessage(raw) {
  let message
  try {
    message = JSON.parse(raw)
  } catch {
    return
  }

  if (message?.type === 'command_ack') {
    lastAck.value = message
    const ackRequestId = message.requestId ?? message.request_id ?? null
    if (apRequest.value.pending && ackRequestId === apRequest.value.request_id) {
      if (message.accepted === false) {
        failApRequest('rejected', message.message || '服务器拒绝请求')
      } else if (message.accepted === true) {
        setApRequest({ ...apRequest.value, status: 'waiting' })
      }
    }
  }
  if (message?.type === 'device_state' && typeof message.online === 'boolean') {
    deviceOnline.value = message.online
    if (message.online === false) resetApRequest()
  }

  const reported = message?.type === 'device_state'
    ? message.ap_stream
    : message?.device_state?.ap_stream
  const normalized = normalizeApState(reported)
  if (normalized) {
    apStream.value = normalized
    const finalState = normalized.state === 'enabled'
      || normalized.state === 'disabled'
      || normalized.state === 'error'
    if (
      normalized.request_id === apRequest.value.request_id
      && finalState
    ) {
      // A final device heartbeat can arrive after the local timeout. Reconcile
      // it by request ID so a stale starting/stopping state cannot lock the UI.
      resetApRequest()
    }
  }
}

function connect() {
  if (!cfg.WS_MAIN) return
  client = getWsClient('control', cfg.WS_MAIN, {
    onMessage: handleMessage,
    onOpen() {
      connected.value = true
      deviceOnline.value = null
      resetApRequest()
      unknownApState()
    },
    onClose() {
      connected.value = false
      deviceOnline.value = null
      resetApRequest()
      unknownApState()
    },
    onError() {
      connected.value = false
      deviceOnline.value = null
      resetApRequest()
      unknownApState()
    },
  })
  client.connect()
}

function makeRequestId() {
  requestCounter = (requestCounter + 1) >>> 0
  return `web-${Date.now().toString(36)}-${requestCounter.toString(36)}`
}

function sendCommand(cmd, params = {}) {
  if (!connected.value || !client) return null
  const requestId = makeRequestId()
  const sent = client.send({ type: 'command_request', cmd, params, requestId })
  return sent ? requestId : null
}

function requestApStream(enabled) {
  if (!connected.value || !client || apRequest.value.pending) return null
  const requestId = makeRequestId()
  const desiredEnabled = enabled === true
  const sent = client.send({
    type: 'command_request',
    cmd: 'ap_stream',
    params: { enabled: desiredEnabled },
    requestId,
  })
  if (!sent) return null

  clearApRequestTimer()
  setApRequest({
    pending: true,
    request_id: requestId,
    desired_enabled: desiredEnabled,
    status: 'queued',
    error: null,
  })
  apRequestTimer = setTimeout(() => {
    if (apRequest.value.pending && apRequest.value.request_id === requestId) {
      failApRequest('timeout', '设备确认超时')
    }
  }, 7000)
  return requestId
}

export function useControlChannel() {
  if (!started) {
    started = true
    connect()
  }

  const apStreamEnabled = computed(() => (
    deviceOnline.value === true && apStream.value.state === 'enabled'
  ))
  const apStreamBusy = computed(() => (
    apRequest.value.pending
    || (
      apRequest.value.status === 'idle'
      && (apStream.value.state === 'starting' || apStream.value.state === 'stopping')
    )
  ))

  return {
    connected,
    deviceOnline,
    lastAck,
    apRequest,
    apStream,
    apStreamEnabled,
    apStreamBusy,
    sendCommand,
    requestApStream,
  }
}
