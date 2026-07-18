<template>
  <Teleport to="body">
    <div v-if="modelValue" class="video-settings-overlay" @click.self="close">
      <section class="video-settings-dialog" role="dialog" aria-modal="true" aria-label="视频流设置">
        <header class="dialog-header">
          <div>
            <h2>视频流设置</h2>
            <p>{{ statusText }}</p>
          </div>
          <button class="close-button" type="button" aria-label="关闭视频流设置" @click="close">×</button>
        </header>

        <div class="dialog-body">
          <section class="current-card">
            <span>当前视频流</span>
            <strong>{{ currentStreamText }}</strong>
          </section>

          <section class="settings-card">
            <div class="section-heading">
              <div>
                <h3>清晰度</h3>
                <p>选择摄像头输出分辨率。</p>
              </div>
            </div>
            <div class="option-grid resolution-grid">
              <button
                v-for="option in resolutionOptions"
                :key="option.value"
                type="button"
                :class="{ active: selectedResolution === option.value }"
                :disabled="videoFpsRequest.pending"
                @click="selectedResolution = option.value"
              >
                <strong>{{ option.shortLabel }}</strong>
                <span>{{ option.fullLabel }}</span>
              </button>
            </div>
          </section>

          <section class="settings-card">
            <div class="section-heading">
              <div>
                <h3>视频帧率</h3>
                <p>选择云端视频每秒发送的画面数量。</p>
              </div>
            </div>
            <div class="option-grid fps-grid">
              <button
                v-for="fps in fpsOptions"
                :key="fps"
                type="button"
                :class="{ active: selectedFps === fps }"
                :disabled="videoFpsRequest.pending"
                @click="selectedFps = fps"
              >{{ fps }} FPS</button>
            </div>
          </section>

          <p class="default-note">默认设置：640×480 / 8 FPS。更高清晰度和更高帧率会增加 4G 上行流量。</p>
          <button class="primary-button" type="button" :disabled="!canApply" @click="applySettings">
            {{ videoFpsRequest.pending ? '等待设备确认…' : '保存并应用' }}
          </button>
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

const resolutionOptions = [
  { value: '640x480', shortLabel: '640', fullLabel: '640 × 480' },
  { value: '1280x720', shortLabel: '720', fullLabel: '1280 × 720' },
  { value: '1920x1080', shortLabel: '1080', fullLabel: '1920 × 1080' },
]
const fpsOptions = [5, 8, 15, 20, 30]
const selectedResolution = ref('640x480')
const selectedFps = ref(8)
const localError = ref('')
const recordedRequests = new Set()

const {
  connected,
  deviceOnline,
  videoFps,
  videoFpsRequest,
  requestVideoFps,
} = useControlChannel()
const { recordVideoEvent } = useEventStore()

function resolutionLabel(value) {
  return resolutionOptions.find((option) => option.value === value)?.fullLabel || '640 × 480'
}

const currentStreamText = computed(() => {
  const resolution = videoFps.value.resolution || '640x480'
  const fps = videoFps.value.fps ?? 8
  return `${resolutionLabel(resolution)} / ${fps} FPS`
})
const statusText = computed(() => {
  if (!connected.value) return '服务器未连接'
  if (deviceOnline.value === false) return '设备离线'
  if (videoFps.value.supported === false) return '当前固件不支持视频流设置'
  if (videoFpsRequest.value.pending) return '正在等待 ESP 应用并确认设置'
  if (localError.value) return localError.value
  if (['error', 'timeout', 'rejected', 'disconnected'].includes(videoFpsRequest.value.status)) {
    return `上次操作失败：${videoFpsRequest.value.error || videoFpsRequest.value.status}`
  }
  return '默认 640×480 / 8 FPS'
})
const canApply = computed(() => (
  connected.value
  && deviceOnline.value !== false
  && videoFps.value.supported !== false
  && !videoFpsRequest.value.pending
))

function close() {
  localError.value = ''
  emit('update:modelValue', false)
}

function applySettings() {
  if (!canApply.value) return
  localError.value = ''
  const requestId = requestVideoFps(selectedFps.value, selectedResolution.value)
  if (requestId) return
  localError.value = '请求未发送：设备离线、连接不可用或已有视频设置正在进行'
  recordVideoEvent({
    operation: `设置视频流：${resolutionLabel(selectedResolution.value)} / ${selectedFps.value} FPS`,
    success: false,
    message: localError.value,
  })
}

