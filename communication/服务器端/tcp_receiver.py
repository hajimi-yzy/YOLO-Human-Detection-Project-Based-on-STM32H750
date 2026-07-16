"""
TCP v34 — H264 Base64 分块重组接收器 (verbose debug)
协议(单行): F seq nchunks orig_len\n  /  D seq idx <base64>\n  /  C seq crc\n
"""
import asyncio, socket, threading, base64

_CRC16_TABLE = None

def _make_crc16_table():
    global _CRC16_TABLE
    if _CRC16_TABLE: return _CRC16_TABLE
    tbl = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = (crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1
        tbl.append(crc & 0xFFFF)
    _CRC16_TABLE = tbl
    return tbl

def crc16(data: bytes, init: int = 0xFFFF) -> int:
    tbl = _make_crc16_table()
    crc = init
    for b in data:
        crc = ((crc << 8) ^ tbl[((crc >> 8) ^ b) & 0xFF]) & 0xFFFF
    return crc

def _handler(conn, addr, queue, loop):
    from mjpeg_server import feed_h264_frame

    print(f'[TCP] connected: {addr}', flush=True)
    buf = bytearray()
    frames = {}
    total_frames = 0
    lc = 0  # line count

    try:
        conn.settimeout(3.0)
        while True:
            try:
                d = conn.recv(65536)
                if not d: break
            except socket.timeout: continue
            except OSError: break

            buf.extend(d)
            if len(buf) > 1024 * 1024:
                buf = buf[-512 * 1024:]

            while b'\n' in buf:
                nl = buf.find(b'\n')
                line = bytes(buf[:nl]).rstrip(b'\r')
                del buf[:nl + 1]
                if not line: continue

                lc += 1
                sp = line.find(b' ')
                if sp < 0:
                    print(f'[TCP] L{lc} NOSPACE: {line[:80]}', flush=True)
                    continue
                typ = line[0:sp]
                rest = line[sp+1:]

                if typ == b'F':
                    parts = rest.split(b' ')
                    if len(parts) < 3:
                        print(f'[TCP] L{lc} F_SHORT: {rest[:80]}', flush=True)
                        continue
                    try:
                        seq = int(parts[0], 16)
                        nchunks = int(parts[1], 16)
                        orig_len = int(parts[2], 16)
                    except:
                        print(f'[TCP] L{lc} F_PARSE: {rest[:80]}', flush=True)
                        continue
                    frames[seq] = {
                        'nchunks': nchunks, 'orig_len': orig_len,
                        'chunks': {}, 'crc_recv': None,
                    }
                    print(f'[TCP] L{lc} F seq={seq} chunks={nchunks} orig={orig_len}B', flush=True)
                    old = [s for s in frames if s < seq - 10]
                    for s in old: del frames[s]

                elif typ == b'D':
                    parts = rest.split(b' ', 2)
                    if len(parts) < 3: continue
                    try:
                        seq = int(parts[0], 16)
                        idx = int(parts[1], 16)
                    except: continue
                    b64_data = parts[2]
                    if seq not in frames:
                        frames[seq] = {'nchunks': 999, 'orig_len': 0, 'chunks': {}, 'crc_recv': None}
                    fr = frames[seq]
                    if idx not in fr['chunks']:
                        fr['chunks'][idx] = b64_data
                        got = len(fr['chunks'])
                        if seq <= 1:
                            print(f'[TCP] L{lc} D seq={seq} idx={idx} len={len(b64_data)} ({got}/{fr["nchunks"]})', flush=True)

                elif typ == b'C':
                    parts = rest.split(b' ')
                    if len(parts) < 2: continue
                    try:
                        seq = int(parts[0], 16)
                        crc_recv = int(parts[1], 16)
                    except: continue

                    print(f'[TCP] L{lc} C seq={seq} crc=0x{crc_recv:04X}', flush=True)
                    if seq not in frames:
                        print(f'[TCP]   C for unknown seq={seq}', flush=True)
                        continue

                    fr = frames[seq]
                    fr['crc_recv'] = crc_recv
                    got = len(fr['chunks'])
                    need = fr['nchunks']
                    print(f'[TCP]   chunks: {got}/{need}', flush=True)

                    if got != need:
                        missing = [i for i in range(need) if i not in fr['chunks']]
                        print(f'[TCP]   MISSING: {missing[:10]}', flush=True)
                        continue

                    # 重组
                    b64_parts = [fr['chunks'][i] for i in range(need)]
                    b64_all = b''.join(b64_parts)

                    # CRC
                    calc_crc = crc16(b64_all)
                    if calc_crc != crc_recv:
                        print(f'[TCP]   CRC_BAD calc=0x{calc_crc:04X}', flush=True)
                        del frames[seq]
                        continue

                    # Base64 decode
                    try:
                        h264_data = base64.b64decode(b64_all)
                    except Exception as e:
                        print(f'[TCP]   b64 decode err: {e}', flush=True)
                        del frames[seq]
                        continue

                    if len(h264_data) != fr['orig_len']:
                        print(f'[TCP]   len mismatch {len(h264_data)} != {fr["orig_len"]}', flush=True)
                        del frames[seq]
                        continue

                    total_frames += 1
                    print(f'[TCP]   => FRAME#{total_frames} OK H264={len(h264_data)}B', flush=True)

                    loop.call_soon_threadsafe(lambda f=h264_data: _feed(f, queue))
                    feed_h264_frame(h264_data)
                    del frames[seq]

                else:
                    if lc <= 10:
                        print(f'[TCP] L{lc} UNKNOWN type={typ} rest={rest[:60]}', flush=True)

    except Exception as e:
        print(f'[TCP] err: {e}', flush=True)
    finally:
        conn.close()
        print(f'[TCP] disc: {total_frames} frames, {lc} lines', flush=True)

def _feed(data, queue):
    try:
        while queue.full():
            queue.get_nowait()
        queue.put_nowait(data)
    except: pass

def _server(port, queue, loop, stop_event):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', port)); s.listen(1); s.settimeout(1.0)
    print(f'[TCP] v34 verbose :{port}', flush=True)
    try:
        while not stop_event.is_set():
            try: c, a = s.accept()
            except socket.timeout: continue
            except OSError: break
            threading.Thread(target=_handler, args=(c, a, queue, loop), daemon=True).start()
    finally:
        s.close()

async def start_tcp_receiver(port=9092, queue=None):
    if queue is None: queue = asyncio.Queue(maxsize=1)
    stop_event = threading.Event()
    t = threading.Thread(target=_server, args=(port, queue, asyncio.get_running_loop(), stop_event), daemon=True)
    t.start()
    try:
        while t.is_alive(): await asyncio.sleep(1)
    except asyncio.CancelledError:
        stop_event.set()
        raise
    finally:
        stop_event.set()
        t.join(timeout=1.5)
