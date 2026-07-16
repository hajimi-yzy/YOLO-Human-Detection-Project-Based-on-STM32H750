import asyncio
import signal
from aiohttp import web

from udp_receiver import start_udp_receiver
from tcp_receiver import start_tcp_receiver
from udp_sensor_receiver import start_udp_sensor_receiver
from ws_video import ws_video_handler, frame_broadcaster
from ws_control import ws_control_handler
from ws_sensor import ws_sensor_handler, periodic_sensor_broadcast
from ws_gps import ws_gps_handler
from api_handler import setup_api_routes
from mjpeg_server import mjpeg_handler, start_ffmpeg


async def main():
    # 0. 启动 ffmpeg H.264→MJPEG 转码
    try:
        start_ffmpeg()
    except FileNotFoundError:
        print("[WARN] ffmpeg 未安装，MJPEG 端点不可用，其他功能正常", flush=True)

    # 1. UDP H.264 接收队列
    udp_queue = asyncio.Queue(maxsize=30)

    # 2. 创建 aiohttp app
    app = web.Application()

    # 3. WebSocket 路由
    app.router.add_get('/ws/video', ws_video_handler)
    app.router.add_get('/ws/control', ws_control_handler)
    app.router.add_get('/ws/sensor', ws_sensor_handler)
    app.router.add_get('/ws/gps', ws_gps_handler)

    # 4. HTTP API 路由
    setup_api_routes(app)

    # 5. MJPEG 降级端点
    app.router.add_get('/live/mjpeg', mjpeg_handler)

    # 6. 启动 HTTP + WebSocket 服务
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, '0.0.0.0', 8765)
    await site.start()

    print("=" * 50, flush=True)
    print("  视频推流服务器已启动", flush=True)
    print("  HTTP/WS 端口: 8765", flush=True)
    print("  UDP  H264 端口: 9091 (WiFi 版本)", flush=True)
    print("  TCP  H264 端口: 9092 (4G/L610 版本)", flush=True)
    print("  UDP  传感器端口: 9093 (BW21 传感器)", flush=True)
    print("=" * 50, flush=True)
    print("  WebSocket 视频: ws://0.0.0.0:8765/ws/video", flush=True)
    print("  MJPEG 降级:    http://0.0.0.0:8765/live/mjpeg", flush=True)
    print("=" * 50, flush=True)

    # 7. 启动 UDP 接收器、TCP 接收器、传感器接收器和帧广播
    udp_task = asyncio.create_task(start_udp_receiver(port=9091, queue=udp_queue))
    tcp_task = asyncio.create_task(start_tcp_receiver(port=9092, queue=udp_queue))
    sensor_task = asyncio.create_task(start_udp_sensor_receiver(port=9093))
    sensor_bc_task = asyncio.create_task(periodic_sensor_broadcast(interval=5.0))
    broadcast_task = asyncio.create_task(frame_broadcaster(udp_queue))

    # 8. 等待直到被中断
    stop_event = asyncio.Event()

    def _signal_handler():
        print("\n服务器正在停止...", flush=True)
        stop_event.set()

    loop = asyncio.get_event_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _signal_handler)
        except NotImplementedError:
            pass  # Windows 不支持 add_signal_handler

    try:
        await stop_event.wait()
    except asyncio.CancelledError:
        pass
    finally:
        udp_task.cancel()
        tcp_task.cancel()
        sensor_task.cancel()
        sensor_bc_task.cancel()
        broadcast_task.cancel()
        await runner.cleanup()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n服务器已停止", flush=True)
