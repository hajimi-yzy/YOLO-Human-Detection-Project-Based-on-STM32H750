"""Existing frontend control WebSocket and legacy BW21 UDP forwarding."""

import json

from aiohttp import web

from udp_sensor_receiver import (
    CONTROL_REQUEST_ID_MAX,
    MODEM_4G_ACTIONS,
    MODEM_LTE_BANDS,
    PS2_BUTTONS,
    PS2_BUTTON_STATES,
    VIDEO_FPS_OPTIONS,
    VIDEO_RESOLUTIONS,
    WIFI_STA_ACTIONS,
    _valid_wifi_credentials,
    get_ap_stream_state,
    get_modem_4g_state,
    get_video_fps_state,
    get_wifi_sta_state,
    send_command_to_bw21,
)


control_clients = set()
VALID_PS2_BUTTONS = PS2_BUTTONS
VALID_PS2_STATES = PS2_BUTTON_STATES


def _current_control_state():
    ap_stream = get_ap_stream_state()
    modem_4g = get_modem_4g_state()
    video_fps = get_video_fps_state()
    wifi_sta = get_wifi_sta_state()
    return {
        "device_id": (ap_stream.get("device_id") or modem_4g.get("device_id")
                      or video_fps.get("device_id") or wifi_sta.get("device_id")),
        "boot_id": (ap_stream.get("boot_id") or modem_4g.get("boot_id")
                    or video_fps.get("boot_id") or wifi_sta.get("boot_id")),
        "online": bool(ap_stream.get("online") or modem_4g.get("online")
                       or video_fps.get("online") or wifi_sta.get("online")),
        "ap_stream": ap_stream,
        "modem_4g": modem_4g,
        "video_fps": video_fps,
        "wifi_sta": wifi_sta,
    }


def _validated_request_id(value):
    if not isinstance(value, str):
        return None
    value = value.strip()
    if not value or len(value) > CONTROL_REQUEST_ID_MAX:
        return None
    return value


def _validate_command(command, params, request_id=None):
    if command == "ap_stream":
        if (not isinstance(params, dict)
                or not isinstance(params.get("enabled"), bool)):
            return None
        request_id = _validated_request_id(request_id)
        if request_id is None:
            return None
        return {
            "cmd": "ap_stream",
            "enabled": params["enabled"],
            "request_id": request_id,
        }

    if command == "modem_4g":
        request_id = _validated_request_id(request_id)
        if request_id is None or not isinstance(params, dict):
            return None
        action = params.get("action")
        if action not in MODEM_4G_ACTIONS:
            return None
        result = {
            "cmd": "modem_4g",
            "action": action,
            "request_id": request_id,
        }
        if action == "set_bands":
            bands = params.get("bands")
            if (not isinstance(bands, list) or not bands
                    or len(bands) > len(MODEM_LTE_BANDS)
                    or any(not isinstance(item, int) or isinstance(item, bool)
                           or item not in MODEM_LTE_BANDS for item in bands)
                    or len(set(bands)) != len(bands)):
                return None
            result["bands"] = bands
        elif action == "set_cell_lock":
            earfcn = params.get("earfcn")
            pci = params.get("pci")
            if (not isinstance(earfcn, int) or isinstance(earfcn, bool)
                    or not 0 <= earfcn <= 0xFFFFFFFF):
                return None
            if (pci is not None and
                    (not isinstance(pci, int) or isinstance(pci, bool)
                     or not 0 <= pci <= 503)):
                return None
            result["earfcn"] = earfcn
            result["pci"] = pci
        return result

    if command == "video_fps":
        request_id = _validated_request_id(request_id)
        fps = params.get("fps") if isinstance(params, dict) else None
        if (request_id is None or not isinstance(fps, int)
                or isinstance(fps, bool) or fps not in VIDEO_FPS_OPTIONS):
            return None
        result = {"cmd": "video_fps", "fps": fps, "request_id": request_id}
        resolution = params.get("resolution")
        if resolution is not None:
            if resolution not in VIDEO_RESOLUTIONS:
                return None
            result["resolution"] = resolution
        return result

    if command == "wifi_sta":
        request_id = _validated_request_id(request_id)
        if request_id is None or not isinstance(params, dict):
            return None
        action = params.get("action")
        if not isinstance(action, str) or action not in WIFI_STA_ACTIONS:
            return None
        result = {
            "cmd": "wifi_sta",
            "action": action,
            "request_id": request_id,
        }
        if action == "set_enabled":
            enabled = params.get("enabled")
            if not isinstance(enabled, bool):
                return None
            result["enabled"] = enabled
        elif action == "connect":
            ssid = params.get("ssid")
            password = params.get("password")
            security = params.get("security")
            if not _valid_wifi_credentials(ssid, password, security):
                return None
            result.update({
                "ssid": ssid,
                "password": password,
                "security": security,
            })
        elif action == "select_uplink":
            use_wifi = params.get("use_wifi")
            if not isinstance(use_wifi, bool):
                return None
            result["use_wifi"] = use_wifi
        return result

    if command != "ps2_button" or not isinstance(params, dict):
        return None
    request_id = _validated_request_id(request_id)
    button = params.get("button")
    state = params.get("state")
    if (request_id is None
            or not isinstance(button, str) or button not in VALID_PS2_BUTTONS
            or not isinstance(state, str) or state not in VALID_PS2_STATES):
        return None
    return {
        "cmd": "ps2_button",
        "button": button,
        "state": state,
        "request_id": request_id,
    }


