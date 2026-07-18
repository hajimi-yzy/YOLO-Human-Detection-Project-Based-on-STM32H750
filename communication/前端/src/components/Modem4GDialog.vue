<template>
  <Teleport to="body">
    <div v-if="modelValue" class="modem-overlay" @click.self="close">
      <section class="modem-dialog" role="dialog" aria-modal="true" aria-label="4G 管理">
        <header class="modem-header">
          <div>
            <h2>4G 管理</h2>
            <p>{{ statusText }}</p>
          </div>
          <button class="close-button" type="button" aria-label="关闭 4G 管理" @click="close">×</button>
        </header>

        <div class="modem-body">
          <section class="info-grid">
            <div class="info-card"><span>运营商</span><strong>{{ modem4g.operator || 'NA' }}</strong></div>
            <div class="info-card"><span>网络制式</span><strong>{{ modem4g.rat || 'NA' }}</strong></div>
            <div class="info-card"><span>注册状态</span><strong>{{ registrationText }}</strong></div>
            <div class="info-card"><span>信号 CSQ</span><strong>{{ modem4g.rssi ?? 'NA' }}</strong></div>
            <div class="info-card"><span>当前频段</span><strong>{{ servingBand ? `B${servingBand}` : 'NA' }}</strong></div>
            <div class="info-card"><span>小区锁定</span><strong>{{ modem4g.cell_lock || '未报告' }}</strong></div>
          </section>
          <div class="quick-tools">
            <div>
              <span>4G 控制链路延迟</span>
              <strong>{{ latencyText }}</strong>
            </div>
            <button class="tiny-button" type="button" :disabled="latencyTesting || busy" @click="testLatency">
              {{ latencyTesting ? '测速中…' : '测速' }}
            </button>
          </div>

          <section class="section-card">
            <div class="section-heading">
              <div>
                <h3>服务小区与邻区</h3>
                <p>选择一个小区后保存，将按 EARFCN 与 PCI 锁定并重新选网。</p>
              </div>
              <button class="secondary-button" type="button" :disabled="busy" @click="refresh">刷新</button>
            </div>
            <div class="cell-table-wrap">
              <table class="cell-table">
                <thead>
                  <tr><th></th><th>类型</th><th>频段</th><th>EARFCN</th><th>PCI</th><th>小区 ID</th><th>RSRP</th></tr>
                </thead>
                <tbody>
                  <tr v-for="cell in cells" :key="cellKey(cell)">
                    <td><input v-model="selectedCellKey" type="radio" name="lte-cell" :value="cellKey(cell)" /></td>
                    <td>{{ cell.serving ? '当前' : '邻区' }}</td>
                    <td>{{ formatCellBand(cell) }}</td>
                    <td>{{ cell.earfcn }}</td>
                    <td>{{ cell.pci }}</td>
                    <td>{{ cell.cell_id }}</td>
                    <td>{{ cell.rsrp >= 0 ? cell.rsrp : 'NA' }}</td>
                  </tr>
                  <tr v-if="cells.length === 0"><td colspan="7" class="empty-cell">暂无小区信息，请刷新</td></tr>
                </tbody>
              </table>
            </div>
            <div class="button-row">
              <button class="primary-button" type="button" :disabled="busy || !selectedCell" @click="applySelectedCell">
                保存并切换到所选小区
              </button>
              <button class="secondary-button" type="button" :disabled="busy" @click="clearCellLock">解除小区锁定</button>
              <button class="secondary-button" type="button" :disabled="busy" @click="reselect">自动重选基站</button>
            </div>
          </section>

          <section class="section-card">
            <div class="section-heading">
              <div>
                <h3>LTE 频段</h3>
                <p>至少选择一个频段；保存后立即重新注册网络。</p>
              </div>
            </div>
            <div class="band-grid">
              <label v-for="band in supportedBands" :key="band">
                <input v-model="selectedBands" type="checkbox" :value="band" /> B{{ band }}
              </label>
            </div>
            <p class="band-restart-warning">注意：切换频段后必须断电重启设备，频段设置才能完全生效。</p>
            <button class="primary-button" type="button" :disabled="busy || selectedBands.length === 0" @click="applyBands">
              保存并应用频段
            </button>
          </section>

          <p class="disconnect-warning">切换频段或小区时，4G、云端视频和控制链路可能短暂中断；ESP 会在重新注册后回报最终结果。</p>
        </div>
      </section>
    </div>
  </Teleport>
