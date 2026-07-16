"""
UDP 帧接收器 v3 (自动检测 JPEG/H.264)
接收 BW21 发送的帧: [帧数据][0x64 0x00 0x00 0x00]
自动检测:
  - JPEG: 0xFF 0xD8 开头 → feed_jpeg_frame() (直通, 无转码)
  - H.264: 0x00 0x00 0x00 0x01 开头 → feed_h264_frame() (ffmpeg 转码)
"""
import asyncio, socket, threading
from mjpeg_server import feed_h264_frame, feed_jpeg_frame

FRAME_MARKER = b'\x64\x00\x00\x00'
NAL_START   = b'\x00\x00\x00\x01'
JPEG_START  = b'\xff\xd8'
_sps_pps = b''
_mode = None  # 'jpeg' or 'h264'


def _extract_sps_pps(frame):
    global _sps_pps
    sps = pps = None
    old_sps_pps = _sps_pps
    data = frame
    while True:
        i = data.find(NAL_START)
        if i < 0: break
        nxt = data.find(NAL_START, i + 4)
        nal = data[i:] if nxt < 0 else data[i:nxt]
        data = b'' if nxt < 0 else data[nxt:]
        if len(nal) > 4:
            t = nal[4] & 0x1F
            if t == 7 and sps is None: sps = nal
            elif t == 8 and pps is None: pps = nal
        if sps and pps: break
    if sps and pps:
        _sps_pps = sps + pps
        if _sps_pps != old_sps_pps:
            profile = sps[5] if len(sps) > 5 else 0
            name = "Baseline" if profile == 66 else "High" if profile == 100 else f"profile={profile}"
            print(f'[UDP] SPS: {name} ({len(sps)}+{len(pps)}B) profile_idc={profile}', flush=True)


def _detect_mode(fd):
    """检测帧类型: 'jpeg' | 'h264' | None"""
    if fd[:2] == JPEG_START:
        return 'jpeg'
    if fd[:4] == NAL_START:
        return 'h264'
    return None


def _udp_reader_thread(port, queue, loop):
    global _sps_pps, _mode
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', port))
    sock.settimeout(1.0)

    print(f'[UDP] listening :{port}', flush=True)
    buf, fc = bytearray(), 0

    try:
        while True:
            try:
                data, addr = sock.recvfrom(65536)
            except socket.timeout: continue
            except OSError: break

            if len(data) < 4: continue

            # 检查帧尾标记
            if data[-4:] == FRAME_MARKER:
                fd = bytes(data[:-4])
                if buf:
                    fd = bytes(buf) + fd
                    buf.clear()

                # 自动检测帧类型
                m = _detect_mode(fd)
                if m and _mode != m:
                    _mode = m
                    print(f'[UDP] MODE={_mode.upper()} detected', flush=True)

                if _mode == 'jpeg':
                    # JPEG 直通模式
                    if fc < 5:
                        print(f'[UDP] JPEG#{fc} {len(fd)}B pre={fd[:16].hex()}', flush=True)
                    loop.call_soon_threadsafe(lambda f=fd: queue.put_nowait(f) if not queue.full() else None)
                    feed_jpeg_frame(fd)
                    fc += 1
                    if fc % 50 == 0:
                        print(f'[UDP] {fc} JPEG frames received', flush=True)

                elif _mode == 'h264':
                    # H.264 转码模式
                    if fc < 5:
                        nals = fd.count(NAL_START)
                        print(f'[UDP] H264#{fc} {len(fd)}B {nals}NAL pre={fd[:32].hex()}', flush=True)

                    if fd.find(NAL_START + b'\x67') >= 0 or fd.find(NAL_START + b'\x27') >= 0:
                        _extract_sps_pps(fd)

                    if _sps_pps and not any(x in fd for x in [
                        NAL_START + b'\x67', NAL_START + b'\x68',
                        NAL_START + b'\x27', NAL_START + b'\x28']):
                        fd = _sps_pps + fd

                    loop.call_soon_threadsafe(lambda f=fd: queue.put_nowait(f) if not queue.full() else None)
                    feed_h264_frame(fd)
                    fc += 1
                    if fc % 50 == 0:
                        print(f'[UDP] {fc} H264 frames received', flush=True)

                else:
                    if fc < 3:
                        print(f'[UDP] unknown format, pre={fd[:32].hex()}', flush=True)

            elif data[:4] == NAL_START:
                # 裸NAL开头 → flush旧帧
                if len(buf) > 100:
                    old = bytes(buf)
                    loop.call_soon_threadsafe(lambda f=old: queue.put_nowait(f) if not queue.full() else None)
                    feed_h264_frame(old)
                    fc += 1
                buf.clear()
                buf.extend(data)
            else:
                buf.extend(data)

            if len(buf) > 2 * 1024 * 1024:
                buf.clear()

    except Exception as e:
        print(f'[UDP] err: {e}', flush=True)
    finally:
        sock.close()
        print(f'[UDP] stopped, {fc} frames total', flush=True)


async def start_udp_receiver(port=9091, queue=None):
    if queue is None:
        queue = asyncio.Queue(maxsize=30)
    t = threading.Thread(target=_udp_reader_thread, args=(port, queue, asyncio.get_running_loop()), daemon=True)
    t.start()
    try:
        while t.is_alive():
            await asyncio.sleep(1)
    except asyncio.CancelledError:
        pass
