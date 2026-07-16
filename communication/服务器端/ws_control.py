"""
WebSocket 控制指令通道
前端 → 服务器 → BW21 设备
"""

import json
import time
from aiohttp import web
from udp_sensor_receiver import send_command_to_bw21

control_clients = set()


async def ws_control_handler(request):
    """WebSocket 控制指令端点"""
    ws = web.WebSocketResponse()
    await ws.prepare(request)

    client_addr = request.remote
    control_clients.add(ws)
    print(f'[ControlWS] 客户端已连接: {client_addr}')

    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.TEXT:
                try:
                    data = json.loads(msg.data)
                    await handle_control_message(ws, data)
                except json.JSONDecodeError:
                    await ws.send_json({
                        'type': 'error',
                        'message': 'Invalid JSON'
                    })
            elif msg.type == web.WSMsgType.ERROR:
                print(f'[ControlWS] 连接错误: {ws.exception()}')
    except Exception as e:
        print(f'[ControlWS] 异常: {e}')
    finally:
        control_clients.discard(ws)
        print(f'[ControlWS] 客户端已断开: {client_addr}')

    return ws


async def handle_control_message(ws, data):
    """处理控制指令"""
    msg_type = data.get('type')

    if msg_type == 'command_request':
        request_id = data.get('requestId', '')
        cmd = data.get('cmd', '')
        params = data.get('params', {})

        print(f'[Control] 收到指令: {cmd}, requestId: {request_id}')

        # 回复确认（accepted = true 表示已入队）
        await ws.send_json({
            'type': 'command_ack',
            'requestId': request_id,
            'accepted': True,
            'message': 'queued'
        })

        # 转发指令给 BW21 设备（通过 UDP 回传）
        result = send_command_to_bw21({
            "cmd": cmd,
            **params
        })

    else:
        await ws.send_json({
            'type': 'error',
            'message': f'Unknown message type: {msg_type}'
        })


async def broadcast_to_control_clients(message):
    """向所有控制客户端广播消息"""
    stale = set()
    for ws in control_clients:
        try:
            await ws.send_json(message)
        except Exception:
            stale.add(ws)
    control_clients -= stale
