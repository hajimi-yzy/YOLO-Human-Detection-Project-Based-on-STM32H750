"""
HTTP API 路由
兼容前端 PROJECT.md 已定义的 API 契约
"""

import os
import secrets
import time
from aiohttp import web


def setup_api_routes(app):
    """注册所有 API 路由"""
    app.router.add_post('/api/auth/login', api_login)
    app.router.add_get('/api/video/stream', api_video_stream_info)
    app.router.add_get('/api/device/status', api_device_status)
    app.router.add_get('/api/sensor/latest', api_sensor_latest)


async def api_login(request):
    """登录接口"""
    try:
        body = await request.json()
    except Exception:
        return web.json_response({'code': 400, 'message': 'Invalid JSON'}, status=400)

    username = body.get('username', '')
    password = body.get('password', '')
    expected_username = os.environ.get('ROBOT_ADMIN_USER', 'admin')
    expected_password = os.environ.get('ROBOT_ADMIN_PASSWORD', '')

    if not expected_password:
        return web.json_response({
            'code': 503,
            'message': '服务器未配置 ROBOT_ADMIN_PASSWORD'
        }, status=503)

    if (secrets.compare_digest(username, expected_username)
            and secrets.compare_digest(password, expected_password)):
        return web.json_response({
            'code': 200,
            'data': {
                'token': secrets.token_urlsafe(32),
                'username': username
            }
        })
    else:
        return web.json_response({'code': 401, 'message': '用户名或密码错误'}, status=401)


async def api_video_stream_info(request):
    """获取视频流信息"""
    return web.json_response({
        'code': 200,
        'data': {
            'protocol': 'websocket',
            'wsUrl': 'ws://localhost:8765/ws/video',
            'mjpegUrl': 'http://localhost:8765/live/mjpeg',
            'resolution': '640x480',
            'fps': 15,
            'codec': 'h264'
        }
    })


async def api_device_status(request):
    """获取设备状态"""
    return web.json_response({
        'code': 200,
        'data': {
            'bw21': {'online': False, 'ip': None, 'uptime': 0},
            'l610': {'online': False, 'signal': 0},
            'timestamp': time.time()
        }
    })


async def api_sensor_latest(request):
    """获取最新传感器数据"""
    return web.json_response({
        'code': 200,
        'data': {
            'temperature': 25.6,
            'humidity': 65.2,
            'gas': {'co2': 400, 'smoke': 0},
            'timestamp': time.time()
        }
    })
