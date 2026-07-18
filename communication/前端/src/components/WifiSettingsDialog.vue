<template>
  <Teleport to="body">
    <div v-if="modelValue" class="wifi-overlay" @click.self="close">
      <section class="wifi-dialog" role="dialog" aria-modal="true" aria-label="Wi-Fi 管理">
        <header class="wifi-header">
          <div class="min-w-0">
            <h2>Wi-Fi 管理</h2>
            <p :class="{ error: Boolean(statusError) }">{{ statusText }}</p>
          </div>
          <button class="icon-button" type="button" aria-label="关闭 Wi-Fi 管理" @click="close">×</button>
        </header>

        <div class="wifi-body">
          <p class="ap-stream-warning" :class="{ blocking: wifiEnableBlocked }">
            开启 Wi-Fi 连接功能前，必须先在机器人配置中关闭 AP 热点串流。
            <span v-if="wifiEnableBlocked">当前 AP 热点串流尚未关闭。</span>
          </p>

          <section class="status-grid">
            <div><span>连接功能</span><strong>{{ wifiStatus.feature_enabled ? '已开启' : '已关闭' }}</strong></div>
            <div><span>连接网络</span><strong>{{ wifiStatus.connected ? wifiStatus.ssid : '未连接' }}</strong></div>
            <div><span>STA 地址</span><strong>{{ wifiStatus.ip || 'NA' }}</strong></div>
            <div><span>云端上行</span><strong>{{ activeUplinkLabel }}</strong></div>
          </section>

          <section class="settings-section toggle-section">
            <div>
              <h3>Wi-Fi 连接功能</h3>
              <p>{{ wifiStatus.feature_enabled ? 'STA 已启用' : 'STA、扫描和重连均已停止' }}</p>
            </div>
            <button
              class="ios-toggle"
              :class="{ active: wifiStatus.feature_enabled, disabled: busy || wifiEnableBlocked }"
              type="button"
              role="switch"
              :aria-checked="wifiStatus.feature_enabled"
              :disabled="busy || wifiEnableBlocked"
              @click="toggleFeature"
            ><span></span></button>
          </section>

          <section class="settings-section">
            <div class="section-heading">
              <div>
                <h3>附近网络</h3>
                <p>{{ networks.length ? `发现 ${networks.length} 个网络` : '尚未扫描' }}</p>
              </div>
              <button
                class="secondary-button"
                type="button"
                :disabled="!wifiStatus.feature_enabled || busy"
                @click="scan"
              >{{ scanning ? '扫描中…' : '扫描' }}</button>
            </div>

            <div class="network-list">
              <button
                v-for="network in networks"
                :key="`${network.ssid}:${network.channel}`"
                class="network-row"
                :class="{ selected: selectedSsid === network.ssid, unsupported: network.supported === false }"
                type="button"
                :disabled="network.supported === false"
                @click="selectNetwork(network)"
              >
                <span class="network-name">{{ network.ssid || '隐藏网络' }}</span>
                <span>{{ network.security }} · {{ network.rssi }} dBm{{ network.supported === false ? ' · 不支持' : '' }}</span>
              </button>
              <div v-if="!networks.length" class="network-empty">没有扫描结果</div>
            </div>

            <div class="connect-form">
              <label>
                <span>SSID</span>
                <input v-model.trim="selectedSsid" type="text" maxlength="32" readonly :disabled="!wifiStatus.feature_enabled || busy" />
              </label>
              <label>
                <span>密码</span>
                <input
                  v-model="password"
                  :type="showPassword ? 'text' : 'password'"
                  maxlength="63"
                  autocomplete="new-password"
                  :disabled="!wifiStatus.feature_enabled || busy || !selectedNetworkSecured"
                  :placeholder="selectedNetworkSecured ? '输入 Wi-Fi 密码' : '开放网络无需密码'"
                />
              </label>
              <label class="password-toggle">
                <input v-model="showPassword" type="checkbox" :disabled="!selectedNetworkSecured" />
                <span>显示密码</span>
              </label>
              <button
                class="primary-button"
                type="button"
                :disabled="!canConnect"
                @click="connect"
              >{{ connecting ? '连接中…' : '连接' }}</button>
            </div>
          </section>

          <section class="settings-section">
            <div class="section-heading">
              <div>
                <h3>云端上行</h3>
                <p>当前：{{ activeUplinkLabel }}</p>
              </div>
            </div>
            <div class="uplink-control">
              <button
                type="button"
                :class="{ active: wifiStatus.active_uplink === 'l610' }"
                :disabled="busy"
                @click="setUplink(false)"
              >L610</button>
              <button
                type="button"
                :class="{ active: wifiStatus.active_uplink === 'wifi' }"
                :disabled="busy || !wifiStatus.connected"
                @click="setUplink(true)"
              >Wi-Fi</button>
            </div>
          </section>

          <p class="local-note">本页面通过云端控制链路管理 ESP Wi-Fi；无需连接机器人热点。</p>
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
const {
  connected,
  deviceOnline,
  apStream,
  wifiSta: wifiStatus,
  wifiRequest,
  requestWifiSta,
} = useControlChannel()
const { recordWifiEvent } = useEventStore()

