/**
 * 本地调试 Mock 数据生成器
 * 当 WebSocket 未连接时，使用模拟数据驱动 UI 展示
 */

// 模拟传感器数据
export function mockSensorData() {
  return {
    timestamp: Date.now(),
    gas: {
      concentration: +(Math.random() * 50 + 10).toFixed(1),
      unit: '%LEL',
      alarm: Math.random() > 0.85,
    },
    temperature: +(Math.random() * 15 + 20).toFixed(1),
    humidity: +(Math.random() * 30 + 40).toFixed(1),
    altitude: +(Math.random() * 100 + 50).toFixed(1),
    pressure: +(Math.random() * 30 + 1000).toFixed(1),
  }
}

// 模拟 GPS 数据
export function mockGpsData() {
  const baseLat = 39.9042
  const baseLng = 116.4074
  return {
    timestamp: Date.now(),
    lat: +(baseLat + (Math.random() - 0.5) * 0.005).toFixed(6),
    lng: +(baseLng + (Math.random() - 0.5) * 0.005).toFixed(6),
    heading: +(Math.random() * 360).toFixed(1),
    speed: +(Math.random() * 5).toFixed(1),
    satellites: Math.floor(Math.random() * 8) + 8,
  }
}

// 生成历史趋势数据（最近20个点）
export function mockHistoryData(generator, count = 20) {
  return Array.from({ length: count }, (_, i) => ({
    time: new Date(Date.now() - (count - i) * 2000).toLocaleTimeString(),
    ...generator(),
  }))
}
