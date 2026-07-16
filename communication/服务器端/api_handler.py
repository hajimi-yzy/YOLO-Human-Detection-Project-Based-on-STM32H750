"""HTTP API routes used by the existing web frontend."""

import time

from aiohttp import web

from device_registry import device_registry
from mjpeg_server import get_latest_jpeg
from udp_sensor_receiver import (
    get_ap_stream_state,
    get_gps_data,
    get_sensor_data,
    telemetry_online,
)


def setup_api_routes(app):
    app.router.add_post('/api/auth/login', api_login)
    app.router.add_get('/api/video/stream', api_video_stream_info)
    app.router.add_get('/api/video/snapshot', api_video_snapshot)
    app.router.add_get('/api/device/status', api_device_status)
    app.router.add_get('/api/sensor/latest', api_sensor_latest)
    app.router.add_get('/api/telemetry/latest', api_telemetry_latest)
    app.router.add_get('/api/devices', api_devices)
    app.router.add_get('/api/health', api_health)
    app.router.add_get('/api/ready', api_ready)


async def api_login(request):
    try:
        body = await request.json()
    except Exception:
        return web.json_response({'code': 400, 'message': 'Invalid JSON'}, status=400)

    username = body.get('username', '')
    password = body.get('password', '')
    if username == 'demo' and password == 'CHANGE_ME_ADMIN_PASSWORD':
        return web.json_response({
            'code': 200,
            'data': {'token': 'CHANGE_ME_DEMO_TOKEN', 'username': username},
        })
    return web.json_response({'code': 401, 'message': 'Invalid username or password'}, status=401)


def _external_schemes(request):
    forwarded_host = request.headers.get('X-Forwarded-Host')
    host = forwarded_host.split(',')[0].strip() if forwarded_host else request.host
    forwarded_proto = request.headers.get('X-Forwarded-Proto', request.scheme)
    secure = forwarded_proto.split(',')[0].strip().lower() == 'https'
    return host, ('https' if secure else 'http'), ('wss' if secure else 'ws')


async def api_video_stream_info(request):
    host, http_scheme, ws_scheme = _external_schemes(request)
    device = device_registry.latest()
    width = device.get('width') if device else None
    height = device.get('height') if device else None
    return web.json_response({
        'code': 200,
        'data': {
            'protocol': 'mjpeg',
            'wsUrl': f'{ws_scheme}://{host}/ws/video',
            'mjpegUrl': f'{http_scheme}://{host}/live/mjpeg',
            'resolution': f'{width}x{height}' if width and height else None,
            'fps': device.get('fps', 0) if device else 0,
            'codec': 'jpeg',
            'online': bool(device and device['online']),
            'deviceId': device.get('device_id') if device else None,
            'kibPerSecond': device.get('kib_per_second', 0) if device else 0,
        },
    })


async def api_video_snapshot(request):
    frame_id, frame, metadata = get_latest_jpeg()
    headers = {
        'Access-Control-Allow-Origin': '*',
        'Cache-Control': 'no-store, no-cache, must-revalidate, max-age=0',
        'Pragma': 'no-cache',
    }
    if frame is None:
        return web.json_response(
            {'code': 404, 'message': 'No video frame available'},
            status=404,
            headers=headers,
        )
    headers['X-Frame-Id'] = str(frame_id)
    if metadata.get('frame_seq') is not None:
        headers['X-Frame-Sequence'] = str(metadata['frame_seq'])
    return web.Response(body=frame, content_type='image/jpeg', headers=headers)


async def api_device_status(request):
    device = device_registry.latest()
    video_online = bool(device and device['online'])
    sensor = get_sensor_data()
    telemetry_is_online = telemetry_online()
    return web.json_response({
        'code': 200,
        'data': {
            'bw21': {'online': telemetry_is_online, 'ip': None, 'uptime': 0},
            'esp32': device,
            'l610': {
                'online': video_online,
                'ip': device.get('source_ip') if device else None,
                # RSSI is not part of ESJP v1; do not invent a value.
                'signal': None,
            },
            'telemetry': {
                'online': telemetry_is_online,
                'deviceId': sensor.get('device_id'),
                'bootId': sensor.get('boot_id'),
                'sequence': sensor.get('seq'),
                'ageMs': sensor.get('age_ms'),
            },
            'ap_stream': get_ap_stream_state(),
            'timestamp': time.time(),
        },
    })


async def api_sensor_latest(request):
    return web.json_response({'code': 200, 'data': get_sensor_data()})


async def api_telemetry_latest(request):
    return web.json_response({
        'code': 200,
        'data': {
            'online': telemetry_online(),
            'sensor': get_sensor_data(),
            'gps': get_gps_data(),
        },
    })


async def api_devices(request):
    return web.json_response({'code': 200, 'data': device_registry.snapshot()})


async def api_health(request):
    runtime = request.app.get('runtime', {})
    started = runtime.get('started_monotonic', time.monotonic())
    devices = device_registry.snapshot()
    return web.json_response({
        'status': 'ok',
        'uptimeSeconds': round(time.monotonic() - started, 3),
        'devicesOnline': sum(1 for item in devices if item['online']),
    })


async def api_ready(request):
    ready = bool(request.app.get('runtime', {}).get('listeners_ready', False))
    return web.json_response(
        {'status': 'ready' if ready else 'starting'},
        status=200 if ready else 503,
    )
