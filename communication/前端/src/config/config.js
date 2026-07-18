/**
 * 机器人远程控制面板 - 统一接口配置
 *
 * 切换环境：修改 ENV 为 'production' 或 'development'
 * 打包部署时使用 production，本地调试使用 development
 */

const ENV = 'development'; // 示例配置；部署前替换为实际地址

// ==================== 正式服务端接口 ====================
const PRODUCTION = {
  // WebSocket 主控地址
  WS_MAIN: 'ws://192.0.2.10:8765/ws/control',

  // WebSocket 传感器数据地址
  WS_SENSOR: 'ws://192.0.2.10:8765/ws/sensor',

  // WebSocket GPS 数据地址
  WS_GPS: 'ws://192.0.2.10:8765/ws/gps',

  // 视频流 MJPEG
  MJPEG_URL: 'http://192.0.2.10:8765/live/mjpeg',

  // HTTP API 基础地址
  API_BASE: 'http://192.0.2.10:8765/api',
  WIFI_MANAGER_BASE: 'http://192.168.4.1:8081/api/wifi',

  // 地图瓦片服务
  MAP_TILE_URL: 'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
  MAP_CENTER: [0, 0],
  MAP_ZOOM: 2,
  SENSOR_INTERVAL: 1000,
  GPS_INTERVAL: 1000,
  TELEMETRY_STALE_MS: 6500,
}

// ==================== 本地调试接口 ====================
const DEVELOPMENT = {
  WS_MAIN: 'ws://192.0.2.10:8765/ws/control',
  WS_SENSOR: 'ws://192.0.2.10:8765/ws/sensor',
  WS_GPS: 'ws://192.0.2.10:8765/ws/gps',
  RTSP_URL: '',
  // BW21 实时视频流 (MJPEG)
  MJPEG_URL: 'http://192.0.2.10:8765/live/mjpeg',
  // WebSocket 视频流（备用）
  WS_VIDEO: 'ws://192.0.2.10:8765/ws/video',
  API_BASE: 'http://192.0.2.10:8765/api',
  WIFI_MANAGER_BASE: 'http://192.168.4.1:8081/api/wifi',
  MAP_TILE_URL: 'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
  MAP_CENTER: [0, 0],
  MAP_ZOOM: 2,
  SENSOR_INTERVAL: 2000,
  GPS_INTERVAL: 2000,
  TELEMETRY_STALE_MS: 6500,
}

// ==================== 键盘到 PS2 按键映射 ====================
const KEY_MAP = {
  ArrowUp: 'PAD_UP',
  ArrowRight: 'PAD_RIGHT',
  ArrowDown: 'PAD_DOWN',
  ArrowLeft: 'PAD_LEFT',
  w: 'PAD_UP',
  W: 'PAD_UP',
  d: 'PAD_RIGHT',
  D: 'PAD_RIGHT',
  s: 'PAD_DOWN',
  S: 'PAD_DOWN',
  a: 'PAD_LEFT',
  A: 'PAD_LEFT',
  q: 'L1',
  Q: 'L1',
  e: 'R1',
  E: 'R1',
  z: 'L2',
  Z: 'L2',
  c: 'R2',
  C: 'R2',
  i: 'TRIANGLE',
  I: 'TRIANGLE',
  l: 'CIRCLE',
  L: 'CIRCLE',
  k: 'CROSS',
  K: 'CROSS',
  j: 'SQUARE',
  J: 'SQUARE',
  Backspace: 'SELECT',
  Enter: 'START',
}

// ==================== PS2 手柄按键布局定义 ====================
const PS2_BUTTONS = {
  // 左侧十字方向键
  dpad: [
    { key: 'PAD_UP', button: 'PAD_UP', label: '▲', class: 'dpad-up' },
    { key: 'PAD_LEFT', button: 'PAD_LEFT', label: '◀', class: 'dpad-left' },
    { key: 'center', label: '', cmd: null, class: 'dpad-center' },
    { key: 'PAD_RIGHT', button: 'PAD_RIGHT', label: '▶', class: 'dpad-right' },
    { key: 'PAD_DOWN', button: 'PAD_DOWN', label: '▼', class: 'dpad-down' },
  ],
  // 右侧功能键
  actions: [
    { key: 'TRIANGLE', button: 'TRIANGLE', label: '△', color: 'text-green-400' },
    { key: 'CIRCLE', button: 'CIRCLE', label: '○', color: 'text-red-400' },
    { key: 'CROSS', button: 'CROSS', label: '×', color: 'text-blue-400' },
    { key: 'SQUARE', button: 'SQUARE', label: '□', color: 'text-pink-400' },
  ],
  // 肩键
  shoulders: [
    { key: 'L1', button: 'L1', label: 'L1' },
    { key: 'R1', button: 'R1', label: 'R1' },
    { key: 'L2', button: 'L2', label: 'L2' },
    { key: 'R2', button: 'R2', label: 'R2' },
  ],
  // Select / Start
  center: [
    { key: 'SELECT', button: 'SELECT', label: 'SELECT' },
    { key: 'START', button: 'START', label: 'START', color: '!bg-red-500 !text-white' },
  ],
}

const cfg = ENV === 'production' ? PRODUCTION : DEVELOPMENT

export default {
  ...cfg,
  ENV,
  KEY_MAP,
  PS2_BUTTONS,
}
