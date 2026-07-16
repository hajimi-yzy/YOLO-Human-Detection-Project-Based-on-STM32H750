"""Thread-safe runtime state for ESP32 video devices and legacy sensors."""

from collections import deque
import threading
import time


DEVICE_OFFLINE_SECONDS = 15.0
RATE_WINDOW_SECONDS = 5.0


class DeviceRegistry:
    def __init__(self, now_fn=time.monotonic, wall_time_fn=time.time):
        self._now = now_fn
        self._wall_time = wall_time_fn
        self._lock = threading.Lock()
        self._devices = {}

    def record_jpeg(self, frame):
        now = self._now()
        device_id = int(frame.device_id)
        with self._lock:
            device = self._devices.get(device_id)
            if device is None:
                device = {
                    "device_id": device_id,
                    "device_type": "esp32s3-ov5640-l610",
                    "first_seen": self._wall_time(),
                    "frames": 0,
                    "bytes": 0,
                    "samples": deque(),
                }
                self._devices[device_id] = device

            device.update({
                "last_seen_monotonic": now,
                "last_seen": self._wall_time(),
                "source_ip": frame.source[0],
                "source_port": frame.source[1],
                "frame_seq": int(frame.frame_seq),
                "device_timestamp_ms": int(frame.timestamp_ms),
                "width": int(frame.width),
                "height": int(frame.height),
                "jpeg_bytes": len(frame.jpeg),
            })
            device["frames"] += 1
            device["bytes"] += len(frame.jpeg)
            samples = device["samples"]
            samples.append((now, len(frame.jpeg)))
            cutoff = now - RATE_WINDOW_SECONDS
            while samples and samples[0][0] < cutoff:
                samples.popleft()

    def _snapshot_device(self, device, now):
        samples = device["samples"]
        if len(samples) >= 2:
            duration = max(samples[-1][0] - samples[0][0], 0.001)
            fps = (len(samples) - 1) / duration
            bytes_per_second = sum(
                size for index, (_, size) in enumerate(samples) if index > 0
            ) / duration
        else:
            fps = 0.0
            bytes_per_second = 0.0

        result = {key: value for key, value in device.items()
                  if key not in ("samples", "last_seen_monotonic")}
        result.update({
            "device_id": f'{device["device_id"]:08X}',
            "online": now - device.get("last_seen_monotonic", 0) <= DEVICE_OFFLINE_SECONDS,
            "fps": round(fps, 2),
            "bytes_per_second": round(bytes_per_second, 1),
            "kib_per_second": round(bytes_per_second / 1024.0, 1),
        })
        return result

    def snapshot(self):
        now = self._now()
        with self._lock:
            return [self._snapshot_device(device, now)
                    for device in self._devices.values()]

    def latest(self):
        devices = self.snapshot()
        if not devices:
            return None
        return max(devices, key=lambda item: item.get("last_seen", 0))

    def any_online(self):
        return any(device["online"] for device in self.snapshot())

    def clear(self):
        with self._lock:
            self._devices.clear()


device_registry = DeviceRegistry()
