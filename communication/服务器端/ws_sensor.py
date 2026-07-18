"""Sensor WebSocket. Device online state comes from valid unified telemetry only."""

from aiohttp import web


sensor_clients = set()
_latest_data = {
    "online": False,
    "device_id": None,
    "boot_id": None,
    "seq": None,
    "temperature": None,
    "humidity": None,
    "altitude": None,
    "pressure": None,
    "gas": None,
    "person_detected": None,
    "timestamp": None,
    "received_at": None,
    "age_ms": None,
}


async def ws_sensor_handler(request):
    ws = web.WebSocketResponse(heartbeat=15)
    await ws.prepare(request)
    sensor_clients.add(ws)
    print(f'[SensorWS] connected: {request.remote}', flush=True)

    try:
        # Always send a snapshot, including the explicit offline/NA state.
        await ws.send_json(_latest_data)
        async for _ in ws:
            pass
    except Exception as exc:
        print(f'[SensorWS] error: {exc}', flush=True)
    finally:
        sensor_clients.discard(ws)
        print(f'[SensorWS] disconnected: {request.remote}', flush=True)
    return ws


async def broadcast_sensor_data(data):
    global _latest_data, sensor_clients
    _latest_data = dict(data)
    stale = set()
    for ws in tuple(sensor_clients):
        try:
            await ws.send_json(_latest_data)
        except Exception:
            stale.add(ws)
    sensor_clients -= stale