</template>

<script setup>
import { computed, ref, watch } from 'vue'
import { useControlChannel } from '@/composables/useControlChannel'
import { useEventStore } from '@/composables/useEventStore'

const props = defineProps({ modelValue: { type: Boolean, default: false } })
const emit = defineEmits(['update:modelValue'])

const supportedBands = [1, 3, 5, 7, 8, 20, 34, 39, 40, 41]
const selectedBands = ref([...supportedBands])
const selectedCellKey = ref('')
const recordedRequests = new Set()
const latencyTesting = ref(false)
const latencyMs = ref(null)
const latencyError = ref('')
const pingRequestId = ref(null)
let pingStartedAt = 0

const {
  connected,
  deviceOnline,
  modem4g,
  modemRequest,
  requestModem4g,
} = useControlChannel()
const { recordModemEvent } = useEventStore()

const cells = computed(() => [...(modem4g.value.cells || [])].sort((a, b) => Number(b.serving) - Number(a.serving)))
const servingCell = computed(() => cells.value.find((cell) => cell.serving) || null)
const lteEarfcnBands = [
  { band: 1, first: 0, last: 599 },
  { band: 3, first: 1200, last: 1949 },
  { band: 5, first: 2400, last: 2649 },
  { band: 7, first: 2750, last: 3449 },
  { band: 8, first: 3450, last: 3799 },
  { band: 20, first: 6150, last: 6449 },
  { band: 34, first: 36200, last: 36349 },
  { band: 39, first: 38250, last: 38649 },
  { band: 40, first: 38650, last: 39649 },
  { band: 41, first: 39650, last: 41589 },
]

function cellBand(cell) {
  if (Number.isInteger(cell?.band) && cell.band > 0) return cell.band
  if (!Number.isInteger(cell?.earfcn)) return null
  return lteEarfcnBands.find((entry) => (
    cell.earfcn >= entry.first && cell.earfcn <= entry.last
  ))?.band ?? null
}

function formatCellBand(cell) {
  const band = cellBand(cell)
  return band ? `B${band}` : 'NA'
}

const servingBand = computed(() => cellBand(servingCell.value))
const selectedCell = computed(() => cells.value.find((cell) => cellKey(cell) === selectedCellKey.value) || null)
const busy = computed(() => modemRequest.value.pending || modem4g.value.state === 'querying' || modem4g.value.state === 'applying')
const registrationText = computed(() => {
  const labels = { 0: '未注册', 1: '已注册', 2: '搜索中', 3: '被拒绝', 4: '未知', 5: '漫游注册' }
  return labels[modem4g.value.registration] ?? 'NA'
})
const statusText = computed(() => {
  if (!connected.value) return '服务器未连接'
  if (deviceOnline.value === false) return '设备离线'
  if (modem4g.value.supported === false) return '当前固件不支持 4G 管理'
  if (modemRequest.value.pending) return '正在等待 L610 应用并确认设置'
  if (modemRequest.value.status === 'error' || modemRequest.value.status === 'timeout' || modemRequest.value.status === 'rejected') {
    return `上次操作失败：${modemRequest.value.error || '未知错误'}`
  }
  return 'L610 状态已连接'
})
const latencyText = computed(() => {
  if (latencyTesting.value) return '测量中'
  if (Number.isFinite(latencyMs.value)) return `${latencyMs.value} ms`
  return latencyError.value || '未测试'
})

