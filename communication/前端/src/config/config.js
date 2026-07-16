/**
 * 机器人远程控制面板 - 统一接口配置
 *
 * 切换环境：修改 ENV 为 'production' 或 'development'
 * 打包部署时使用 production，本地调试使用 development
 */

const ENV = 'development'; // 已临时指向远程服务器

// ==================== 正式服务端接口 ====================
const PRODUCTION = {
  // WebSocket 主控地址
  WS_MAIN: 'ws://8.134.38.218:8765/ws/control',

  // WebSocket 传感器数据地址
  WS_SENSOR: 'ws://8.134.38.218:8765/ws/sensor',

  // WebSocket GPS 数据地址
  WS_GPS: 'ws://8.134.38.218:8765/ws/gps',

  // 视频流 MJPEG
  MJPEG_URL: 'http://8.134.38.218:8765/live/mjpeg',

  // HTTP API 基础地址
  API_BASE: 'http://8.134.38.218:8765/api',

  // 地图瓦片服务
  MAP_TILE_URL: 'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
  MAP_CENTER: [39.9042, 116.4074],
  MAP_ZOOM: 15,
  SENSOR_INTERVAL: 1000,
  GPS_INTERVAL: 1000,
}

// ==================== 本地调试接口 ====================
const DEVELOPMENT = {
  WS_MAIN: 'ws://8.134.38.218:8765/ws/control',
  WS_SENSOR: 'ws://8.134.38.218:8765/ws/sensor',
  WS_GPS: 'ws://8.134.38.218:8765/ws/gps',
  RTSP_URL: '',
  // BW21 实时视频流 (MJPEG)
  MJPEG_URL: 'http://8.134.38.218:8765/live/mjpeg',
  // WebSocket 视频流（备用）
  WS_VIDEO: 'ws://8.134.38.218:8765/ws/video',
  API_BASE: 'http://8.134.38.218:8765/api',
  MAP_TILE_URL: 'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
  MAP_CENTER: [39.9042, 116.4074],
  MAP_ZOOM: 15,
  SENSOR_INTERVAL: 2000,
  GPS_INTERVAL: 2000,
}

// ==================== 摇杆/手柄按键映射 ====================
const KEY_MAP = {
  // 方向键 → 控制指令
  ArrowUp: { cmd: 'move', params: { direction: 'forward', speed: 100 } },
  ArrowDown: { cmd: 'move', params: { direction: 'backward', speed: 100 } },
  ArrowLeft: { cmd: 'move', params: { direction: 'left', speed: 100 } },
  ArrowRight: { cmd: 'move', params: { direction: 'right', speed: 100 } },
  // WASD 备用
  w: { cmd: 'move', params: { direction: 'forward', speed: 100 } },
  W: { cmd: 'move', params: { direction: 'forward', speed: 100 } },
  s: { cmd: 'move', params: { direction: 'backward', speed: 100 } },
  S: { cmd: 'move', params: { direction: 'backward', speed: 100 } },
  a: { cmd: 'move', params: { direction: 'left', speed: 100 } },
  A: { cmd: 'move', params: { direction: 'left', speed: 100 } },
  d: { cmd: 'move', params: { direction: 'right', speed: 100 } },
  D: { cmd: 'move', params: { direction: 'right', speed: 100 } },
  // 功能键
  ' ': { cmd: 'stop' },
  q: { cmd: 'rotate', params: { angle: -15 } },
  Q: { cmd: 'rotate', params: { angle: -15 } },
  e: { cmd: 'rotate', params: { angle: 15 } },
  E: { cmd: 'rotate', params: { angle: 15 } },
  r: { cmd: 'speed_up' },
  R: { cmd: 'speed_up' },
  f: { cmd: 'speed_down' },
  F: { cmd: 'speed_down' },
}

// ==================== PS2 手柄按键布局定义 ====================
const PS2_BUTTONS = {
  // 左侧十字方向键
  dpad: [
    { key: 'up', label: '▲', cmd: 'move', params: { direction: 'forward', speed: 100 }, class: 'dpad-up' },
    { key: 'left', label: '◀', cmd: 'move', params: { direction: 'left', speed: 100 }, class: 'dpad-left' },
    { key: 'center', label: '', cmd: null, class: 'dpad-center' },
    { key: 'right', label: '▶', cmd: 'move', params: { direction: 'right', speed: 100 }, class: 'dpad-right' },
    { key: 'down', label: '▼', cmd: 'move', params: { direction: 'backward', speed: 100 }, class: 'dpad-down' },
  ],
  // 右侧功能键
  actions: [
    { key: 'triangle', label: '△', cmd: 'speed_up', color: 'text-green-400' },
    { key: 'circle', label: '○', cmd: 'rotate', params: { angle: 15 }, color: 'text-red-400' },
    { key: 'cross', label: '×', cmd: 'rotate', params: { angle: -15 }, color: 'text-blue-400' },
    { key: 'square', label: '□', cmd: 'speed_down', color: 'text-pink-400' },
  ],
  // 肩键
  shoulders: [
    { key: 'L1', label: 'L1', cmd: 'camera_left' },
    { key: 'R1', label: 'R1', cmd: 'camera_right' },
    { key: 'L2', label: 'L2', cmd: 'laser_off' },
    { key: 'R2', label: 'R2', cmd: 'laser_on' },
  ],
  // Select / Start
  center: [
    { key: 'select', label: 'SELECT', cmd: 'mode_switch' },
    { key: 'start', label: 'START', cmd: 'emergency_stop', color: '!bg-red-500 !text-white' },
  ],
}

const cfg = ENV === 'production' ? PRODUCTION : DEVELOPMENT

export default {
  ...cfg,
  ENV,
  KEY_MAP,
  PS2_BUTTONS,
}
