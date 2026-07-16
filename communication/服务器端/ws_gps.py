"""GPS WebSocket sourced from the same validated unified telemetry packet."""

from aiohttp import web


gps_clients = set()
_latest_gps = {
    "online": False,
    "device_id": None,
    "boot_id": None,
    "seq": None,
    "lat": None,
    "lng": None,
    "satellites": None,
    "speed": None,
    "heading": None,
    "timestamp": None,
    "received_at": None,
    "age_ms": None,
}


async def ws_gps_handler(request):
    ws = web.WebSocketResponse(heartbeat=15)
    await ws.prepare(request)
    gps_clients.add(ws)
    print(f'[GPSWS] connected: {request.remote}', flush=True)

    try:
        await ws.send_json(_latest_gps)
        async for _ in ws:
            pass
    except Exception as exc:
        print(f'[GPSWS] error: {exc}', flush=True)
    finally:
        gps_clients.discard(ws)
        print(f'[GPSWS] disconnected: {request.remote}', flush=True)
    return ws


async def broadcast_gps_data(data):
    global _latest_gps, gps_clients
    _latest_gps = dict(data)
    stale = set()
    for ws in tuple(gps_clients):
        try:
            await ws.send_json(_latest_gps)
        except Exception:
            stale.add(ws)
    gps_clients -= stale