watch(() => props.modelValue, (opened) => {
  if (!opened) return
  localError.value = ''
  if (resolutionOptions.some((option) => option.value === videoFps.value.resolution)) {
    selectedResolution.value = videoFps.value.resolution
  }
  if (fpsOptions.includes(videoFps.value.fps)) selectedFps.value = videoFps.value.fps
})

watch(() => videoFps.value.resolution, (value) => {
  if (!videoFpsRequest.value.pending && resolutionOptions.some((option) => option.value === value)) {
    selectedResolution.value = value
  }
})

watch(() => videoFps.value.fps, (value) => {
  if (!videoFpsRequest.value.pending && fpsOptions.includes(value)) selectedFps.value = value
})

watch(videoFpsRequest, (request) => {
  if (!request.request_id || recordedRequests.has(request.request_id)) return
  if (!['success', 'error', 'timeout', 'rejected', 'disconnected'].includes(request.status)) return
  recordedRequests.add(request.request_id)
  const success = request.status === 'success'
  recordVideoEvent({
    operation: `设置视频流：${resolutionLabel(request.resolution)} / ${request.fps} FPS`,
    success,
    message: success
      ? 'ESP 已确认视频流设置生效'
      : `未生效：${request.error || request.status}`,
  })
}, { deep: true })
</script>

<style scoped>
.video-settings-overlay { position: fixed; inset: 0; z-index: 10020; display: flex; align-items: center; justify-content: center; padding: 24px; background: rgba(0,0,0,.58); backdrop-filter: blur(4px); }
.video-settings-dialog { width: min(680px, 94vw); max-height: 90vh; display: flex; flex-direction: column; overflow: hidden; border: 1px solid var(--glass-border); border-radius: 12px; background: var(--glass-bg); box-shadow: 0 24px 80px rgba(0,0,0,.42); color: var(--text-primary); }
.dialog-header { display: flex; align-items: flex-start; justify-content: space-between; gap: 12px; padding: 15px 18px; border-bottom: 1px solid var(--border-color); }
.dialog-header h2 { margin: 0; font-size: 17px; font-weight: 700; }
.dialog-header p { margin: 4px 0 0; font-size: 10px; color: var(--text-tertiary); }
.close-button { border: 0; background: transparent; color: var(--text-secondary); font-size: 24px; line-height: 1; cursor: pointer; }
.dialog-body { padding: 14px 18px 18px; overflow-y: auto; }
.current-card, .settings-card { border: 1px solid var(--border-color); border-radius: 9px; background: rgba(127,127,127,.06); }
.current-card { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 11px 12px; font-size: 10px; color: var(--text-tertiary); }
.current-card strong { color: #007aff; font-size: 12px; }
.settings-card { margin-top: 10px; padding: 12px; }
.section-heading { margin-bottom: 10px; }
.section-heading h3 { margin: 0; font-size: 12px; font-weight: 700; }
.section-heading p { margin: 3px 0 0; color: var(--text-tertiary); font-size: 9px; }
.option-grid { display: grid; gap: 8px; }
.resolution-grid { grid-template-columns: repeat(3, minmax(0, 1fr)); }
.fps-grid { grid-template-columns: repeat(5, minmax(0, 1fr)); }
.option-grid button { min-height: 42px; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 2px; border: 1px solid var(--border-color); border-radius: 7px; background: transparent; color: var(--text-secondary); font-size: 10px; cursor: pointer; }
.option-grid button strong { font-size: 13px; }
.option-grid button span { color: var(--text-tertiary); font-size: 9px; }
.option-grid button.active { border-color: #007aff; background: #007aff; color: #fff; }
.option-grid button.active span { color: rgba(255,255,255,.78); }
.option-grid button:disabled { opacity: .42; cursor: not-allowed; }
.default-note { margin: 12px 2px; color: var(--text-tertiary); font-size: 9px; line-height: 1.5; }
.primary-button { width: 100%; min-height: 34px; border: 1px solid #007aff; border-radius: 7px; background: #007aff; color: #fff; font-size: 10px; cursor: pointer; }
.primary-button:disabled { opacity: .42; cursor: not-allowed; }
@media (max-width: 600px) { .video-settings-overlay { padding: 8px; } .fps-grid { grid-template-columns: repeat(3, minmax(0, 1fr)); } }
</style>