function cellKey(cell) {
  return `${cell.serving ? 1 : 0}:${cell.earfcn}:${cell.pci}:${cell.cell_id}`
}

function close() {
  emit('update:modelValue', false)
}

function submit(params, description, recordEvent = true) {
  const id = requestModem4g(params, description, recordEvent)
  if (!id && recordEvent) {
    recordModemEvent({ operation: description, success: false, message: '请求未发送：设备离线、连接不可用或已有操作正在进行' })
  }
  return id
}

function refresh() {
  submit({ action: 'query' }, '刷新 4G 信息', false)
}

function reselect() {
  submit({ action: 'reselect' }, '自动重选基站')
}

function clearCellLock() {
  submit({ action: 'clear_cell_lock' }, '解除小区锁定')
}

function applySelectedCell() {
  const cell = selectedCell.value
  if (!cell) return
  submit(
    { action: 'set_cell_lock', earfcn: cell.earfcn, pci: cell.pci },
    `切换基站：EARFCN ${cell.earfcn} / PCI ${cell.pci}`,
  )
}

function applyBands() {
  const bands = [...selectedBands.value].sort((a, b) => a - b)
  if (!bands.length) {
    recordModemEvent({ operation: '设置 LTE 频段', success: false, message: '至少选择一个频段' })
    return
  }
  submit({ action: 'set_bands', bands }, `设置 LTE 频段：${bands.map((band) => `B${band}`).join('、')}`)
}

function testLatency() {
  if (latencyTesting.value) return
  const id = requestModem4g({ action: 'ping' }, '4G 控制链路延迟测速', false)
  if (!id) {
    latencyError.value = '无法发送'
    recordModemEvent({ operation: '4G 控制链路延迟测速', success: false, message: '测速请求未发送' })
    return
  }
  pingRequestId.value = id
  pingStartedAt = performance.now()
  latencyMs.value = null
  latencyError.value = ''
  latencyTesting.value = true
}

watch(() => props.modelValue, (opened) => {
  if (opened) refresh()
})

watch(() => modem4g.value.band_config, (value) => {
  const bands = String(value || '').split(',')
    .map((item) => Number.parseInt(item.trim(), 10))
    .filter((item) => item >= 101 && supportedBands.includes(item - 100))
    .map((item) => item - 100)
  if (bands.length && !modemRequest.value.pending) selectedBands.value = [...new Set(bands)]
})

watch(cells, (value) => {
  if (!selectedCell.value && value.length) selectedCellKey.value = cellKey(value[0])
}, { immediate: true })

watch(modemRequest, (request) => {
  if (request.request_id === pingRequestId.value && ['success', 'error', 'timeout', 'rejected', 'disconnected'].includes(request.status)) {
    const success = request.status === 'success'
    latencyTesting.value = false
    if (success) {
      latencyMs.value = Math.max(1, Math.round(performance.now() - pingStartedAt))
      recordModemEvent({ operation: '4G 控制链路延迟测速', success: true, message: `往返延迟 ${latencyMs.value} ms` })
    } else {
      latencyError.value = request.error || '测速失败'
      recordModemEvent({ operation: '4G 控制链路延迟测速', success: false, message: latencyError.value })
    }
    pingRequestId.value = null
  }
  if (!request.request_id || !request.record_event || recordedRequests.has(request.request_id)) return
  if (!['success', 'error', 'timeout', 'rejected', 'disconnected'].includes(request.status)) return
  recordedRequests.add(request.request_id)
  const success = request.status === 'success'
  recordModemEvent({
    operation: request.description,
    success,
    message: success ? 'L610 已确认设置生效' : `未生效：${request.error || request.status}`,
  })
}, { deep: true })

</script>

