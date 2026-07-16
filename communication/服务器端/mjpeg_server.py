"""H.264-to-MJPEG bridge plus direct JPEG publishing."""

import asyncio
import subprocess
import threading

from aiohttp import web


JPEG_START = b"\xff\xd8"
JPEG_END = b"\xff\xd9"
MAX_JPEG_SIZE = 256 * 1024

_latest_frame = None
_latest_frame_id = 0
_latest_metadata = {}
_frame_lock = threading.Lock()
_ffmpeg_proc = None
_reader_thread = None
_stderr_thread = None
_frame_count = 0
_direct_jpeg_mode = False


def _publish_jpeg(jpeg_data, metadata=None):
    global _latest_frame, _latest_frame_id, _latest_metadata
    with _frame_lock:
        _latest_frame = bytes(jpeg_data)
        _latest_frame_id += 1
        _latest_metadata = dict(metadata or {})
        return _latest_frame_id


def _get_latest_jpeg():
    with _frame_lock:
        return _latest_frame_id, _latest_frame, dict(_latest_metadata)


def get_latest_jpeg():
    """Return an immutable snapshot of the latest validated/published JPEG."""
    return _get_latest_jpeg()


def get_video_status():
    frame_id, frame, metadata = _get_latest_jpeg()
    return {
        "frame_id": frame_id,
        "jpeg_bytes": len(frame) if frame is not None else 0,
        "metadata": metadata,
        "direct_jpeg": _direct_jpeg_mode,
        "ffmpeg_running": _ffmpeg_proc is not None and _ffmpeg_proc.poll() is None,
    }


def feed_jpeg_frame(jpeg_data, *, device_id=None, width=None, height=None, frame_seq=None):
    """Publish one verified JPEG directly without FFmpeg transcoding."""
    global _frame_count, _direct_jpeg_mode
    if not isinstance(jpeg_data, (bytes, bytearray, memoryview)):
        return False
    if not (4 <= len(jpeg_data) <= MAX_JPEG_SIZE):
        return False
    if not (jpeg_data[:2] == JPEG_START and jpeg_data[-2:] == JPEG_END):
        return False

    metadata = {
        "device_id": device_id,
        "width": width,
        "height": height,
        "frame_seq": frame_seq,
    }
    _direct_jpeg_mode = True
    _publish_jpeg(jpeg_data, metadata)
    _frame_count += 1
    if _frame_count <= 3 or _frame_count % 50 == 0:
        source = f" device={device_id:08X}" if device_id is not None else ""
        print(f"[JPEG] direct frame#{_frame_count}: {len(jpeg_data)}B{source}", flush=True)
    return True


def start_ffmpeg():
    global _ffmpeg_proc, _reader_thread, _stderr_thread
    if _ffmpeg_proc is not None and _ffmpeg_proc.poll() is None:
        return
    command = [
        "ffmpeg", "-f", "h264", "-i", "pipe:0", "-f", "mjpeg",
        "-q:v", "5", "-an", "-flush_packets", "1", "-loglevel", "info", "pipe:1",
    ]
    _ffmpeg_proc = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    _reader_thread = threading.Thread(target=_read_jpeg_frames, daemon=True)
    _reader_thread.start()
    _stderr_thread = threading.Thread(target=_read_ffmpeg_stderr, daemon=True)
    _stderr_thread.start()
    print("[FFmpeg] started", flush=True)


def stop_ffmpeg():
    global _ffmpeg_proc, _reader_thread, _stderr_thread
    process = _ffmpeg_proc
    _ffmpeg_proc = None
    if process is None:
        return
    if process.stdin:
        try:
            process.stdin.close()
        except OSError:
            pass
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=1.0)
    for thread in (_reader_thread, _stderr_thread):
        if thread is not None and thread.is_alive():
            thread.join(timeout=1.0)
    _reader_thread = None
    _stderr_thread = None
    print("[FFmpeg] stopped", flush=True)


def _read_ffmpeg_stderr():
    process = _ffmpeg_proc
    if not process or not process.stderr:
        return
    while True:
        line = process.stderr.readline()
        if not line:
            break
        text = line.decode("utf-8", errors="replace").strip()
        if text:
            print(f"[FFmpeg] {text}", flush=True)


def _read_jpeg_frames():
    process = _ffmpeg_proc
    if not process or not process.stdout:
        return
    buffer = bytearray()
    jpeg_count = 0
    while True:
        try:
            chunk = process.stdout.read(4096)
            if not chunk:
                break
            buffer.extend(chunk)
            while True:
                start = buffer.find(JPEG_START)
                if start < 0:
                    buffer.clear()
                    break
                end = buffer.find(JPEG_END, start + 2)
                if end < 0:
                    if start > 0:
                        del buffer[:start]
                    break
                jpeg = bytes(buffer[start:end + 2])
                _publish_jpeg(jpeg, {"source": "ffmpeg"})
                jpeg_count += 1
                if jpeg_count <= 3:
                    print(f"[FFmpeg] JPEG#{jpeg_count}: {len(jpeg)}B", flush=True)
                del buffer[:end + 2]
        except Exception as exc:
            print(f"[FFmpeg] read error: {exc}", flush=True)
            break
    print(f"[FFmpeg] reader stopped after {jpeg_count} frames", flush=True)


def feed_h264_frame(h264_data):
    process = _ffmpeg_proc
    if not process or not process.stdin or process.poll() is not None:
        return
    try:
        process.stdin.write(h264_data)
        process.stdin.flush()
    except (BrokenPipeError, OSError) as exc:
        print(f"[FFmpeg] stdin error: {exc}", flush=True)


async def mjpeg_handler(request):
    print("[MJPEG] client connected", flush=True)
    response = web.StreamResponse(
        status=200,
        reason="OK",
        headers={
            "Content-Type": "multipart/x-mixed-replace; boundary=frame",
            "Cache-Control": "no-cache, no-store, must-revalidate",
            "Connection": "keep-alive",
            "Access-Control-Allow-Origin": "*",
        },
    )
    await response.prepare(request)

    last_frame_id = 0
    sent_count = 0
    try:
        while True:
            frame_id, frame, metadata = _get_latest_jpeg()
            if frame is None or frame_id == last_frame_id:
                await asyncio.sleep(0.01)
                continue
            await response.write(
                b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                + str(len(frame)).encode("ascii")
                + b"\r\n\r\n"
                + frame
                + b"\r\n"
            )
            last_frame_id = frame_id
            sent_count += 1
            if sent_count <= 3 or sent_count % 100 == 0:
                print(f"[MJPEG] sent frame#{sent_count} {len(frame)}B {metadata}", flush=True)
    except (ConnectionResetError, asyncio.CancelledError, RuntimeError):
        pass
    finally:
        print(f"[MJPEG] disconnected after {sent_count} frames", flush=True)
    return response
