"""
WebSocket 视频广播
接收 UDP 队列中的 H.264 帧，广播给所有已连接的 /ws/video 客户端
"""

from aiohttp import web

video_clients = set()


async def ws_video_handler(request):
    """WebSocket 视频流端点"""
    ws = web.WebSocketResponse()
    await ws.prepare(request)

    client_addr = request.remote
    video_clients.add(ws)
    print(f'[VideoWS] 客户端已连接: {client_addr}，当前 {len(video_clients)} 个客户端')

    try:
        # 客户端不发送视频数据，只保持连接
        async for msg in ws:
            if msg.type == web.WSMsgType.TEXT:
                # 可接收客户端的控制消息（如请求关键帧）
                if msg.data == 'request_keyframe':
                    pass  # 未来可转发给 BW21
            elif msg.type == web.WSMsgType.ERROR:
                print(f'[VideoWS] 连接错误: {ws.exception()}')
    except Exception as e:
        print(f'[VideoWS] 异常: {e}')
    finally:
        video_clients.discard(ws)
        print(f'[VideoWS] 客户端已断开: {client_addr}，剩余 {len(video_clients)} 个客户端')

    return ws


async def frame_broadcaster(queue):
    """从 UDP 队列取帧，广播给所有视频 WebSocket 客户端"""
    frame_count = 0
    print('[Broadcast] 帧广播协程已启动')

    while True:
        frame = await queue.get()
        print(f'[Broadcast] got frame: {len(frame)}B, clients={len(video_clients)}', flush=True)

        if not video_clients:
            continue  # 无客户端，丢弃帧

        stale = set()
        for ws in video_clients:
            try:
                await ws.send_bytes(frame)
            except Exception:
                stale.add(ws)

        # 清理断开的客户端
        for ws in stale:
            video_clients.discard(ws)

        frame_count += 1
        if frame_count % 100 == 0:
            print(f'[Broadcast] 已广播 {frame_count} 帧，当前 {len(video_clients)} 个客户端')
