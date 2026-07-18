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
MODEM_4G_STATES = {
    "unknown", "unsupported", "idle", "querying", "applying", "success", "error",
}
MODEM_4G_ACTIONS = {
    "query", "ping", "reselect", "set_bands", "set_cell_lock", "clear_cell_lock",
}
MODEM_LTE_BANDS = frozenset({1, 3, 5, 7, 8, 20, 34, 39, 40, 41})
MODEM_MAX_CELLS = 6
WIFI_STA_STATES = {
    "unknown", "unsupported", "idle", "applying", "working", "success", "error",
}
WIFI_STA_ACTIONS = {
    "query", "set_enabled", "scan", "connect", "select_uplink",
}
WIFI_SECURITY_MODES = frozenset({
    "open", "wpa", "wpa2", "wpa/wpa2", "wpa3", "wpa2/wpa3",
})
WIFI_MAX_NETWORKS = 10
VIDEO_FPS_OPTIONS = frozenset({5, 8, 15, 20, 30})
VIDEO_RESOLUTIONS = {
    "640x480": (640, 480),
    "1280x720": (1280, 720),
    "1920x1080": (1920, 1080),
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


def _empty_wifi_sta_state():
    return {
        "device_id": None,
        "boot_id": None,
        "supported": None,
        "state": "unknown",
        "action": "query",
        "request_id": None,
        "error": None,
        "feature_enabled": False,
        "scanning": False,
        "connected": False,
        "ssid": "",
        "ip": "",
        "rssi": None,
        "wifi_uplink_selected": False,
        "active_uplink": "none",
        "networks": [],
        "received_at": None,
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
_modem_4g_state = {
    "device_id": None,
    "boot_id": None,
    "supported": None,
    "state": "unknown",
    "action": "query",
    "request_id": None,
    "error": None,
    "operator": None,
    "registration": None,
    "rssi": None,
    "rat": None,
    "band_config": "",
    "cell_lock": "",
    "cells": [],
    "received_at": None,
}
_video_fps_state = {
    "device_id": None,
    "boot_id": None,
    "supported": None,
    "fps": None,
    "resolution": "640x480",
    "width": 640,
    "height": 480,
    "request_id": None,
    "error": None,
    "received_at": None,
}
_wifi_sta_state = _empty_wifi_sta_state()
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


def get_modem_4g_state():
    with _state_lock:
        state = copy.deepcopy(_modem_4g_state)
        state["device_id"] = state.get("device_id") or _esp_meta.get("device_id")
        state["boot_id"] = state.get("boot_id") or _esp_meta.get("boot_id")
        state["online"] = (
            _telemetry_last_seen > 0
            and time.monotonic() - _telemetry_last_seen <= TELEMETRY_OFFLINE_SECONDS
        )
        return state


def get_video_fps_state():
    with _state_lock:
        state = copy.deepcopy(_video_fps_state)
        state["device_id"] = state.get("device_id") or _esp_meta.get("device_id")
        state["boot_id"] = state.get("boot_id") or _esp_meta.get("boot_id")
        state["online"] = (
            _telemetry_last_seen > 0
            and time.monotonic() - _telemetry_last_seen <= TELEMETRY_OFFLINE_SECONDS
        )
        return state


def get_wifi_sta_state():
    with _state_lock:
        state = copy.deepcopy(_wifi_sta_state)
        state["device_id"] = state.get("device_id") or _esp_meta.get("device_id")
        state["boot_id"] = state.get("boot_id") or _esp_meta.get("boot_id")
        state["online"] = (
            _telemetry_last_seen > 0
            and time.monotonic() - _telemetry_last_seen <= TELEMETRY_OFFLINE_SECONDS
        )
        return state


def get_control_state():
    with _state_lock:
        online = (
            _telemetry_last_seen > 0
            and time.monotonic() - _telemetry_last_seen <= TELEMETRY_OFFLINE_SECONDS
        )
        return {
            "device_id": _esp_meta.get("device_id"),
            "boot_id": _esp_meta.get("boot_id"),
            "online": online,
            "ap_stream": copy.deepcopy(_ap_stream_state),
            "modem_4g": copy.deepcopy(_modem_4g_state),
            "video_fps": copy.deepcopy(_video_fps_state),
            "wifi_sta": copy.deepcopy(_wifi_sta_state),
        }


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


def _bounded_int(value, minimum, maximum):
    return (
        isinstance(value, int) and not isinstance(value, bool)
        and minimum <= value <= maximum
    )


def _valid_wifi_credentials(ssid, password, security):
    if (not isinstance(ssid, str) or not ssid or "\x00" in ssid
            or not isinstance(password, str) or "\x00" in password
            or not isinstance(security, str)
            or security not in WIFI_SECURITY_MODES):
        return False
    try:
        ssid_length = len(ssid.encode("utf-8"))
        password_length = len(password.encode("utf-8"))
    except UnicodeEncodeError:
        return False
    if not 1 <= ssid_length <= 32 or password_length > 63:
        return False
    if security == "open":
        return password_length == 0
    return 8 <= password_length <= 63


def _normalize_modem_cell(value):
    if not isinstance(value, dict):
        return None
    if not isinstance(value.get("serving"), bool):
        return None
    if not _bounded_int(value.get("mcc"), 0, 999):
        return None
    if not _bounded_int(value.get("mnc"), 0, 999):
        return None
    tac = value.get("tac")
    cell_id = value.get("cell_id")
    if (not isinstance(tac, str) or not tac or len(tac) > 11
            or not isinstance(cell_id, str) or not cell_id or len(cell_id) > 15):
        return None
    if not _bounded_int(value.get("earfcn"), 0, 0xFFFFFFFF):
        return None
    if not _bounded_int(value.get("pci"), -1, 503):
        return None
    if not _bounded_int(value.get("band"), -1, 255):
        return None
    for field in ("rxlev", "rsrp", "rsrq"):
        if not _bounded_int(value.get(field), -1, 255):
            return None
    return {
        "serving": value["serving"],
        "mcc": value["mcc"],
        "mnc": value["mnc"],
        "tac": tac,
        "cell_id": cell_id,
        "earfcn": value["earfcn"],
        "pci": value["pci"],
        "band": value["band"],
        "rxlev": value["rxlev"],
        "rsrp": value["rsrp"],
        "rsrq": value["rsrq"],
    }


def _normalize_modem_4g(value, fallback_request_id=None):
    if value is None:
        return None
    if not isinstance(value, dict) or not isinstance(value.get("supported"), bool):
        return False
    state = value.get("state")
    action = value.get("action", "query")
    if state not in MODEM_4G_STATES or action not in MODEM_4G_ACTIONS:
        return False
    request_value = value.get("request_id", fallback_request_id)
    request_id = _normalize_request_id(request_value)
    if request_value is not None and request_id is None:
        return False
    error = value.get("error")
    operator = value.get("operator")
    rat = value.get("rat")
    band_config = value.get("band_config", "")
    cell_lock = value.get("cell_lock", "")
    if error is not None and (not isinstance(error, str) or len(error) > 256):
        return False
    if operator is not None and (not isinstance(operator, str) or len(operator) > 64):
        return False
    if rat is not None and (not isinstance(rat, str) or len(rat) > 16):
        return False
    if (not isinstance(band_config, str) or len(band_config) > 160
            or not isinstance(cell_lock, str) or len(cell_lock) > 128):
        return False
    registration = value.get("registration")
    rssi = value.get("rssi")
    if registration is not None and not _bounded_int(registration, 0, 255):
        return False
    if rssi is not None and not _bounded_int(rssi, 0, 99):
        return False
    cell_values = value.get("cells", [])
    if not isinstance(cell_values, list) or len(cell_values) > MODEM_MAX_CELLS:
        return False
    cells = []
    for item in cell_values:
        cell = _normalize_modem_cell(item)
        if cell is None:
            return False
        cells.append(cell)
    return {
        "supported": value["supported"],
        "state": state,
        "action": action,
        "request_id": request_id,
        "error": error,
        "operator": operator,
        "registration": registration,
        "rssi": rssi,
        "rat": rat,
        "band_config": band_config,
        "cell_lock": cell_lock,
        "cells": cells,
    }


def _normalize_video_fps(value, fallback_request_id=None):
    if value is None:
        return None
    if not isinstance(value, dict) or not isinstance(value.get("supported"), bool):
        return False
    fps = value.get("fps")
    if (not isinstance(fps, int) or isinstance(fps, bool)
            or fps not in VIDEO_FPS_OPTIONS):
        return False
    # All firmware before configurable resolution used VGA. Treating an
    # omitted field as VGA keeps those devices compatible without accepting
    # arbitrary dimensions from the network.
    resolution = value.get("resolution", "640x480")
    if resolution not in VIDEO_RESOLUTIONS:
        return False
    width, height = VIDEO_RESOLUTIONS[resolution]
    reported_width = value.get("width", width)
    reported_height = value.get("height", height)
    if reported_width != width or reported_height != height:
        return False
    request_value = value.get("request_id", fallback_request_id)
    request_id = _normalize_request_id(request_value)
    if request_value is not None and request_id is None:
        return False
    error = value.get("error")
    if error is not None and (not isinstance(error, str) or len(error) > 256):
        return False
    return {
        "supported": value["supported"],
        "fps": fps,
        "resolution": resolution,
        "width": width,
        "height": height,
        "request_id": request_id,
        "error": error,
    }


def _normalize_wifi_network(value):
    if not isinstance(value, dict):
        return None
    ssid = value.get("ssid")
    security = value.get("security")
    if not isinstance(ssid, str):
        return None
    try:
        ssid_length = len(ssid.encode("utf-8"))
    except UnicodeEncodeError:
        return None
    if (not ssid or "\x00" in ssid or ssid_length > 32
            or not isinstance(security, str)
            or security not in WIFI_SECURITY_MODES):
        return None
    if not _bounded_int(value.get("rssi"), -127, 0):
        return None
    if not _bounded_int(value.get("channel"), 1, 14):
        return None
    if (not isinstance(value.get("secured"), bool)
            or not isinstance(value.get("supported"), bool)):
        return None
    return {
        "ssid": ssid,
        "rssi": value["rssi"],
        "channel": value["channel"],
        "security": security,
        "secured": value["secured"],
        "supported": value["supported"],
    }


def _normalize_wifi_sta(value, fallback_request_id=None):
    if value is None:
        return None
    if not isinstance(value, dict) or not isinstance(value.get("supported"), bool):
        return False
    state = value.get("state")
    action = value.get("action", "query")
    if (not isinstance(state, str) or state not in WIFI_STA_STATES
            or not isinstance(action, str) or action not in WIFI_STA_ACTIONS):
        return False
    request_value = value.get("request_id", fallback_request_id)
    request_id = _normalize_request_id(request_value)
    if request_value is not None and request_id is None:
        return False
    error = value.get("error")
    if error is not None and (not isinstance(error, str) or len(error) > 256):
        return False
    boolean_fields = (
        "feature_enabled", "scanning", "connected", "wifi_uplink_selected",
    )
    if any(not isinstance(value.get(field), bool) for field in boolean_fields):
        return False
    ssid = value.get("ssid", "")
    ip = value.get("ip", "")
    if not isinstance(ssid, str):
        return False
    try:
        ssid_length = len(ssid.encode("utf-8"))
    except UnicodeEncodeError:
        return False
    if ("\x00" in ssid or ssid_length > 32
            or not isinstance(ip, str) or len(ip) > 45):
        return False
    rssi = value.get("rssi")
    if rssi is not None and not _bounded_int(rssi, -127, 0):
        return False
    active_uplink = value.get("active_uplink")
    if active_uplink not in ("none", "l610", "wifi"):
        return False
    network_values = value.get("networks", [])
    if not isinstance(network_values, list) or len(network_values) > WIFI_MAX_NETWORKS:
        return False
    networks = []
    for item in network_values:
        network = _normalize_wifi_network(item)
        if network is None:
            return False
        networks.append(network)
    # Deliberately construct a whitelist. A password accidentally included by
    # a device is never retained, returned by an API, or broadcast to clients.
    return {
        "supported": value["supported"],
        "state": state,
        "action": action,
        "request_id": request_id,
        "error": error,
        "feature_enabled": value["feature_enabled"],
        "scanning": value["scanning"],
        "connected": value["connected"],
        "ssid": ssid,
        "ip": ip,
        "rssi": rssi,
        "wifi_uplink_selected": value["wifi_uplink_selected"],
        "active_uplink": active_uplink,
        "networks": networks,
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
        modem_4g = _normalize_modem_4g(
            parsed.get("modem_4g"), parsed.get("request_id")
        )
        if modem_4g is False:
            return None
        video_fps = _normalize_video_fps(
            parsed.get("video_fps"), parsed.get("request_id")
        )
        if video_fps is False:
            return None
        wifi_sta = _normalize_wifi_sta(
            parsed.get("wifi_sta"), parsed.get("request_id")
        )
        if wifi_sta is False:
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
                "modem_4g": modem_4g,
                "video_fps": video_fps,
                "wifi_sta": wifi_sta,
                "legacy": False,
            }

        if parsed.get("kind") in ("control_state", "status"):
            if (ap_stream is None and modem_4g is None and video_fps is None
                    and wifi_sta is None):
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
                "modem_4g": modem_4g,
                "video_fps": video_fps,
                "wifi_sta": wifi_sta,
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
            "modem_4g": modem_4g,
            "video_fps": video_fps,
            "wifi_sta": wifi_sta,
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
        "modem_4g": None,
        "video_fps": None,
        "wifi_sta": None,
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
    if command == "modem_4g":
        action = cmd_dict.get("action")
        if action not in MODEM_4G_ACTIONS:
            return None
        params = {"action": action}
        if action == "set_bands":
            bands = cmd_dict.get("bands")
            if (not isinstance(bands, list) or not bands
                    or len(bands) > len(MODEM_LTE_BANDS)
                    or any(not isinstance(item, int) or isinstance(item, bool)
                           or item not in MODEM_LTE_BANDS for item in bands)
                    or len(set(bands)) != len(bands)):
                return None
            params["bands"] = bands
        elif action == "set_cell_lock":
            earfcn = cmd_dict.get("earfcn")
            pci = cmd_dict.get("pci")
            if not _bounded_int(earfcn, 0, 0xFFFFFFFF):
                return None
            if pci is not None and not _bounded_int(pci, 0, 503):
                return None
            params["earfcn"] = earfcn
            params["pci"] = pci
        payload["params"] = params
        return payload
    if command == "video_fps":
        fps = cmd_dict.get("fps")
        if (not isinstance(fps, int) or isinstance(fps, bool)
                or fps not in VIDEO_FPS_OPTIONS):
            return None
        payload["params"] = {"fps": fps}
        resolution = cmd_dict.get("resolution")
        if resolution is not None:
            if resolution not in VIDEO_RESOLUTIONS:
                return None
            payload["params"]["resolution"] = resolution
        return payload
    if command == "wifi_sta":
        action = cmd_dict.get("action")
        if not isinstance(action, str) or action not in WIFI_STA_ACTIONS:
            return None
        params = {"action": action}
        if action == "set_enabled":
            enabled = cmd_dict.get("enabled")
            if not isinstance(enabled, bool):
                return None
            params["enabled"] = enabled
        elif action == "connect":
            ssid = cmd_dict.get("ssid")
            password = cmd_dict.get("password")
            security = cmd_dict.get("security")
            if not _valid_wifi_credentials(ssid, password, security):
                return None
            params.update({
                "ssid": ssid,
                "password": password,
                "security": security,
            })
        elif action == "select_uplink":
            use_wifi = cmd_dict.get("use_wifi")
            if not isinstance(use_wifi, bool):
                return None
            params["use_wifi"] = use_wifi
        payload["params"] = params
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
            if cmd_dict.get("cmd") in (
                "ap_stream", "ps2_button", "modem_4g", "video_fps", "wifi_sta",
            )
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
    global _esp_meta, _ap_stream_state, _modem_4g_state, _video_fps_state
    global _wifi_sta_state, _sock
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

                control_snapshot = None
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
                    if packet.get("modem_4g") is not None:
                        _modem_4g_state = {
                            "device_id": packet["device_id"],
                            "boot_id": packet["boot_id"],
                            **packet["modem_4g"],
                            "received_at": now_wall,
                        }
                    if packet.get("video_fps") is not None:
                        _video_fps_state = {
                            "device_id": packet["device_id"],
                            "boot_id": packet["boot_id"],
                            **packet["video_fps"],
                            "received_at": now_wall,
                        }
                    if packet.get("wifi_sta") is not None:
                        _wifi_sta_state = {
                            "device_id": packet["device_id"],
                            "boot_id": packet["boot_id"],
                            **packet["wifi_sta"],
                            "received_at": now_wall,
                        }
                    if (packet.get("ap_stream") is not None
                            or packet.get("modem_4g") is not None
                            or packet.get("video_fps") is not None
                            or packet.get("wifi_sta") is not None):
                        control_snapshot = {
                            "device_id": packet["device_id"],
                            "boot_id": packet["boot_id"],
                            "online": True,
                            "ap_stream": copy.deepcopy(_ap_stream_state),
                            "modem_4g": copy.deepcopy(_modem_4g_state),
                            "video_fps": copy.deepcopy(_video_fps_state),
                            "wifi_sta": copy.deepcopy(_wifi_sta_state),
                        }
                    _bw21_addr = addr
                    _telemetry_last_seen = time.monotonic()
                    sensor_snapshot, gps_snapshot = _snapshot_locked(_telemetry_last_seen)

                _schedule_broadcast(loop, sensor_snapshot, gps_snapshot)
                if control_snapshot is not None:
                    _schedule_ap_stream_broadcast(
                        loop, ap_stream_callback, control_snapshot
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
                    control_snapshot = {
                        "device_id": _esp_meta.get("device_id"),
                        "boot_id": _esp_meta.get("boot_id"),
                        "online": False,
                        "ap_stream": copy.deepcopy(_ap_stream_state),
                        "modem_4g": copy.deepcopy(_modem_4g_state),
                        "video_fps": copy.deepcopy(_video_fps_state),
                        "wifi_sta": copy.deepcopy(_wifi_sta_state),
                    }
                _schedule_ap_stream_broadcast(
                    loop, ap_stream_callback, control_snapshot
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
            _modem_4g_state = {
                "device_id": None, "boot_id": None, "supported": None,
                "state": "unknown", "action": "query", "request_id": None,
                "error": None, "operator": None, "registration": None,
                "rssi": None, "rat": None, "band_config": "",
                "cell_lock": "", "cells": [], "received_at": None,
            }
            _video_fps_state = {
                "device_id": None, "boot_id": None, "supported": None,
                "fps": None, "resolution": "640x480", "width": 640,
                "height": 480, "request_id": None, "error": None,
                "received_at": None,
            }
            _wifi_sta_state = _empty_wifi_sta_state()
        sock.close()
        _schedule_broadcast(loop, _empty_sensor(), _empty_gps())
        _schedule_ap_stream_broadcast(
            loop, ap_stream_callback, {
                "device_id": None,
                "boot_id": None,
                "online": False,
                "ap_stream": copy.deepcopy(_ap_stream_state),
                "modem_4g": copy.deepcopy(_modem_4g_state),
                "video_fps": copy.deepcopy(_video_fps_state),
                "wifi_sta": copy.deepcopy(_wifi_sta_state),
            }
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