const selectedSsid = ref('')
const selectedNetworkSecured = ref(true)
const selectedNetworkSupported = ref(false)
const selectedSecurity = ref('')
const password = ref('')
const showPassword = ref(false)
const localError = ref('')
const recordedRequests = new Set()

const networks = computed(() => wifiStatus.value.networks || [])
const scanning = computed(() => (
  wifiStatus.value.scanning
  || (wifiRequest.value.pending && wifiRequest.value.action === 'scan')
))
const connecting = computed(() => (
  wifiRequest.value.pending && wifiRequest.value.action === 'connect'
))
const busy = computed(() => (
  wifiRequest.value.pending
  || wifiStatus.value.scanning
  || wifiStatus.value.state === 'working'
  || wifiStatus.value.state === 'applying'
))
const wifiEnableBlocked = computed(() => (
  !wifiStatus.value.feature_enabled
  && apStream.value.state !== 'disabled'
))
const statusError = computed(() => {
  if (localError.value) return localError.value
  if (wifiRequest.value.pending) return ''
  if (['error', 'timeout', 'rejected', 'disconnected'].includes(wifiRequest.value.status)) {
    return wifiRequest.value.error || 'Wi-Fi 操作失败'
  }
  if (wifiStatus.value.state === 'error') return wifiStatus.value.error || 'Wi-Fi 操作失败'
  return ''
})
const canConnect = computed(() => (
  wifiStatus.value.feature_enabled
  && !busy.value
  && selectedSsid.value.length > 0
  && selectedNetworkSupported.value
  && selectedSecurity.value.length > 0
  && (!selectedNetworkSecured.value || password.value.length >= 8)
))
const activeUplinkLabel = computed(() => {
  if (wifiStatus.value.active_uplink === 'wifi') return 'Wi-Fi'
  if (wifiStatus.value.active_uplink === 'l610') return 'L610'
  return '无可用上行'
})
const statusText = computed(() => {
  if (!connected.value) return '服务器未连接'
  if (deviceOnline.value === false) return '设备离线'
  if (wifiStatus.value.supported === false) return '当前固件不支持 Wi-Fi 管理'
  if (statusError.value) return statusError.value
  if (wifiRequest.value.pending) {
    if (wifiRequest.value.action === 'scan') return '正在扫描附近 Wi-Fi'
    if (wifiRequest.value.action === 'connect') return '正在等待 ESP 连接并确认'
    return '正在等待 ESP 应用并确认设置'
  }
  if (wifiStatus.value.connected) {
    return `${wifiStatus.value.ssid} · ${wifiStatus.value.rssi ?? 'NA'} dBm`
  }
  return wifiStatus.value.feature_enabled ? '连接功能已开启' : '连接功能已关闭'
})

function close() {
  password.value = ''
  showPassword.value = false
  localError.value = ''
  emit('update:modelValue', false)
}

function submit(params, description, recordEvent = true) {
  localError.value = ''
  const requestId = requestWifiSta(params, description, recordEvent)
  if (!requestId) {
    const message = '请求未发送：设备离线、连接不可用或已有操作正在进行'
    localError.value = message
    if (recordEvent) {
      recordWifiEvent({ operation: description, success: false, message })
    }
  }
  return requestId
}

function refreshStatus() {
  submit({ action: 'query' }, '刷新 Wi-Fi 状态', false)
}

function toggleFeature() {
  if (busy.value || wifiEnableBlocked.value) return
  const enabled = !wifiStatus.value.feature_enabled
  submit(
    { action: 'set_enabled', enabled },
    enabled ? '开启 Wi-Fi 连接功能' : '关闭 Wi-Fi 连接功能',
  )
}