async def ws_control_handler(request):
    ws = web.WebSocketResponse(heartbeat=15)
    await ws.prepare(request)
    control_clients.add(ws)
    print(f'[ControlWS] connected: {request.remote}', flush=True)
    try:
        await ws.send_json(_device_state_message(_current_control_state()))
        async for msg in ws:
            if msg.type != web.WSMsgType.TEXT:
                continue
            try:
                data = json.loads(msg.data)
            except json.JSONDecodeError:
                await ws.send_json({'type': 'error', 'message': 'Invalid JSON'})
                continue
            await handle_control_message(ws, data)
    finally:
        control_clients.discard(ws)
        print(f'[ControlWS] disconnected: {request.remote}', flush=True)
    return ws


async def handle_control_message(ws, data):
    if not isinstance(data, dict) or data.get('type') != 'command_request':
        await ws.send_json({'type': 'error', 'message': 'Unknown message type'})
        return

    request_id = data.get('requestId', '')
    command = _validate_command(
        data.get('cmd'), data.get('params', {}), request_id
    )
    if command is None:
        await ws.send_json({
            'type': 'command_ack', 'requestId': request_id,
            'accepted': False, 'message': 'invalid_command',
        })
        return

    sent = send_command_to_bw21(command)
    success_message = (
        'queued'
        if command['cmd'] in ('ap_stream', 'modem_4g', 'video_fps', 'wifi_sta')
        else 'sent'
    )
    await ws.send_json({
        'type': 'command_ack', 'requestId': request_id,
        'accepted': sent, 'message': success_message if sent else 'device_offline',
    })


async def broadcast_to_control_clients(message):
    stale = set()
    for ws in control_clients:
        try:
            await ws.send_json(message)
        except Exception:
            stale.add(ws)
    control_clients.difference_update(stale)


def _device_state_message(state):
    ap_stream = state.get('ap_stream') if isinstance(state.get('ap_stream'), dict) else state
    modem_4g = state.get('modem_4g') if isinstance(state.get('modem_4g'), dict) else {}
    video_fps = state.get('video_fps') if isinstance(state.get('video_fps'), dict) else {}
    wifi_sta = state.get('wifi_sta') if isinstance(state.get('wifi_sta'), dict) else {}
    return {
        'type': 'device_state',
        'device_id': state.get('device_id'),
        'boot_id': state.get('boot_id'),
        'online': bool(state.get('online')),
        'ap_stream': {
            'supported': ap_stream.get('supported'),
            'state': ap_stream.get('state', 'unknown'),
            'request_id': ap_stream.get('request_id'),
            'error': ap_stream.get('error'),
            'received_at': ap_stream.get('received_at'),
        },
        'modem_4g': {
            'supported': modem_4g.get('supported'),
            'state': modem_4g.get('state', 'unknown'),
            'action': modem_4g.get('action', 'query'),
            'request_id': modem_4g.get('request_id'),
            'error': modem_4g.get('error'),
            'operator': modem_4g.get('operator'),
            'registration': modem_4g.get('registration'),
            'rssi': modem_4g.get('rssi'),
            'rat': modem_4g.get('rat'),
            'band_config': modem_4g.get('band_config', ''),
            'cell_lock': modem_4g.get('cell_lock', ''),
            'cells': modem_4g.get('cells', []),
            'received_at': modem_4g.get('received_at'),
        },
        'video_fps': {
            'supported': video_fps.get('supported'),
            'fps': video_fps.get('fps'),
            'resolution': video_fps.get('resolution', '640x480'),
            'width': video_fps.get('width', 640),
            'height': video_fps.get('height', 480),
            'request_id': video_fps.get('request_id'),
            'error': video_fps.get('error'),
            'received_at': video_fps.get('received_at'),
        },
        'wifi_sta': {
            'supported': wifi_sta.get('supported'),
            'state': wifi_sta.get('state', 'unknown'),
            'action': wifi_sta.get('action', 'query'),
            'request_id': wifi_sta.get('request_id'),
            'error': wifi_sta.get('error'),
            'feature_enabled': bool(wifi_sta.get('feature_enabled')),
            'scanning': bool(wifi_sta.get('scanning')),
            'connected': bool(wifi_sta.get('connected')),
            'ssid': wifi_sta.get('ssid', ''),
            'ip': wifi_sta.get('ip', ''),
            'rssi': wifi_sta.get('rssi'),
            'wifi_uplink_selected': bool(wifi_sta.get('wifi_uplink_selected')),
            'active_uplink': wifi_sta.get('active_uplink', 'none'),
            'networks': wifi_sta.get('networks', []),
            'received_at': wifi_sta.get('received_at'),
        },
    }


async def broadcast_ap_stream_state(state):
    await broadcast_to_control_clients(_device_state_message(state))
