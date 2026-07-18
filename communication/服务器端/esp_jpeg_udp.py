"""ESP32-S3 JPEG over UDP (ESJP v1/v2) packet reassembly."""

from dataclasses import dataclass, field
import struct
import time
from typing import Dict
import zlib


MAGIC = b"ESJP"
VERSION_V1 = 1
VERSION_V2 = 2
VERSION = VERSION_V1  # Backward-compatible alias used by existing v1 clients/tests.
# OV5640 FHD JPEGs can exceed the former 256 KiB VGA-oriented ceiling.
# Keep a hard bound aligned with firmware so malformed UDP headers cannot
# trigger unbounded allocation/reassembly.
MAX_FRAME_SIZE = 512 * 1024
V1_MAX_CHUNK_PAYLOAD = 1200
V2_MAX_CHUNK_PAYLOAD = 7200
MAX_CHUNK_PAYLOAD = V1_MAX_CHUNK_PAYLOAD  # Backward-compatible v1 alias.
CHUNK_PAYLOAD_BY_VERSION = {
    VERSION_V1: V1_MAX_CHUNK_PAYLOAD,
    VERSION_V2: V2_MAX_CHUNK_PAYLOAD,
}
MAX_CHUNK_COUNT = max(
    (MAX_FRAME_SIZE + payload_size - 1) // payload_size
    for payload_size in CHUNK_PAYLOAD_BY_VERSION.values()
)
FRAME_TIMEOUT_SECONDS = 1.5
MAX_INFLIGHT_FRAMES = 16

# magic, version, flags, header_len, device_id, frame_seq, timestamp_ms,
# frame_len, frame_crc32, chunk_index, chunk_count, payload_len, width, height
HEADER = struct.Struct("!4sBBHIIIIIHHHHH")
HEADER_SIZE = HEADER.size


@dataclass
class _InflightFrame:
    created_at: float
    version: int
    frame_len: int
    crc32: int
    chunk_count: int
    width: int
    height: int
    timestamp_ms: int
    chunks: Dict[int, bytes] = field(default_factory=dict)
    highest_chunk_index: int = -1


@dataclass(frozen=True)
class CompletedJpegFrame:
    source: tuple
    device_id: int
    frame_seq: int
    timestamp_ms: int
    width: int
    height: int
    jpeg: bytes


