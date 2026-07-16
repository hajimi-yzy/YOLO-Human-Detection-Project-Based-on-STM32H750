"""Validated unified telemetry UDP receiver and legacy BW21 control return path."""

import asyncio
import copy
import json
import math
import os
import socket
import threading
import time

from ws_gps import broadcast_gps_data
from ws_sensor import broadcast_sensor_data


TELEMETRY_OFFLINE_SECONDS = float(os.getenv("TELEMETRY_OFFLINE_SECONDS", "5"))
TELEMETRY_DATA_STALE_SECONDS = float(os.getenv("TELEMETRY_DATA_STALE_SECONDS", "5"))
BW21_ADDRESS_TTL_SECONDS = 15.0
MAX_TELEMETRY_BYTES = 4096
CONTROL_REQUEST_ID_MAX = 64
PS2_BUTTONS = frozenset({
    "PAD_UP", "PAD_RIGHT", "PAD_DOWN", "PAD_LEFT",
    "L2", "R2", "L1", "R1",
    "TRIANGLE", "CIRCLE", "CROSS", "SQUARE",
    "SELECT", "START",
})
PS2_BUTTON_STATES = frozenset({"down", "up"})
AP_STREAM_STATES = {
    "unknown", "unsupported", "disabled", "starting", "enabled", "stopping", "error",
}


def _empty_sensor(online=False):
    return {
        "online": online,
        "device_id": None,
        "boot_id": None,
        "seq": None,
        "temperature": None,
        "humidity": None,
        "altitude": None,
        "pressure": None,
        "gas": None,
        "person_detected": None,
        "timestamp": None,
        "received_at": None,
        "age_ms": None,
    }


