"""Existing frontend control WebSocket and legacy BW21 UDP forwarding."""

import json

from aiohttp import web

from udp_sensor_receiver import (
    CONTROL_REQUEST_ID_MAX,
    PS2_BUTTONS,
    PS2_BUTTON_STATES,
    get_ap_stream_state,
    send_command_to_bw21,
)


control_clients = set()
VALID_PS2_BUTTONS = PS2_BUTTONS
VALID_PS2_STATES = PS2_BUTTON_STATES


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
        await ws.send_json(_device_state_message(get_ap_stream_state()))
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
    success_message = 'queued' if command['cmd'] == 'ap_stream' else 'sent'
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
    return {
        'type': 'device_state',
        'device_id': state.get('device_id'),
        'boot_id': state.get('boot_id'),
        'online': bool(state.get('online')),
        'ap_stream': {
            'supported': state.get('supported'),
            'state': state.get('state', 'unknown'),
            'request_id': state.get('request_id'),
            'error': state.get('error'),
            'received_at': state.get('received_at'),
        },
    }


async def broadcast_ap_stream_state(state):
    await broadcast_to_control_clients(_device_state_message(state))