class EspJpegReassembler:
    def __init__(self, timeout=FRAME_TIMEOUT_SECONDS, now_fn=time.monotonic):
        self.timeout = timeout
        self._now = now_fn
        self._frames = {}
        self._chunk_counts = [0] * MAX_CHUNK_COUNT
        self.stats = {
            "packets": 0,
            "started": 0,
            "completed": 0,
            "invalid": 0,
            "crc_error": 0,
            "jpeg_error": 0,
            "expired": 0,
            "missing_chunks": 0,
            "duplicates": 0,
            "reordered": 0,
            "assembly_ms_total": 0.0,
            "assembly_ms_max": 0.0,
        }

    def _expire(self, now):
        stale = [key for key, frame in self._frames.items()
                 if now - frame.created_at > self.timeout]
        for key in stale:
            frame = self._frames[key]
            self.stats["missing_chunks"] += frame.chunk_count - len(frame.chunks)
            del self._frames[key]
        self.stats["expired"] += len(stale)

    def snapshot(self):
        result = dict(self.stats)
        result["inflight"] = len(self._frames)
        result["chunk_counts"] = list(self._chunk_counts)
        return result

    def push(self, datagram, source):
        """Accept one datagram and return a completed JPEG, or None."""
        self.stats["packets"] += 1
        now = self._now()
        self._expire(now)

        if len(datagram) < HEADER_SIZE:
            self.stats["invalid"] += 1
            return None

        try:
            (magic, version, flags, header_len, device_id, frame_seq,
             timestamp_ms, frame_len, frame_crc32, chunk_index, chunk_count,
             payload_len, width, height) = HEADER.unpack_from(datagram)
        except struct.error:
            self.stats["invalid"] += 1
            return None

        del flags  # Reserved for protocol extensions.
        payload = datagram[HEADER_SIZE:]
        chunk_payload_size = CHUNK_PAYLOAD_BY_VERSION.get(version)
        header_valid = (
            magic == MAGIC and chunk_payload_size is not None
            and header_len == HEADER_SIZE
            and 4 <= frame_len <= MAX_FRAME_SIZE
            and 0 < chunk_count
            and chunk_index < chunk_count
            and 0 < width <= 4096 and 0 < height <= 4096
        )
        if not header_valid:
            self.stats["invalid"] += 1
            return None

        version_max_chunk_count = (
            MAX_FRAME_SIZE + chunk_payload_size - 1
        ) // chunk_payload_size
        expected_count = (
            frame_len + chunk_payload_size - 1
        ) // chunk_payload_size
        expected_payload = min(
            chunk_payload_size,
            frame_len - chunk_index * chunk_payload_size,
        )
        payload_valid = (
            chunk_count <= version_max_chunk_count
            and chunk_count == expected_count
            and payload_len == len(payload)
            and payload_len == expected_payload
        )
        if not payload_valid:
            self.stats["invalid"] += 1
            return None

        self._chunk_counts[chunk_index] += 1

        key = (source, device_id, frame_seq)
        frame = self._frames.get(key)
        if frame is None:
            # Keep all in-flight frames during the reassembly window.  Cellular
            # UDP may reorder a late chunk from frame N behind the first chunk
            # of frame N+1; deleting N here turns harmless reordering into a
            # guaranteed frame loss.
            if len(self._frames) >= MAX_INFLIGHT_FRAMES:
                oldest = min(self._frames, key=lambda item: self._frames[item].created_at)
                old_frame = self._frames[oldest]
                self.stats["missing_chunks"] += (
                    old_frame.chunk_count - len(old_frame.chunks)
                )
                del self._frames[oldest]
                self.stats["expired"] += 1
            frame = _InflightFrame(
                created_at=now,
                version=version,
                frame_len=frame_len,
                crc32=frame_crc32,
                chunk_count=chunk_count,
                width=width,
                height=height,
                timestamp_ms=timestamp_ms,
            )
            self._frames[key] = frame
            self.stats["started"] += 1
        elif (frame.version != version or frame.frame_len != frame_len
              or frame.crc32 != frame_crc32
              or frame.chunk_count != chunk_count or frame.width != width
              or frame.height != height):
            del self._frames[key]
            self.stats["invalid"] += 1
            return None

        previous = frame.chunks.get(chunk_index)
        if previous is not None:
            self.stats["duplicates"] += 1
            if previous != payload:
                del self._frames[key]
                self.stats["invalid"] += 1
            return None
        if chunk_index < frame.highest_chunk_index:
            self.stats["reordered"] += 1
        frame.highest_chunk_index = max(frame.highest_chunk_index, chunk_index)
        frame.chunks[chunk_index] = bytes(payload)

        if len(frame.chunks) != frame.chunk_count:
            return None

        del self._frames[key]
        jpeg = b"".join(frame.chunks[index] for index in range(frame.chunk_count))
        if len(jpeg) != frame.frame_len or (zlib.crc32(jpeg) & 0xFFFFFFFF) != frame.crc32:
            self.stats["crc_error"] += 1
            return None
        if not (jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9")):
            self.stats["jpeg_error"] += 1
            return None

        assembly_ms = (now - frame.created_at) * 1000.0
        self.stats["assembly_ms_total"] += assembly_ms
        self.stats["assembly_ms_max"] = max(
            self.stats["assembly_ms_max"], assembly_ms
        )
        self.stats["completed"] += 1
        return CompletedJpegFrame(
            source=source,
            device_id=device_id,
            frame_seq=frame_seq,
            timestamp_ms=frame.timestamp_ms,
            width=frame.width,
            height=frame.height,
            jpeg=jpeg,
        )
