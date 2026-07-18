"""Video gateway entry point for legacy BW21 and ESP32-S3 ESJP uplinks."""

import asyncio
import os
import signal
import time

from aiohttp import web

from api_handler import setup_api_routes
from mjpeg_server import mjpeg_handler, start_ffmpeg, stop_ffmpeg
from tcp_receiver import start_tcp_receiver
from udp_receiver import start_udp_receiver
from udp_sensor_receiver import start_udp_sensor_receiver
from ws_control import broadcast_ap_stream_state, ws_control_handler
from ws_gps import ws_gps_handler
from ws_sensor import ws_sensor_handler
from ws_video import frame_broadcaster, ws_video_handler


def _env_bool(name, default=True):
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


@web.middleware
async def telemetry_api_cors(request, handler):
    """Allow the separately hosted static control panel to read live state."""
    response = await handler(request)
    if request.path.startswith('/api/'):
        response.headers['Access-Control-Allow-Origin'] = '*'
        response.headers['Access-Control-Allow-Methods'] = 'GET, POST, OPTIONS'
    return response


async def main():
    legacy_enabled = _env_bool("LEGACY_BW21_ENABLED", True)
    runtime = {
        'started_monotonic': time.monotonic(),
        'listeners_ready': False,
    }
    app = web.Application(middlewares=[telemetry_api_cors])
    app['runtime'] = runtime
    app.router.add_get('/ws/video', ws_video_handler)
    app.router.add_get('/ws/control', ws_control_handler)
    app.router.add_get('/ws/sensor', ws_sensor_handler)
    app.router.add_get('/ws/gps', ws_gps_handler)
    setup_api_routes(app)
    app.router.add_get('/live/mjpeg', mjpeg_handler)

    runner = web.AppRunner(app)
    tasks = []
    stop_event = asyncio.Event()

    def request_stop():
        stop_event.set()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, request_stop)
        except NotImplementedError:
            pass

    try:
        await runner.setup()
        await web.TCPSite(runner, '0.0.0.0', 8765).start()

        if legacy_enabled:
            try:
                start_ffmpeg()
            except (FileNotFoundError, OSError) as exc:
                print(f'[WARN] FFmpeg unavailable; legacy H.264 MJPEG disabled: {exc}', flush=True)

        video_queue = asyncio.Queue(maxsize=1)
        tasks.extend([
            asyncio.create_task(
                start_udp_receiver(9091, video_queue, legacy_enabled=legacy_enabled),
                name='udp-video-9091',
            ),
            asyncio.create_task(frame_broadcaster(video_queue), name='video-broadcaster'),
            # UDP 9093 carries both new ESUT/1 telemetry and legacy flat JSON.
            # It must stay available even when legacy video transports are disabled.
            asyncio.create_task(
                start_udp_sensor_receiver(
                    9093, ap_stream_callback=broadcast_ap_stream_state
                ),
                name='udp-telemetry-9093',
            ),
        ])
        if legacy_enabled:
            tasks.append(
                asyncio.create_task(start_tcp_receiver(9092, video_queue), name='tcp-video-9092')
            )

        await asyncio.sleep(0.25)
        failed = [task for task in tasks if task.done()]
        if failed:
            names = ', '.join(task.get_name() for task in failed)
            raise RuntimeError(f'listener task stopped during startup: {names}')
        runtime['listeners_ready'] = True

        print('=' * 60, flush=True)
        print('Video gateway ready', flush=True)
        print('HTTP/WebSocket :8765', flush=True)
        print('ESP32 ESJP v1/v2 UDP :9091', flush=True)
        print(f'Legacy BW21    : {"enabled" if legacy_enabled else "disabled"}', flush=True)
        print('MJPEG          : http://0.0.0.0:8765/live/mjpeg', flush=True)
        print('Health         : http://0.0.0.0:8765/api/health', flush=True)
        print('=' * 60, flush=True)
        await stop_event.wait()
    finally:
        runtime['listeners_ready'] = False
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        stop_ffmpeg()
        await runner.cleanup()
        print('Video gateway stopped', flush=True)


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
