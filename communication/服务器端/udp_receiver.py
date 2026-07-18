"""UDP video receiver supporting legacy BW21 and ESP32 ESJP v1/v2."""

import asyncio
import socket
import struct
import threading
import time

from esp_jpeg_udp import EspJpegReassembler, MAGIC as ESJP_MAGIC
from device_registry import device_registry
from mjpeg_server import feed_h264_frame, feed_jpeg_frame


FRAME_MARKER = b"\x64\x00\x00\x00"
NAL_START = b"\x00\x00\x00\x01"
JPEG_START = b"\xff\xd8"
_sps_pps = b""
_mode = None
UDP_RECEIVE_BUFFER_BYTES = 4 * 1024 * 1024
STATS_INTERVAL_SECONDS = 5.0


def _queue_frame(loop, queue, frame):
    def put_if_possible():
        while queue.full():
            try:
                queue.get_nowait()
            except asyncio.QueueEmpty:
                break
        queue.put_nowait(frame)
    loop.call_soon_threadsafe(put_if_possible)


def _sequence_is_newer(sequence, previous):
    distance = (sequence - previous) & 0xFFFFFFFF
    return 0 < distance < 0x80000000


def _accept_completed_frame(frame, latest_by_device, now):
    previous = latest_by_device.get(frame.device_id)
    if previous is not None:
        previous_sequence, previous_timestamp, previous_at = previous
        restarted = (
            now - previous_at > 2.0
            or frame.timestamp_ms + 1000 < previous_timestamp
        )
        if not restarted and not _sequence_is_newer(
                frame.frame_seq, previous_sequence):
            return False
    latest_by_device[frame.device_id] = (
        frame.frame_seq, frame.timestamp_ms, now
    )
    return True


def _log_esjp_stats(esjp, kernel_drops, truncated, receive_buffer,
                    previous_stats, late_completed_dropped):
    stats = esjp.snapshot()
    completed = stats["completed"]
    average_ms = (
        stats["assembly_ms_total"] / completed if completed else 0.0
    )
    previous_stats = previous_stats or {
        "packets": 0,
        "started": 0,
        "completed": 0,
        "expired": 0,
        "missing_chunks": 0,
        "chunk_counts": [0] * len(stats["chunk_counts"]),
    }
    chunk_window = [
        current - previous
        for current, previous in zip(
            stats["chunk_counts"], previous_stats["chunk_counts"]
        )
    ]
    chunk_text = ",".join(
        f"{index}:{count}" for index, count in enumerate(chunk_window) if count
    ) or "none"
    print(
        "[ESJP-STATS] "
        f"packets={stats['packets']} started={stats['started']} "
        f"completed={completed} expired={stats['expired']} "
        f"missing_chunks={stats['missing_chunks']} "
        f"reordered={stats['reordered']} duplicates={stats['duplicates']} "
        f"invalid={stats['invalid']} crc_error={stats['crc_error']} "
        f"assembly_avg={average_ms:.1f}ms "
        f"assembly_max={stats['assembly_ms_max']:.1f}ms "
        f"inflight={stats['inflight']} kernel_drops={kernel_drops} "
        f"truncated={truncated} rcvbuf={receive_buffer} "
        f"late_display_drop={late_completed_dropped} "
        f"window_packets={stats['packets'] - previous_stats['packets']} "
        f"window_started={stats['started'] - previous_stats['started']} "
        f"window_completed={stats['completed'] - previous_stats['completed']} "
        f"window_expired={stats['expired'] - previous_stats['expired']} "
        f"window_missing={stats['missing_chunks'] - previous_stats['missing_chunks']} "
        f"window_chunks={chunk_text}",
        flush=True,
    )
    return stats


def _extract_sps_pps(frame):
    global _sps_pps
    sps = pps = None
    old_sps_pps = _sps_pps
    data = frame
    while True:
        index = data.find(NAL_START)
        if index < 0:
            break
        next_index = data.find(NAL_START, index + 4)
        nal = data[index:] if next_index < 0 else data[index:next_index]
        data = b"" if next_index < 0 else data[next_index:]
        if len(nal) > 4:
            nal_type = nal[4] & 0x1F
            if nal_type == 7 and sps is None:
                sps = nal
            elif nal_type == 8 and pps is None:
                pps = nal
        if sps and pps:
            break
    if sps and pps:
        _sps_pps = sps + pps
        if _sps_pps != old_sps_pps:
            profile = sps[5] if len(sps) > 5 else 0
            name = "Baseline" if profile == 66 else "High" if profile == 100 else f"profile={profile}"
            print(f"[UDP] SPS: {name} ({len(sps)}+{len(pps)}B), profile_idc={profile}", flush=True)


def _detect_mode(frame):
    if frame.startswith(JPEG_START):
        return "jpeg"
    if frame.startswith(NAL_START):
        return "h264"
    return None