<style scoped>
.modem-overlay { position: fixed; inset: 0; z-index: 10020; display: flex; align-items: center; justify-content: center; padding: 24px; background: rgba(0,0,0,.58); backdrop-filter: blur(4px); }
.modem-dialog { width: min(980px, 94vw); max-height: 90vh; display: flex; flex-direction: column; overflow: hidden; border: 1px solid var(--glass-border); border-radius: 14px; background: var(--glass-bg); box-shadow: 0 24px 80px rgba(0,0,0,.42); color: var(--text-primary); }
.modem-header { display: flex; align-items: flex-start; justify-content: space-between; padding: 16px 18px; border-bottom: 1px solid var(--border-color); }
.modem-header h2 { margin: 0; font-size: 17px; font-weight: 700; }
.modem-header p, .section-heading p { margin: 4px 0 0; font-size: 10px; color: var(--text-tertiary); }
.close-button { border: 0; background: transparent; color: var(--text-secondary); font-size: 24px; line-height: 1; cursor: pointer; }
.modem-body { padding: 14px 18px 18px; overflow-y: auto; }
.info-grid { display: grid; grid-template-columns: repeat(6, minmax(0, 1fr)); gap: 8px; }
.info-card, .section-card { border: 1px solid var(--border-color); border-radius: 9px; background: rgba(127,127,127,.06); }
.info-card { min-width: 0; padding: 10px; }
.info-card span { display: block; font-size: 9px; color: var(--text-tertiary); }
.info-card strong { display: block; margin-top: 4px; overflow: hidden; font-size: 11px; text-overflow: ellipsis; white-space: nowrap; }
.quick-tools { margin-top: 8px; display: flex; align-items: center; justify-content: space-between; gap: 10px; padding: 8px 10px; border: 1px solid var(--border-color); border-radius: 8px; font-size: 10px; color: var(--text-secondary); }
.quick-tools div { display: flex; align-items: center; gap: 8px; }
.quick-tools strong { color: var(--text-primary); }
.tiny-button { min-height: 24px; padding: 0 9px; border: 1px solid #007aff; border-radius: 6px; background: transparent; color: #007aff; font-size: 9px; cursor: pointer; }
.tiny-button:disabled { opacity: .42; cursor: not-allowed; }
.section-card { margin-top: 10px; padding: 12px; }
.section-heading { display: flex; align-items: flex-start; justify-content: space-between; gap: 12px; margin-bottom: 10px; }
.section-heading h3 { margin: 0; font-size: 12px; font-weight: 700; }
.cell-table-wrap { overflow-x: auto; }
.cell-table { width: 100%; border-collapse: collapse; font-size: 10px; }
.cell-table th, .cell-table td { padding: 7px 6px; text-align: left; border-bottom: 1px solid var(--border-color); white-space: nowrap; }
.cell-table th { color: var(--text-tertiary); font-weight: 600; }
.empty-cell { height: 54px; text-align: center !important; color: var(--text-tertiary); }
.button-row { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
.primary-button, .secondary-button { min-height: 30px; padding: 0 12px; border-radius: 7px; font-size: 10px; cursor: pointer; }
.primary-button { border: 1px solid #007aff; color: #fff; background: #007aff; }
.secondary-button { border: 1px solid var(--border-color); color: var(--text-secondary); background: transparent; }
.primary-button:disabled, .secondary-button:disabled { opacity: .42; cursor: not-allowed; }
.band-grid { display: grid; grid-template-columns: repeat(5, 1fr); gap: 8px; margin-bottom: 10px; font-size: 10px; }
.band-grid label { display: flex; align-items: center; gap: 5px; }
.band-restart-warning { margin: 0 0 10px; padding: 8px 10px; border: 1px solid rgba(245, 158, 11, .45); border-radius: 7px; color: #f59e0b; background: rgba(245, 158, 11, .1); font-size: 10px; font-weight: 700; line-height: 1.5; }
.disconnect-warning { margin: 12px 2px 0; font-size: 10px; line-height: 1.5; color: #f59e0b; }
@media (max-width: 760px) { .info-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); } .band-grid { grid-template-columns: repeat(3, 1fr); } .modem-overlay { padding: 8px; } }
</style>
