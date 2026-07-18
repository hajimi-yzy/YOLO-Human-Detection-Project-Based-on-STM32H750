/** In-memory alarm event journal, deduplicated by verified telemetry frame. */
import { ref, watch } from 'vue'
import { fetchTelemetryLatest, fetchVideoSnapshot } from '@/api'
import { useSensorStore } from '@/composables/useSensorStore'
import { useGpsStore } from '@/composables/useGpsStore'

const MAX_EVENTS = 200
const MAX_PROCESSED_FRAMES = 512

const events = ref([])
const processedFrames = new Set()
const processedFrameOrder = []

let started = false
let activeBootId = null
let lastReceivedAt = 0
let personLatched = false
let gasLatched = false
let eventCounter = 0

function makeEventId() {
  eventCounter = (eventCounter + 1) >>> 0
  return `event-${Date.now().toString(36)}-${eventCounter.toString(36)}`
}

function frameKey(point) {
  if (!point || point.boot_id == null || point.seq == null) return null
  return `${point.device_id ?? 'unknown'}:${point.boot_id}:${point.seq}`
}

function sameFrame(sensor, gps) {
  return sensor && gps
    && sensor.device_id === gps.device_id
    && sensor.boot_id === gps.boot_id
    && sensor.seq === gps.seq
}

function validLocation(gps) {
  if (!gps || gps.online !== true) return null
  if (!Number.isFinite(gps.lat) || !Number.isFinite(gps.lng)) return null
  return { lat: gps.lat, lng: gps.lng }
}

function normalizeGasAlarm(value) {
  const alarm = value && typeof value === 'object' ? value.alarm : value
  if (alarm === true || alarm === 1) return true
  if (alarm === false || alarm === 0) return false
  return null
}

function rememberFrame(key) {
  processedFrames.add(key)
  processedFrameOrder.push(key)
  while (processedFrameOrder.length > MAX_PROCESSED_FRAMES) {
    processedFrames.delete(processedFrameOrder.shift())
  }
}

function revokeSnapshot(event) {
  if (event?.snapshotUrl) URL.revokeObjectURL(event.snapshotUrl)
}

function trimEvents() {
  if (events.value.length <= MAX_EVENTS) return
  const removed = events.value.splice(MAX_EVENTS)
  removed.forEach(revokeSnapshot)
}

function updateEvent(id, patch) {
  const target = events.value.find((event) => event.id === id)
  if (!target) return false
  Object.assign(target, patch)
  return true
}

async function resolveExactLocation(id, sensorPoint, currentGps) {
  const current = sameFrame(sensorPoint, currentGps) ? validLocation(currentGps) : null
  if (current) {
    updateEvent(id, { location: current })
    return
  }

  try {
    const response = await fetchTelemetryLatest()
    const latestSensor = response?.data?.sensor
    const latestGps = response?.data?.gps
    if (sameFrame(sensorPoint, latestSensor) && sameFrame(sensorPoint, latestGps)) {
      const location = validLocation(latestGps)
      if (location) updateEvent(id, { location })
    }
  } catch {
    // A missing exact GPS sample is recorded as NA; stale coordinates are never used.
  }
}

async function attachSnapshot(id) {
  try {
    const snapshot = await fetchVideoSnapshot()
    const snapshotUrl = URL.createObjectURL(snapshot.blob)
    if (!updateEvent(id, {
      snapshotUrl,
      snapshotStatus: 'ready',
      frameId: snapshot.frameId,
      frameSequence: snapshot.frameSequence,
    })) {
      URL.revokeObjectURL(snapshotUrl)
    }
  } catch {
    updateEvent(id, { snapshotStatus: 'error' })
  }
}

function addEvent(type, point, currentGps) {
  const timestamp = Number.isFinite(point.received_at)
    ? Math.round(point.received_at * 1000)
    : Date.now()
  const event = {
    id: makeEventId(),
    type,
    timestamp,
    deviceId: point.device_id ?? null,
    bootId: point.boot_id ?? null,
    sequence: point.seq ?? null,
    location: sameFrame(point, currentGps) ? validLocation(currentGps) : null,
    gas: type === 'gas' ? {
      concentration: point.gas?.concentration ?? null,
      unit: point.gas?.unit ?? null,
    } : null,
    snapshotUrl: null,
    snapshotStatus: type === 'survivor' ? 'loading' : 'none',
    frameId: null,
    frameSequence: null,
  }

  events.value.unshift(event)
  trimEvents()

  if (!event.location) void resolveExactLocation(event.id, point, currentGps)
  if (type === 'survivor') void attachSnapshot(event.id)
}

function processTelemetry(point, currentGps) {
  if (!point || point.online !== true) return
  const key = frameKey(point)
  if (!key || processedFrames.has(key)) return

  const receivedAt = Number(point.received_at) || 0
  if (receivedAt && receivedAt < lastReceivedAt) return
  rememberFrame(key)
  if (receivedAt) lastReceivedAt = Math.max(lastReceivedAt, receivedAt)

  if (activeBootId !== point.boot_id) {
    activeBootId = point.boot_id
    personLatched = false
    gasLatched = false
  }

  const gasAlarm = normalizeGasAlarm(point.gas)
  const personDetected = point.person_detected

  if (gasAlarm === false) gasLatched = false
  if ((personDetected === 0 || personDetected === false)) personLatched = false

  // Keep separate records. Adding the survivor second leaves it first in the list.
  if (gasAlarm === true && !gasLatched) {
    gasLatched = true
    addEvent('gas', point, currentGps)
  }
  if ((personDetected === 1 || personDetected === true) && !personLatched) {
    personLatched = true
    addEvent('survivor', point, currentGps)
  }
}

function start() {
  if (started) return
  started = true
  const sensorStore = useSensorStore()
  const gpsStore = useGpsStore()
  watch(sensorStore.data, (point) => processTelemetry(point, gpsStore.data.value), { immediate: true })
}

function clearEvents() {
  events.value.forEach(revokeSnapshot)
  events.value = []
}

function recordModemEvent({ operation, success, message, location = null }) {
  const event = {
    id: makeEventId(),
    type: 'modem',
    timestamp: Date.now(),
    success: success === true,
    operation: operation || '4G 操作',
    message: message || (success ? '设置已生效' : '设置未生效'),
    location: validLocation({ online: true, ...location }),
    gas: null,
    snapshotUrl: null,
    snapshotStatus: 'none',
    frameId: null,
    frameSequence: null,
  }
  events.value.unshift(event)
  trimEvents()
  return event.id
}

function recordWifiEvent({ operation, success, message }) {
  const event = {
    id: makeEventId(),
    type: 'wifi',
    timestamp: Date.now(),
    success: success === true,
    operation: operation || 'Wi-Fi 操作',
    message: message || (success ? '设置已生效' : '设置未生效'),
    location: null,
    gas: null,
    snapshotUrl: null,
    snapshotStatus: 'none',
    frameId: null,
    frameSequence: null,
  }
  events.value.unshift(event)
  trimEvents()
  return event.id
}

function recordVideoEvent({ operation, success, message }) {
  const event = {
    id: makeEventId(),
    type: 'video',
    timestamp: Date.now(),
    success: success === true,
    operation: operation || '视频流设置',
    message: message || (success ? '设置已生效' : '设置未生效'),
    location: null,
    gas: null,
    snapshotUrl: null,
    snapshotStatus: 'none',
    frameId: null,
    frameSequence: null,
  }
  events.value.unshift(event)
  trimEvents()
  return event.id
}

export function useEventStore() {
  start()
  return { events, clearEvents, recordModemEvent, recordWifiEvent, recordVideoEvent }
}
