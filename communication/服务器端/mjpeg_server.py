"""
H.264 → MJPEG 转码端点 v4 (支持 JPEG 直通)
"""
import asyncio, subprocess, threading
from aiohttp import web

_latest_frame = None
_ffmpeg_proc = None
_reader_thread = None
_stderr_thread = None
_frame_count = 0
_direct_jpeg_mode = False  # True if receiving JPEG directly (no ffmpeg)

JPEG_START = b'\xff\xd8'
JPEG_END = b'\xff\xd9'


def feed_jpeg_frame(jpeg_data):
    """直接接收 JPEG 帧，绕过 ffmpeg"""
    global _latest_frame, _frame_count, _direct_jpeg_mode
    _direct_jpeg_mode = True
    _latest_frame = jpeg_data
    _frame_count += 1
    if _frame_count <= 3:
        print(f'[JPEG] DIRECT #{_frame_count}: {len(jpeg_data)}B', flush=True)
    elif _frame_count % 50 == 0:
        print(f'[JPEG] DIRECT {_frame_count} frames, last {len(jpeg_data)}B', flush=True)


def start_ffmpeg():
    global _ffmpeg_proc, _reader_thread, _stderr_thread

    cmd = [
        'ffmpeg',
        '-f', 'h264',
        '-i', 'pipe:0',
        '-f', 'mjpeg',
        '-q:v', '5',
        '-an',
        '-flush_packets', '1',
        '-loglevel', 'info',
        'pipe:1'
    ]

    _ffmpeg_proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, bufsize=0
    )

    _reader_thread = threading.Thread(target=_read_jpeg_frames, daemon=True)
    _reader_thread.start()
    _stderr_thread = threading.Thread(target=_read_ffmpeg_stderr, daemon=True)
    _stderr_thread.start()

    print('[FFmpeg] started (simple mode)', flush=True)


def _read_ffmpeg_stderr():
    proc = _ffmpeg_proc
    if not proc or not proc.stderr:
        return
    while True:
        line = proc.stderr.readline()
        if not line: break
        text = line.decode('utf-8', errors='replace').strip()
        if text:
            print(f'[FFmpeg] {text}', flush=True)
    print('[FFmpeg] stderr closed', flush=True)


def _read_jpeg_frames():
    global _latest_frame
    proc = _ffmpeg_proc
    if not proc or not proc.stdout:
        print('[FFmpeg] no stdout!', flush=True)
        return

    buf = bytearray()
    jpeg_count = 0
    while True:
        try:
            chunk = proc.stdout.read(4096)
            if not chunk:
                break
            buf.extend(chunk)

            while True:
                start = buf.find(JPEG_START)
                if start < 0:
                    buf.clear(); break
                end = buf.find(JPEG_END, start + 2)
                if end < 0:
                    if start > 0: del buf[:start]
                    break
                _latest_frame = bytes(buf[start:end + 2])
                jpeg_count += 1
                if jpeg_count == 1:
                    print(f'[FFmpeg] FIRST JPEG: {len(_latest_frame)}B', flush=True)
                if jpeg_count <= 3:
                    print(f'[FFmpeg] JPEG#{jpeg_count}: {len(_latest_frame)}B', flush=True)
                del buf[:end + 2]
        except Exception as e:
            print(f'[FFmpeg] read err: {e}', flush=True)
            break

    print(f'[FFmpeg] reader exit, {jpeg_count} JPEGs total', flush=True)


def feed_h264_frame(h264_data):
    proc = _ffmpeg_proc
    if not proc or not proc.stdin:
        return

    if proc.poll() is not None:
        if not hasattr(feed_h264_frame, 'dead'):
            print(f'[FFmpeg] process dead, exitcode={proc.returncode}', flush=True)
            feed_h264_frame.dead = True
        return

    try:
        if not hasattr(feed_h264_frame, 'cnt'):
            feed_h264_frame.cnt = 0
        feed_h264_frame.cnt += 1
        if feed_h264_frame.cnt <= 5:
            print(f'[H264] #{feed_h264_frame.cnt} {len(h264_data)}B '
                  f'pre={h264_data[:16].hex()}', flush=True)
        elif feed_h264_frame.cnt % 50 == 0:
            print(f'[H264] fed {feed_h264_frame.cnt} frames, last {len(h264_data)}B', flush=True)

        proc.stdin.write(h264_data)
        proc.stdin.flush()
    except (BrokenPipeError, OSError) as e:
        print(f'[FFmpeg] stdin broken: {e}', flush=True)


async def mjpeg_handler(request):
    global _latest_frame
    print('[MJPEG] client connected', flush=True)

    response = web.StreamResponse(
        status=200, reason='OK',
        headers={
            'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
            'Cache-Control': 'no-cache, no-store, must-revalidate',
            'Connection': 'keep-alive',
            'Access-Control-Allow-Origin': '*',
        }
    )
    await response.prepare(request)

    waited = 0
    while _latest_frame is None and waited < 150:
        await asyncio.sleep(0.1); waited += 1

    if _latest_frame is None:
        print('[MJPEG] timeout (15s), no frame', flush=True)
        await response.write(
            b'--frame\r\nContent-Type: text/plain\r\n\r\nNo video stream yet. Please wait...\r\n')
        # 继续等
        waited2 = 0
        while _latest_frame is None and waited2 < 600:
            await asyncio.sleep(0.1); waited2 += 1
        if _latest_frame is None:
            print('[MJPEG] timeout (60s), giving up', flush=True)
            return response

    print(f'[MJPEG] first frame: {len(_latest_frame)}B', flush=True)
    fc = 0
    try:
        while True:
            frame = _latest_frame
            if frame:
                try:
                    await response.write(
                        b'--frame\r\n'
                        b'Content-Type: image/jpeg\r\n'
                        b'Content-Length: ' + str(len(frame)).encode() + b'\r\n'
                        b'\r\n' + frame + b'\r\n'
                    )
                    fc += 1
                    if fc <= 3:
                        print(f'[MJPEG] sent frame#{fc} {len(frame)}B', flush=True)
                    elif fc % 100 == 0:
                        print(f'[MJPEG] sent {fc} frames', flush=True)
                except Exception as e:
                    print(f'[MJPEG] write err: {e}', flush=True)
                    break
            await asyncio.sleep(0.04)
    except (ConnectionResetError, asyncio.CancelledError):
        pass

    print(f'[MJPEG] disconnected, {fc} frames sent', flush=True)
    return response
