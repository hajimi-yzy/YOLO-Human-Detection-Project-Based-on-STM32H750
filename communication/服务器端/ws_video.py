"""Binary video WebSocket compatible with the existing frontend."""

from aiohttp import web


video_clients = set()


async def ws_video_handler(request):
    ws = web.WebSocketResponse(heartbeat=15)
    await ws.prepare(request)
    video_clients.add(ws)
    print(f'[VideoWS] connected: {request.remote}, clients={len(video_clients)}', flush=True)
    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.ERROR:
                print(f'[VideoWS] error: {ws.exception()}', flush=True)
    finally:
        video_clients.discard(ws)
        print(f'[VideoWS] disconnected: {request.remote}, clients={len(video_clients)}', flush=True)
    return ws


async def frame_broadcaster(queue):
    received_count = 0
    broadcast_count = 0
    print('[Broadcast] latest-frame broadcaster started', flush=True)
    while True:
        frame = await queue.get()
        received_count += 1
        if received_count <= 3 or received_count % 250 == 0:
            print(f'[Broadcast] received#{received_count}: {len(frame)}B, '
                  f'clients={len(video_clients)}', flush=True)
        if not video_clients:
            continue

        stale = set()
        for ws in tuple(video_clients):
            try:
                await ws.send_bytes(frame)
            except Exception:
                stale.add(ws)
        video_clients.difference_update(stale)
        broadcast_count += 1
        if broadcast_count % 250 == 0:
            print(f'[Broadcast] sent#{broadcast_count}, clients={len(video_clients)}', flush=True)