def _udp_reader_thread(port, queue, loop, stop_event, legacy_enabled):
    global _sps_pps, _mode
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, UDP_RECEIVE_BUFFER_BYTES)
    receive_buffer = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
    overflow_option = getattr(socket, "SO_RXQ_OVFL", 40)
    overflow_enabled = False
    try:
        sock.setsockopt(socket.SOL_SOCKET, overflow_option, 1)
        overflow_enabled = hasattr(sock, "recvmsg")
    except OSError:
        pass
    sock.bind(("0.0.0.0", port))
    sock.settimeout(0.5)
    print(
        f"[UDP] listening on :{port} (legacy BW21 + ESP32 ESJP v1/v2), "
        f"SO_RCVBUF={receive_buffer}, "
        f"SO_RXQ_OVFL={'enabled' if overflow_enabled else 'unsupported'}",
        flush=True,
    )

    legacy_buffer = bytearray()
    legacy_count = 0
    esjp_count = 0
    esjp = EspJpegReassembler()
    kernel_drops = 0
    truncated_datagrams = 0
    next_stats_at = time.monotonic() + STATS_INTERVAL_SECONDS
    previous_stats = None
    latest_by_device = {}
    late_completed_dropped = 0

    try:
        while not stop_event.is_set():
            try:
                if overflow_enabled:
                    data, ancillary, flags, addr = sock.recvmsg(
                        65536, socket.CMSG_SPACE(4)
                    )
                    if flags & getattr(socket, "MSG_TRUNC", 0):
                        truncated_datagrams += 1
                    for level, kind, value in ancillary:
                        if (level == socket.SOL_SOCKET and kind == overflow_option
                                and len(value) >= 4):
                            kernel_drops = struct.unpack("=I", value[:4])[0]
                else:
                    data, addr = sock.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                break

            if data.startswith(ESJP_MAGIC):
                frame = esjp.push(data, addr)
                if frame is not None:
                    completed_at = time.monotonic()
                    if _accept_completed_frame(
                            frame, latest_by_device, completed_at):
                        device_registry.record_jpeg(frame)
                        _queue_frame(loop, queue, frame.jpeg)
                        feed_jpeg_frame(
                            frame.jpeg,
                            device_id=frame.device_id,
                            width=frame.width,
                            height=frame.height,
                            frame_seq=frame.frame_seq,
                        )
                        esjp_count += 1
                        if esjp_count <= 3 or esjp_count % 50 == 0:
                            print(
                                f"[ESJP] frame#{esjp_count} device={frame.device_id:08X} "
                                f"seq={frame.frame_seq} {frame.width}x{frame.height} "
                                f"{len(frame.jpeg)}B from {addr[0]}:{addr[1]}",
                                flush=True,
                            )
                    else:
                        late_completed_dropped += 1
                now = time.monotonic()
                if now >= next_stats_at:
                    previous_stats = _log_esjp_stats(
                        esjp, kernel_drops, truncated_datagrams, receive_buffer,
                        previous_stats, late_completed_dropped
                    )
                    next_stats_at = now + STATS_INTERVAL_SECONDS
                continue

            if not legacy_enabled:
                continue

            if len(data) < 4:
                continue

            # Legacy BW21 framing is intentionally kept unchanged.
            if data.endswith(FRAME_MARKER):
                frame_data = bytes(data[:-4])
                if legacy_buffer:
                    frame_data = bytes(legacy_buffer) + frame_data
                    legacy_buffer.clear()

                detected = _detect_mode(frame_data)
                if detected and _mode != detected:
                    _mode = detected
                    print(f"[UDP] legacy mode={_mode.upper()} detected", flush=True)

                if _mode == "jpeg":
                    _queue_frame(loop, queue, frame_data)
                    feed_jpeg_frame(frame_data)
                    legacy_count += 1
                elif _mode == "h264":
                    if frame_data.find(NAL_START + b"\x67") >= 0 or frame_data.find(NAL_START + b"\x27") >= 0:
                        _extract_sps_pps(frame_data)
                    if _sps_pps and not any(marker in frame_data for marker in (
                        NAL_START + b"\x67", NAL_START + b"\x68",
                        NAL_START + b"\x27", NAL_START + b"\x28",
                    )):
                        frame_data = _sps_pps + frame_data
                    _queue_frame(loop, queue, frame_data)
                    feed_h264_frame(frame_data)
                    legacy_count += 1
                elif legacy_count < 3:
                    print(f"[UDP] unknown legacy format, prefix={frame_data[:32].hex()}", flush=True)
            elif data.startswith(NAL_START):
                if len(legacy_buffer) > 100:
                    old_frame = bytes(legacy_buffer)
                    _queue_frame(loop, queue, old_frame)
                    feed_h264_frame(old_frame)
                    legacy_count += 1
                legacy_buffer.clear()
                legacy_buffer.extend(data)
            else:
                legacy_buffer.extend(data)

            if len(legacy_buffer) > 2 * 1024 * 1024:
                legacy_buffer.clear()
    except Exception as exc:
        print(f"[UDP] error: {exc}", flush=True)
    finally:
        sock.close()
        print(
            f"[UDP] stopped: legacy_frames={legacy_count}, esjp_frames={esjp_count}, "
            f"esjp_stats={esjp.stats}",
            flush=True,
        )


async def start_udp_receiver(port=9091, queue=None, legacy_enabled=True):
    if queue is None:
        queue = asyncio.Queue(maxsize=1)
    stop_event = threading.Event()
    thread = threading.Thread(
        target=_udp_reader_thread,
        args=(port, queue, asyncio.get_running_loop(), stop_event, legacy_enabled),
        daemon=True,
    )
    thread.start()
    try:
        while thread.is_alive():
            await asyncio.sleep(0.5)
    except asyncio.CancelledError:
        stop_event.set()
        raise
    finally:
        stop_event.set()
        thread.join(timeout=1.0)
