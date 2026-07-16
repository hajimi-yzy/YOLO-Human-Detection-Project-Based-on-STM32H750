"""
WebSocket 传感器数据通道
服务器 → 前端（实时推送传感器数据）

数据来源: udp_sensor_receiver.py 从 BW21 UDP :9093 接收
"""
import json
import time
import asyncio
from aiohttp import web

sensor_clients = set()

# 最新传感器数据（由 udp_sensor_receiver 更新）
_latest_data = {
    "temperature": None,
    "humidity": None,
    "altitude": None,
    "pressure": None,
    "gas": {},
    "person_detected": 0,
    "timestamp": 0,
}


def update_sensor_data(data: dict):
    """由 UDP 接收器调用，更新最新传感器数据"""
    global _latest_data
    _latest_data = {
        "temperature": data.get("temperature"),
        "humidity": data.get("humidity"),
        "altitude": data.get("altitude"),
        "pressure": data.get("pressure"),
        "gas": data.get("gas", {}),
        "person_detected": data.get("person_detected", 0),
        "timestamp": data.get("timestamp", int(time.time() * 1000)),
    }


async def ws_sensor_handler(request):
    """WebSocket 传感器数据端点"""
    ws = web.WebSocketResponse()
    await ws.prepare(request)

    client_addr = request.remote
    sensor_clients.add(ws)
    print(f'[SensorWS] 客户端已连接: {client_addr}')

    # 新客户端连接时立即发送一次最新数据
    if _latest_data.get("temperature") is not None:
        try:
            await ws.send_json(_latest_data)
        except Exception:
            pass

    try:
        async for msg in ws:
            pass  # 客户端不发送数据
    except Exception as e:
        print(f'[SensorWS] 异常: {e}')
    finally:
        sensor_clients.discard(ws)
        print(f'[SensorWS] 客户端已断开: {client_addr}')

    return ws


async def broadcast_sensor_data(data):
    """向所有传感器客户端广播数据"""
    global _latest_data, sensor_clients
    _latest_data = dict(data)  # 同步更新最新数据
    stale = set()
    for ws in sensor_clients:
        try:
            await ws.send_json(data)
        except Exception:
            stale.add(ws)
    sensor_clients -= stale


async def periodic_sensor_broadcast(interval=5.0):
    """周期性广播：确保未收到 UDP 数据时客户端仍有心跳数据"""
    while True:
        await asyncio.sleep(interval)
        if sensor_clients and _latest_data.get("temperature") is not None:
            await broadcast_sensor_data(_latest_data)