function scan() {
  if (!wifiStatus.value.feature_enabled || busy.value) return
  submit({ action: 'scan' }, '扫描附近 Wi-Fi', false)
}

function selectNetwork(network) {
  if (network.supported === false) return
  selectedSsid.value = network.ssid
  selectedNetworkSecured.value = network.secured !== false
  selectedNetworkSupported.value = network.supported !== false
  selectedSecurity.value = network.security || ''
  password.value = ''
}

function connect() {
  if (!canConnect.value) return
  const ssid = selectedSsid.value
  const credentials = {
    action: 'connect',
    ssid,
    password: selectedNetworkSecured.value ? password.value : '',
    security: selectedSecurity.value,
  }
  submit(credentials, `连接 Wi-Fi：${ssid}`)
  // The password is needed only for this one WebSocket frame. Never keep it
  // in shared state, status objects, event records, or browser storage.
  password.value = ''
  showPassword.value = false
}

function setUplink(useWifi) {
  if (busy.value) return
  submit(
    { action: 'select_uplink', use_wifi: useWifi },
    `切换云端上行：${useWifi ? 'Wi-Fi' : 'L610'}`,
  )
}

function successMessage(request) {
  if (request.action === 'set_enabled') {
    return wifiStatus.value.feature_enabled
      ? 'ESP 已确认 Wi-Fi 连接功能开启'
      : `ESP 已确认 Wi-Fi 连接功能关闭，当前上行为 ${activeUplinkLabel.value}`
  }
  if (request.action === 'connect') {
    return `ESP 已确认连接成功，STA 地址 ${wifiStatus.value.ip || 'NA'}`
  }
  if (request.action === 'select_uplink') {
    return `ESP 已确认云端视频和遥测切换到 ${activeUplinkLabel.value}`
  }
  return 'ESP 已确认操作生效'
}

watch(() => props.modelValue, (opened) => {
  password.value = ''
  showPassword.value = false
  localError.value = ''
  if (opened) refreshStatus()
})

watch(() => wifiStatus.value.feature_enabled, (enabled) => {
  if (enabled) return
  selectedSsid.value = ''
  selectedSecurity.value = ''
  selectedNetworkSupported.value = false
  password.value = ''
})

watch(wifiRequest, (request) => {
  if (!request.request_id || !request.record_event || recordedRequests.has(request.request_id)) return
  if (!['success', 'error', 'timeout', 'rejected', 'disconnected'].includes(request.status)) return
  recordedRequests.add(request.request_id)
  const success = request.status === 'success'
  recordWifiEvent({
    operation: request.description,
    success,
    message: success ? successMessage(request) : `未生效：${request.error || request.status}`,
  })
}, { deep: true })
</script>

