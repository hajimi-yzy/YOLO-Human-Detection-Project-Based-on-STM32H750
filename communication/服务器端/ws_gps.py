"""
WebSocket GPS 数据通道
服务器 → 前端（实时推送 GPS 定位数据）

数据来源: udp_sensor_receiver.py 从 BW21 UDP :9093 传感器JSON中提取GPS字段
"""
import json
import time
import asyncio
from aiohttp import web

gps_clients = set()

# 最新 GPS 数据（由 udp_sensor_receiver 更新）
_latest_gps = {
    "lat": None,
    "lng": None,
    "heading": 0,
    "speed": 0,
    "satellites": 0,
    "timestamp": 0,
}


def update_gps_data(data: dict):
    """由 UDP 接收器调用，更新最新 GPS 数据"""
    global _latest_gps
    _latest_gps = {
        "lat": data.get("lat"),
        "lng": data.get("lng"),
        "heading": data.get("heading", 0),
        "speed": data.get("speed", 0),
        "satellites": data.get("satellites", 0),
        "timestamp": data.get("timestamp", int(time.time() * 1000)),
    }


async def ws_gps_handler(request):
    """WebSocket GPS 数据端点"""
    ws = web.WebSocketResponse()
    await ws.prepare(request)

    client_addr = request.remote
    gps_clients.add(ws)
    print(f'[GPSWS] 客户端已连接: {client_addr}')

    # 新客户端连接时立即发送一次最新数据
    if _latest_gps.get("lat") is not None:
        try:
            await ws.send_json(_latest_gps)
        except Exception:
            pass

    try:
        async for msg in ws:
            pass  # 客户端不发送数据
    except Exception as e:
        print(f'[GPSWS] 异常: {e}')
    finally:
        gps_clients.discard(ws)
        print(f'[GPSWS] 客户端已断开: {client_addr}')

    return ws


async def broadcast_gps_data(data):
    """向所有 GPS 客户端广播数据"""
    global _latest_gps
    _latest_gps = dict(data)  # 同步更新最新数据
    stale = set()
    for ws in gps_clients:
        try:
            await ws.send_json(data)
        except Exception:
            stale.add(ws)
    gps_clients -= stale