def _empty_gps(online=False):
    return {
        "online": online,
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


_sensor_data = _empty_sensor()
_gps_data = _empty_gps()
_bw21_addr = None
_telemetry_last_seen = 0.0
_data_last_seen = 0.0
_esp_meta = {
    "device_id": None,
    "boot_id": None,
    "seq": None,
    "timestamp": None,
    "received_at": None,
}
_ap_stream_state = {
    "device_id": None,
    "boot_id": None,
    "supported": None,
    "state": "unknown",
    "request_id": None,
    "error": None,
    "received_at": None,
}
_last_sequence = {}
_sock = None
_state_lock = threading.Lock()


def _snapshot_locked(now):
    esp_fresh = (
        _telemetry_last_seen > 0
        and now - _telemetry_last_seen <= TELEMETRY_OFFLINE_SECONDS
    )
    if not esp_fresh:
        return _empty_sensor(), _empty_gps()

    data_fresh = (
        _data_last_seen > 0
        and now - _data_last_seen <= TELEMETRY_DATA_STALE_SECONDS
    )
    age_ms = max(0, int((now - _telemetry_last_seen) * 1000))
    if not data_fresh:
        # ESP/L610 is still online, but STM32 has not supplied a recent frame.
        sensor = {**_empty_sensor(True), **_esp_meta, "online": True, "age_ms": age_ms}
        gps = {**_empty_gps(True), **_esp_meta, "online": True, "age_ms": age_ms}
        return sensor, gps

    sensor = copy.deepcopy(_sensor_data)
    gps = copy.deepcopy(_gps_data)
    sensor.update({"online": True, "age_ms": age_ms})
    gps.update({"online": True, "age_ms": age_ms})
    return sensor, gps


def get_sensor_data():
    with _state_lock:
        sensor, _ = _snapshot_locked(time.monotonic())
        return sensor


def get_gps_data():
    with _state_lock:
        _, gps = _snapshot_locked(time.monotonic())
        return gps


def telemetry_online():
    with _state_lock:
        return (
            _telemetry_last_seen > 0
            and time.monotonic() - _telemetry_last_seen <= TELEMETRY_OFFLINE_SECONDS
        )


def get_ap_stream_state():
    with _state_lock:
        state = copy.deepcopy(_ap_stream_state)
        state["device_id"] = state.get("device_id") or _esp_meta.get("device_id")
        state["boot_id"] = state.get("boot_id") or _esp_meta.get("boot_id")
        state["online"] = (
            _telemetry_last_seen > 0
            and time.monotonic() - _telemetry_last_seen <= TELEMETRY_OFFLINE_SECONDS
        )
        return state


def _finite_number(value):
    return value is None or (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
    )


def _validate_numbers(data, names):
    return all(_finite_number(data.get(name)) for name in names)


def _normalize_gas(value):
    """Normalize the STM32 digital gas flag while retaining legacy metadata."""
    if value is None:
        return True, None
    if isinstance(value, bool) or (
            isinstance(value, int) and not isinstance(value, bool) and value in (0, 1)):
        return True, {"alarm": bool(value)}
    if not isinstance(value, dict):
        return False, None

    normalized = copy.deepcopy(value)
    alarm = normalized.get("alarm")
    if alarm is not None:
        if not (isinstance(alarm, bool) or (
                isinstance(alarm, int) and not isinstance(alarm, bool)
                and alarm in (0, 1))):
            return False, None
        normalized["alarm"] = bool(alarm)
    return True, normalized


def _normalize_request_id(value):
    if value is None:
        return None
    if not isinstance(value, str):
        return None
    value = value.strip()
    if not value or len(value) > CONTROL_REQUEST_ID_MAX:
        return None
    return value


def _normalize_ap_stream(value, fallback_request_id=None):
    if value is None:
        return None
    if not isinstance(value, dict):
        return False

    supported = value.get("supported")
    if not isinstance(supported, bool):
        return False

    state = value.get("state")
    enabled = value.get("enabled")
    if isinstance(state, bool):
        state = "enabled" if state else "disabled"
    if state is None and isinstance(enabled, bool):
        state = "enabled" if enabled else "disabled"
    if state not in AP_STREAM_STATES:
        return False

    request_id_value = value.get("request_id", fallback_request_id)
    request_id = _normalize_request_id(request_id_value)
    if request_id_value is not None and request_id is None:
        return False

    error = value.get("error")
    if error is not None and (not isinstance(error, str) or len(error) > 256):
        return False

    return {
        "supported": supported,
        "state": state,
        "request_id": request_id,
        "error": error,
    }


def _normalize_packet(parsed):
    """Return one sensor+GPS packet; old flat BW21 JSON remains accepted."""
    if not isinstance(parsed, dict):
        return None

    if parsed.get("protocol") == "ESUT/1":
        device_id = str(parsed.get("device_id", "")).strip()
        boot_id = str(parsed.get("boot_id", "")).strip()
        if not device_id or len(device_id) > 64 or not boot_id or len(boot_id) > 64:
            return None

        ap_stream = _normalize_ap_stream(
            parsed.get("ap_stream"), parsed.get("request_id")
        )
        if ap_stream is False:
            return None

        # Heartbeats identify the ESP/L610 path. They deliberately carry no
        # STM32 measurements and therefore never overwrite the last data.
        if parsed.get("kind") == "heartbeat":
            return {
                "device_id": device_id,
                "boot_id": boot_id,
                "seq": None,
                "timestamp": parsed.get("timestamp"),
                "sensor": None,
                "gps": None,
                "heartbeat": True,
                "status_only": True,
                "ap_stream": ap_stream,
                "legacy": False,
            }

        if parsed.get("kind") in ("control_state", "status"):
            if ap_stream is None:
                return None
            return {
                "device_id": device_id,
                "boot_id": boot_id,
                "seq": None,
                "timestamp": parsed.get("timestamp"),
                "sensor": None,
                "gps": None,
                "heartbeat": False,
                "status_only": True,
                "ap_stream": ap_stream,
                "legacy": False,
            }

        sensor = parsed.get("sensor", {})
        gps = parsed.get("gps", {})
        seq = parsed.get("seq")
        if not isinstance(sensor, dict) or not isinstance(gps, dict):
            return None
        if not isinstance(seq, int) or isinstance(seq, bool) or not 0 <= seq <= 0xFFFFFFFF:
            return None
        if not _validate_numbers(sensor, ("temperature", "humidity", "altitude", "pressure")):
            return None
        if not _validate_numbers(gps, ("lat", "lng", "satellites", "speed", "heading")):
            return None
        gas_valid, gas = _normalize_gas(sensor.get("gas"))
        if not gas_valid:
            return None
        sensor = dict(sensor)
        sensor["gas"] = gas
        person = sensor.get("person_detected")
        if person is not None and person not in (0, 1, False, True):
            return None
        return {
            "device_id": device_id,
            "boot_id": boot_id,
            "seq": seq,
            "timestamp": parsed.get("timestamp"),
            "sensor": sensor,
            "gps": gps,
            "heartbeat": False,
            "status_only": False,
            "ap_stream": ap_stream,
            "legacy": False,
        }

    # Compatibility with the existing BW21 flat JSON packet on UDP 9093.
    numeric = (
        "temperature", "humidity", "altitude", "pressure",
        "lat", "lng", "satellites", "speed", "heading", "timestamp",
    )
    if not _validate_numbers(parsed, numeric):
        return None
    gas_valid, gas = _normalize_gas(parsed.get("gas"))
    if not gas_valid:
        return None
    if not any(parsed.get(name) is not None for name in
               ("temperature", "humidity", "altitude", "pressure", "lat", "lng")):
        return None
    sensor = dict(parsed)
    sensor["gas"] = gas
    return {
        "device_id": str(parsed.get("device_id") or "legacy-bw21"),
        "boot_id": str(parsed.get("boot_id") or "legacy"),
        "seq": parsed.get("seq"),
        "timestamp": parsed.get("timestamp"),
        "sensor": sensor,
        "gps": parsed,
        "heartbeat": False,
        "status_only": False,
        "ap_stream": None,
        "legacy": True,
    }


def _is_new_sequence(packet):
    if (packet.get("heartbeat") or packet.get("status_only")
            or packet["legacy"] or packet["seq"] is None):
        return True
    key = (packet["device_id"], packet["boot_id"])
    previous = _last_sequence.get(key)
    current = packet["seq"]
    if previous is not None:
        delta = (current - previous) & 0xFFFFFFFF
        if delta == 0 or delta >= 0x80000000:
            return False
    _last_sequence[key] = current
    if len(_last_sequence) > 128:
        _last_sequence.clear()
        _last_sequence[key] = current
    return True


def _prepare_command_payload(cmd_dict, device_id=None, boot_id=None):
    command = cmd_dict.get("cmd")
    request_id = _normalize_request_id(cmd_dict.get("request_id"))
    if not device_id or not boot_id or request_id is None:
        return None

    payload = {
        "protocol": "ESCTL/1",
        "kind": "command",
        "cmd": command,
        "request_id": request_id,
        "device_id": device_id,
        "boot_id": boot_id,
    }
    if command == "ap_stream":
        enabled = cmd_dict.get("enabled")
        if not isinstance(enabled, bool):
            return None
        payload["params"] = {"enabled": enabled}
        return payload
    if command == "ps2_button":
        button = cmd_dict.get("button")
        state = cmd_dict.get("state")
        if (not isinstance(button, str) or button not in PS2_BUTTONS
                or not isinstance(state, str) or state not in PS2_BUTTON_STATES):
            return None
        payload["params"] = {"button": button, "state": state}
        return payload
    return None


def send_command_to_bw21(cmd_dict):
    with _state_lock:
        sock = _sock
        addr = _bw21_addr
        device_id = _esp_meta.get("device_id")
        boot_id = _esp_meta.get("boot_id")
        freshness_limit = (
            TELEMETRY_OFFLINE_SECONDS
            if cmd_dict.get("cmd") in ("ap_stream", "ps2_button")
            else BW21_ADDRESS_TTL_SECONDS
        )
        fresh = (
            _telemetry_last_seen > 0
            and time.monotonic() - _telemetry_last_seen <= freshness_limit
        )
    if sock is None or addr is None or not fresh:
        print('[SENSOR-UDP] command rejected: no recent validated telemetry address', flush=True)
        return False
    command_payload = _prepare_command_payload(cmd_dict, device_id, boot_id)
    if command_payload is None:
        print('[SENSOR-UDP] command rejected: missing device identity or invalid fields', flush=True)
        return False
    try:
        payload = json.dumps(command_payload, separators=(',', ':')).encode('utf-8')
        sent = sock.sendto(payload, addr)
        print(
            f'[SENSOR-UDP] -> device {sent}B cmd={cmd_dict.get("cmd")} '
            f'request_id={cmd_dict.get("request_id")}',
            flush=True,
        )
        return sent == len(payload)
    except OSError as exc:
        print(f'[SENSOR-UDP] command send failed: {exc}', flush=True)
        return False


def _schedule_broadcast(loop, sensor, gps):
    loop.call_soon_threadsafe(
        lambda value=copy.deepcopy(sensor):
        asyncio.create_task(broadcast_sensor_data(value))
    )
    loop.call_soon_threadsafe(
        lambda value=copy.deepcopy(gps):
        asyncio.create_task(broadcast_gps_data(value))
    )


def _schedule_ap_stream_broadcast(loop, callback, state):
    if callback is None:
        return
    loop.call_soon_threadsafe(
        lambda value=copy.deepcopy(state):
        asyncio.create_task(callback(value))
    )


def _udp_sensor_thread(port, loop, stop_event, ap_stream_callback=None):
    global _sensor_data, _gps_data, _bw21_addr, _telemetry_last_seen, _data_last_seen
    global _esp_meta, _ap_stream_state, _sock
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', port))
    sock.settimeout(0.25)
    with _state_lock:
        _sock = sock
    print(f'[SENSOR-UDP] listening :{port}, offline_timeout={TELEMETRY_OFFLINE_SECONDS:.1f}s', flush=True)
    valid_packets = 0
    invalid_packets = 0
    duplicate_packets = 0
    offline_announced = True

    try:
        while not stop_event.is_set():
            data = None
            addr = None
            try:
                data, addr = sock.recvfrom(MAX_TELEMETRY_BYTES + 1)
            except socket.timeout:
                pass
            except OSError:
                break

            if data is not None:
                if len(data) > MAX_TELEMETRY_BYTES:
                    packet = None
                else:
                    try:
                        packet = _normalize_packet(json.loads(data.decode('utf-8')))
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        packet = None
                if packet is None:
                    invalid_packets += 1
                    continue

                ap_stream_snapshot = None
                with _state_lock:
                    if not _is_new_sequence(packet):
                        duplicate_packets += 1
                        continue

                    now_wall = time.time()
                    meta = {
                        "online": True,
                        "device_id": packet["device_id"],
                        "boot_id": packet["boot_id"],
                        "seq": packet["seq"],
                        "timestamp": packet["timestamp"],
                        "received_at": now_wall,
                        "age_ms": 0,
                    }
                    _esp_meta = {
                        "device_id": packet["device_id"],
                        "boot_id": packet["boot_id"],
                        "seq": packet["seq"],
                        "timestamp": packet["timestamp"],
                        "received_at": now_wall,
                    }
                    if not packet.get("status_only"):
                        sensor = packet["sensor"]
                        gps = packet["gps"]
                        _sensor_data = {
                            **meta,
                            "temperature": sensor.get("temperature"),
                            "humidity": sensor.get("humidity"),
                            "altitude": sensor.get("altitude"),
                            "pressure": sensor.get("pressure"),
                            "gas": copy.deepcopy(sensor.get("gas")),
                            "person_detected": sensor.get("person_detected"),
                        }
                        _gps_data = {
                            **meta,
                            "lat": gps.get("lat"),
                            "lng": gps.get("lng"),
                            "satellites": gps.get("satellites"),
                            "speed": gps.get("speed"),
                            "heading": gps.get("heading"),
                        }
                        _data_last_seen = time.monotonic()
                    if packet.get("ap_stream") is not None:
                        _ap_stream_state = {
                            "device_id": packet["device_id"],
                            "boot_id": packet["boot_id"],
                            **packet["ap_stream"],
                            "received_at": now_wall,
                        }
                        ap_stream_snapshot = {
                            **copy.deepcopy(_ap_stream_state),
                            "online": True,
                        }
                    _bw21_addr = addr
                    _telemetry_last_seen = time.monotonic()
                    sensor_snapshot, gps_snapshot = _snapshot_locked(_telemetry_last_seen)

                _schedule_broadcast(loop, sensor_snapshot, gps_snapshot)
                if ap_stream_snapshot is not None:
                    _schedule_ap_stream_broadcast(
                        loop, ap_stream_callback, ap_stream_snapshot
                    )
                offline_announced = False
                valid_packets += 1
                if valid_packets <= 3 or valid_packets % 50 == 0:
                    print(
                        f'[SENSOR-UDP] valid {"heartbeat" if packet.get("heartbeat") else "data"} packet#{valid_packets} '
                        f'device={packet["device_id"]} seq={packet["seq"]} '
                        f'from {addr[0]}:{addr[1]}',
                        flush=True,
                    )

            with _state_lock:
                stale = (
                    _telemetry_last_seen > 0
                    and time.monotonic() - _telemetry_last_seen > TELEMETRY_OFFLINE_SECONDS
                )
            if stale and not offline_announced:
                offline_announced = True
                _schedule_broadcast(loop, _empty_sensor(), _empty_gps())
                with _state_lock:
                    ap_stream_snapshot = {
                        **copy.deepcopy(_ap_stream_state),
                        "online": False,
                    }
                _schedule_ap_stream_broadcast(
                    loop, ap_stream_callback, ap_stream_snapshot
                )
                print('[SENSOR-UDP] telemetry timeout; device offline and values cleared', flush=True)
    finally:
        with _state_lock:
            _sock = None
            _bw21_addr = None
            _telemetry_last_seen = 0.0
            _data_last_seen = 0.0
            _esp_meta = {
                "device_id": None, "boot_id": None, "seq": None,
                "timestamp": None, "received_at": None,
            }
            _ap_stream_state = {
                "device_id": None, "boot_id": None, "supported": None,
                "state": "unknown", "request_id": None, "error": None,
                "received_at": None,
            }
        sock.close()
        _schedule_broadcast(loop, _empty_sensor(), _empty_gps())
        _schedule_ap_stream_broadcast(
            loop, ap_stream_callback, {**_ap_stream_state, "online": False}
        )
        print(
            f'[SENSOR-UDP] stopped, valid={valid_packets}, invalid={invalid_packets}, '
            f'duplicate={duplicate_packets}',
            flush=True,
        )


async def start_udp_sensor_receiver(port=9093, ap_stream_callback=None):
    stop_event = threading.Event()
    thread = threading.Thread(
        target=_udp_sensor_thread,
        args=(port, asyncio.get_running_loop(), stop_event, ap_stream_callback),
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
        thread.join(timeout=1.5)