<style scoped>
.wifi-overlay { position: fixed; inset: 0; z-index: 10020; display: flex; align-items: center; justify-content: center; padding: 24px; background: rgba(0,0,0,.58); backdrop-filter: blur(4px); }
.wifi-dialog { width: min(760px, 94vw); max-height: 90vh; display: flex; flex-direction: column; overflow: hidden; border: 1px solid var(--glass-border); border-radius: 12px; background: var(--glass-bg); box-shadow: 0 24px 80px rgba(0,0,0,.42); color: var(--text-primary); }
.wifi-header { min-height: 62px; display: flex; align-items: flex-start; justify-content: space-between; gap: 12px; padding: 14px 18px; border-bottom: 1px solid var(--border-color); }
.wifi-header h2 { margin: 0; font-size: 17px; font-weight: 700; }
.wifi-header p { margin: 4px 0 0; font-size: 10px; color: var(--text-tertiary); }
.wifi-header p.error { color: #ff3b30; }
.icon-button { width: 30px; height: 30px; border: 0; background: transparent; color: var(--text-secondary); font-size: 24px; line-height: 1; cursor: pointer; }
.wifi-body { padding: 14px 18px 18px; overflow-y: auto; }
.ap-stream-warning { margin: 0 0 12px; padding: 9px 11px; border: 1px solid rgba(245, 158, 11, .48); border-radius: 7px; color: #f59e0b; background: rgba(245, 158, 11, .1); font-size: 10px; font-weight: 700; line-height: 1.5; }
.ap-stream-warning span { display: block; margin-top: 2px; color: #ff3b30; }
.ap-stream-warning.blocking { border-color: rgba(255, 59, 48, .52); }
.status-grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 1px; overflow: hidden; border: 1px solid var(--border-color); border-radius: 7px; background: var(--border-color); }
.status-grid > div { min-width: 0; padding: 9px 10px; background: var(--glass-bg); }
.status-grid span, .connect-form label > span { display: block; font-size: 9px; color: var(--text-tertiary); }
.status-grid strong { display: block; margin-top: 3px; overflow: hidden; font-size: 11px; text-overflow: ellipsis; white-space: nowrap; }
.settings-section { padding: 14px 0; border-bottom: 1px solid var(--border-color); }
.toggle-section, .section-heading { display: flex; align-items: flex-start; justify-content: space-between; gap: 12px; }
.settings-section h3 { margin: 0; font-size: 12px; font-weight: 700; }
.settings-section p { margin: 3px 0 0; font-size: 9px; color: var(--text-tertiary); }
.ios-toggle { position: relative; width: 40px; height: 22px; flex: 0 0 40px; padding: 0; border: 0; border-radius: 11px; background: #9ca3af; cursor: pointer; }
.ios-toggle span { position: absolute; top: 2px; left: 2px; width: 18px; height: 18px; border-radius: 50%; background: #fff; transition: transform .2s ease; box-shadow: 0 1px 3px rgba(0,0,0,.25); }
.ios-toggle.active { background: #34c759; }
.ios-toggle.active span { transform: translateX(18px); }
.ios-toggle.disabled { opacity: .45; cursor: not-allowed; }
.primary-button, .secondary-button { min-height: 30px; padding: 0 12px; border-radius: 6px; font-size: 10px; cursor: pointer; }
.primary-button { border: 1px solid #007aff; background: #007aff; color: #fff; }
.secondary-button { border: 1px solid var(--border-color); background: transparent; color: var(--text-secondary); }
.primary-button:disabled, .secondary-button:disabled { opacity: .42; cursor: not-allowed; }
.network-list { height: 174px; margin-top: 10px; overflow-y: auto; border-top: 1px solid var(--border-color); border-bottom: 1px solid var(--border-color); }
.network-row { width: 100%; min-height: 34px; display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 5px 8px; border: 0; border-bottom: 1px solid var(--border-color); background: transparent; color: var(--text-tertiary); font-size: 9px; cursor: pointer; }
.network-row:hover, .network-row.selected { background: rgba(0,122,255,.08); }
.network-row.selected { box-shadow: inset 3px 0 #007aff; }
.network-row.unsupported { opacity: .45; cursor: not-allowed; }
.network-row.unsupported:hover { background: transparent; }
.network-name { min-width: 0; overflow: hidden; color: var(--text-primary); font-size: 10px; text-overflow: ellipsis; white-space: nowrap; }
.network-empty { height: 100%; display: flex; align-items: center; justify-content: center; color: var(--text-tertiary); font-size: 10px; }
.connect-form { display: grid; grid-template-columns: 1fr 1fr auto; gap: 8px; align-items: end; margin-top: 10px; }
.connect-form label { min-width: 0; }
.connect-form input[type='text'], .connect-form input[type='password'] { width: 100%; height: 30px; margin-top: 4px; padding: 0 8px; border: 1px solid var(--border-color); border-radius: 6px; outline: none; background: transparent; color: var(--text-primary); font-size: 10px; }
.connect-form input:focus { border-color: #007aff; }
.connect-form input:disabled { opacity: .5; }
.password-toggle { grid-column: 1 / 3; display: flex; align-items: center; gap: 5px; }
.password-toggle span { display: inline !important; }
.uplink-control { display: grid; grid-template-columns: 1fr 1fr; gap: 1px; margin-top: 10px; overflow: hidden; border: 1px solid var(--border-color); border-radius: 7px; background: var(--border-color); }
.uplink-control button { height: 34px; border: 0; background: var(--glass-bg); color: var(--text-secondary); font-size: 10px; cursor: pointer; }
.uplink-control button.active { background: #007aff; color: #fff; }
.uplink-control button:disabled { opacity: .42; cursor: not-allowed; }
.local-note { margin: 12px 0 0; font-size: 9px; color: var(--text-tertiary); }
@media (max-width: 640px) { .wifi-overlay { padding: 8px; } .status-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); } .connect-form { grid-template-columns: 1fr; } .password-toggle { grid-column: 1; } }
</style>
