"""HTTP API routes used by the existing web frontend."""

import asyncio
import math
import os
import time

import aiohttp
from aiohttp import web

from device_registry import device_registry
from mjpeg_server import get_latest_jpeg
from udp_sensor_receiver import (
    get_ap_stream_state,
    get_gps_data,
    get_modem_4g_state,
    get_sensor_data,
    get_video_fps_state,
    get_wifi_sta_state,
    telemetry_online,
)


_cell_location_cache = {}
_cell_location_inflight = {}
CELL_LOCATION_CACHE_SECONDS = 300
UNWIRED_CELL_LOCATION_CACHE_SECONDS = 3600
UNWIRED_LOCATION_API_URL = 'https://ap1.unwiredlabs.com/v2/process'


def setup_api_routes(app):
    app.router.add_post('/api/auth/login', api_login)
    app.router.add_get('/api/video/stream', api_video_stream_info)
    app.router.add_get('/api/video/snapshot', api_video_snapshot)
    app.router.add_get('/api/device/status', api_device_status)
    app.router.add_get('/api/sensor/latest', api_sensor_latest)
    app.router.add_get('/api/telemetry/latest', api_telemetry_latest)
    app.router.add_get('/api/modem/cell-location', api_modem_cell_location)
    app.router.add_get('/api/location/latest', api_location_latest)
    app.router.add_get('/api/devices', api_devices)
    app.router.add_get('/api/health', api_health)
    app.router.add_get('/api/ready', api_ready)


async def api_login(request):
    try:
        body = await request.json()
    except Exception:
        return web.json_response({'code': 400, 'message': 'Invalid JSON'}, status=400)

    username = body.get('username', '')
    password = body.get('password', '')
    if username == 'demo' and password == 'CHANGE_ME_ADMIN_PASSWORD':
        return web.json_response({
            'code': 200,
            'data': {'token': 'CHANGE_ME_DEMO_TOKEN', 'username': username},
        })
    return web.json_response({'code': 401, 'message': 'Invalid username or password'}, status=401)


def _external_schemes(request):
    forwarded_host = request.headers.get('X-Forwarded-Host')
    host = forwarded_host.split(',')[0].strip() if forwarded_host else request.host
    forwarded_proto = request.headers.get('X-Forwarded-Proto', request.scheme)
    secure = forwarded_proto.split(',')[0].strip().lower() == 'https'
    return host, ('https' if secure else 'http'), ('wss' if secure else 'ws')


async def api_video_stream_info(request):
    host, http_scheme, ws_scheme = _external_schemes(request)
    device = device_registry.latest()
    width = device.get('width') if device else None
    height = device.get('height') if device else None
    return web.json_response({
        'code': 200,
        'data': {
            'protocol': 'mjpeg',
            'wsUrl': f'{ws_scheme}://{host}/ws/video',
            'mjpegUrl': f'{http_scheme}://{host}/live/mjpeg',
            'resolution': f'{width}x{height}' if width and height else None,
            'fps': device.get('fps', 0) if device else 0,
            'codec': 'jpeg',
            'online': bool(device and device['online']),
            'deviceId': device.get('device_id') if device else None,
            'kibPerSecond': device.get('kib_per_second', 0) if device else 0,
        },
    })


async def api_video_snapshot(request):
    frame_id, frame, metadata = get_latest_jpeg()
    headers = {
        'Access-Control-Allow-Origin': '*',
        'Cache-Control': 'no-store, no-cache, must-revalidate, max-age=0',
        'Pragma': 'no-cache',
    }
    if frame is None:
        return web.json_response(
            {'code': 404, 'message': 'No video frame available'},
            status=404,
            headers=headers,
        )
    headers['X-Frame-Id'] = str(frame_id)
    if metadata.get('frame_seq') is not None:
        headers['X-Frame-Sequence'] = str(metadata['frame_seq'])
    return web.Response(body=frame, content_type='image/jpeg', headers=headers)


async def api_device_status(request):
    device = device_registry.latest()
    video_online = bool(device and device['online'])
    sensor = get_sensor_data()
    telemetry_is_online = telemetry_online()
    return web.json_response({
        'code': 200,
        'data': {
            'bw21': {'online': telemetry_is_online, 'ip': None, 'uptime': 0},
            'esp32': device,
            'l610': {
                'online': video_online,
                'ip': device.get('source_ip') if device else None,
                # RSSI is not part of ESJP v1; do not invent a value.
                'signal': None,
            },
            'telemetry': {
                'online': telemetry_is_online,
                'deviceId': sensor.get('device_id'),
                'bootId': sensor.get('boot_id'),
                'sequence': sensor.get('seq'),
                'ageMs': sensor.get('age_ms'),
            },
            'ap_stream': get_ap_stream_state(),
            'modem_4g': get_modem_4g_state(),
            'video_fps': get_video_fps_state(),
            'wifi_sta': get_wifi_sta_state(),
            'timestamp': time.time(),
        },
    })


async def api_sensor_latest(request):
    return web.json_response({'code': 200, 'data': get_sensor_data()})


async def api_telemetry_latest(request):
    return web.json_response({
        'code': 200,
        'data': {
            'online': telemetry_online(),
            'sensor': get_sensor_data(),
            'gps': get_gps_data(),
        },
    })


def _parse_cell_number(value):
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if not isinstance(value, str):
        raise ValueError('not a cell number')
    value = value.strip()
    if not value:
        raise ValueError('empty cell number')
    # L610 +GTCCINFO reports TAC and ECI as hexadecimal fields, including
    # values that happen to contain digits only. Integers are still accepted
    # for test/integration callers and retain their numeric value.
    if value.lower().startswith('0x'):
        value = value[2:]
    return int(value, 16)


class CellLocationError(Exception):
    def __init__(self, status, message, **details):
        super().__init__(message)
        self.status = status
        self.message = message
        self.details = details


def _cell_error_response(error):
    return web.json_response({
        'code': error.status,
        'message': error.message,
        **error.details,
    }, status=error.status)


def _validated_cell_entries(state, limit):
    cells = state.get('cells') if isinstance(state, dict) else None
    if not isinstance(cells, list):
        cells = []
    ordered = sorted(
        (cell for cell in cells if isinstance(cell, dict)),
        key=lambda item: not bool(item.get('serving')),
    )
    serving = next((cell for cell in ordered if cell.get('serving')), None)
    if serving is None:
        raise CellLocationError(409, 'serving_cell_unavailable')

    entries = []
    serving_identity = None
    seen = set()
    for cell in ordered:
        try:
            mcc = cell.get('mcc')
            mnc = cell.get('mnc')
            if (not isinstance(mcc, int) or isinstance(mcc, bool)
                    or not 0 <= mcc <= 999
                    or not isinstance(mnc, int) or isinstance(mnc, bool)
                    or not 0 <= mnc <= 999):
                raise ValueError('invalid mobile network identity')
            cell_id = _parse_cell_number(cell.get('cell_id'))
            tac = _parse_cell_number(cell.get('tac'))
            if not 0 <= cell_id <= 0x0FFFFFFF or not 0 <= tac <= 0xFFFF:
                raise ValueError('cell identity out of range')
        except (TypeError, ValueError):
            if cell is serving:
                raise CellLocationError(422, 'invalid_serving_cell_identity')
            # One malformed neighbor must not prevent use of the serving cell.
            continue

        identity = (mcc, mnc, tac, cell_id)
        if identity in seen:
            continue
        seen.add(identity)
        entries.append((cell, identity))
        if cell is serving:
            serving_identity = identity
        if len(entries) >= limit:
            break

    if serving_identity is None or not entries:
        raise CellLocationError(422, 'invalid_serving_cell_identity')
    return serving, entries


def _build_cell_geolocation_request(state):
    """Build the existing Google Geolocation API request."""
    serving, entries = _validated_cell_entries(state, 8)
    identities = tuple(identity for _, identity in entries)
    serving_identity = identities[0]
    towers = [{
        'cellId': identity[3],
        'locationAreaCode': identity[2],
        'mobileCountryCode': identity[0],
        'mobileNetworkCode': identity[1],
        'age': 0,
    } for _, identity in entries]
    # GTCCINFO's reported level fields are not documented here as dBm.
    # Sending an invented signalStrength can make provider results worse.
    payload = {
        'homeMobileCountryCode': serving_identity[0],
        'homeMobileNetworkCode': serving_identity[1],
        'radioType': 'lte',
        'considerIp': False,
        'cellTowers': towers,
    }
    return serving, payload, identities


def _build_unwired_geolocation_request(state, token):
    """Build a LocationAPI v2 request without guessing modem signal units."""
    serving, entries = _validated_cell_entries(state, 7)
    identities = tuple(identity for _, identity in entries)
    serving_identity = identities[0]
    cells = []
    for source, identity in entries:
        cell = {
            'radio': 'lte',
            'mcc': identity[0],
            'mnc': identity[1],
            'lac': identity[2],
            'cid': identity[3],
        }
        pci = source.get('pci')
        if (isinstance(pci, int) and not isinstance(pci, bool)
                and 0 <= pci <= 503):
            cell['psc'] = pci
        # Do not send GTCCINFO's raw level as LocationAPI `signal`: its dBm
        # conversion has not been confirmed from the L610 protocol manual.
        cells.append(cell)

    payload = {
        'token': token,
        'radio': 'lte',
        'mcc': serving_identity[0],
        'mnc': serving_identity[1],
        'cells': cells,
        'address': 0,
    }
    return serving, payload, identities


def _normalized_cell_metadata(serving):
    return {
        'mcc': serving.get('mcc'),
        'mnc': serving.get('mnc'),
        'tac': serving.get('tac'),
        'cell_id': serving.get('cell_id'),
        'earfcn': serving.get('earfcn'),
        'pci': serving.get('pci'),
        'band': serving.get('band'),
    }


def _valid_location_coordinate(value, minimum, maximum):
    return (
        isinstance(value, (int, float)) and not isinstance(value, bool)
        and math.isfinite(value) and minimum <= value <= maximum
    )


def _parse_unwired_geolocation_response(result, serving, identities):
    if not isinstance(result, dict):
        raise CellLocationError(502, 'cell_geolocation_invalid_response')
    if result.get('status') != 'ok':
        message = str(result.get('message', '')).strip().lower()
        if message == 'invalid token' or message == 'inactive device':
            raise CellLocationError(503, 'cell_geolocation_auth_failed')
        if message == 'no matches found':
            raise CellLocationError(404, 'cell_geolocation_no_match')
        if (message in ('no slots available', 'quota exhausted')
                or message.startswith('token balance over;')):
            raise CellLocationError(429, 'cell_geolocation_quota_exhausted')
        raise CellLocationError(502, 'cell_geolocation_lookup_failed')

    lat = result.get('lat')
    lng = result.get('lon')
    if (not _valid_location_coordinate(lat, -90, 90)
            or not _valid_location_coordinate(lng, -180, 180)):
        raise CellLocationError(502, 'cell_geolocation_invalid_response')
    accuracy = result.get('accuracy')
    if (not isinstance(accuracy, (int, float)) or isinstance(accuracy, bool)
            or not math.isfinite(accuracy) or accuracy < 0):
        accuracy = None

    fallback = result.get('fallback')
    if not isinstance(fallback, str) or not fallback:
        fallback = None
    aged = result.get('aged')
    if not isinstance(aged, (bool, int)):
        aged = None
    return {
        'lat': lat,
        'lng': lng,
        'accuracy': accuracy,
        'provider': 'unwired',
        'cells_used': len(identities),
        'fallback': fallback,
        'aged': aged,
        'cell': _normalized_cell_metadata(serving),
    }


def _cell_location_cache_ttl(provider):
    default = (UNWIRED_CELL_LOCATION_CACHE_SECONDS
               if provider == 'unwired' else CELL_LOCATION_CACHE_SECONDS)
    value = os.getenv('CELL_LOCATION_CACHE_SECONDS', '').strip()
    if not value:
        return default
    try:
        seconds = int(value)
    except ValueError:
        return default
    return seconds if 30 <= seconds <= 86400 else default


async def _fetch_cell_location(provider, credential, payload, serving,
                               identities):
    timeout = aiohttp.ClientTimeout(total=10)
    try:
        async with aiohttp.ClientSession(timeout=timeout) as session:
            if provider == 'google':
                request = session.post(
                    'https://www.googleapis.com/geolocation/v1/geolocate',
                    params={'key': credential},
                    json=payload,
                )
            else:
                request = session.post(UNWIRED_LOCATION_API_URL, json=payload)
            async with request as response:
                result = await response.json(content_type=None)
                # LocationAPI may return a useful status:error JSON body with
                # a non-200 status. Parse that body before using the generic
                # HTTP fallback so quota/auth/no-match remain distinguishable.
                if provider == 'unwired':
                    if (isinstance(result, dict)
                            and result.get('status') == 'error'):
                        return _parse_unwired_geolocation_response(
                            result, serving, identities
                        )
                    if response.status != 200:
                        raise CellLocationError(
                            502, 'cell_geolocation_lookup_failed',
                            provider_status=response.status,
                        )
                elif response.status != 200:
                    raise CellLocationError(
                        502, 'cell_geolocation_lookup_failed',
                        provider_status=response.status,
                    )
    except CellLocationError:
        raise
    except (aiohttp.ClientError, asyncio.TimeoutError):
        raise CellLocationError(502, 'cell_geolocation_unreachable')
    except (TypeError, ValueError):
        raise CellLocationError(502, 'cell_geolocation_invalid_response')

    if provider == 'unwired':
        return _parse_unwired_geolocation_response(
            result, serving, identities
        )

    location = result.get('location') if isinstance(result, dict) else None
    lat = location.get('lat') if isinstance(location, dict) else None
    lng = location.get('lng') if isinstance(location, dict) else None
    if (not _valid_location_coordinate(lat, -90, 90)
            or not _valid_location_coordinate(lng, -180, 180)):
        raise CellLocationError(502, 'cell_geolocation_invalid_response')
    accuracy = result.get('accuracy') if isinstance(result, dict) else None
    if (not isinstance(accuracy, (int, float)) or isinstance(accuracy, bool)
            or not math.isfinite(accuracy) or accuracy < 0):
        accuracy = None
    return {
        'lat': lat,
        'lng': lng,
        'accuracy': accuracy,
        'provider': 'google',
        'cells_used': len(identities),
        'cell': _normalized_cell_metadata(serving),
    }


async def _fetch_and_cache_cell_location(cache_key, provider, credential,
                                         payload, serving, identities):
    data = await _fetch_cell_location(
        provider, credential, payload, serving, identities
    )
    # Keep a small keyed cache instead of clearing every entry: during a cell
    # handover an older in-flight lookup must not erase the newer cell result.
    if cache_key not in _cell_location_cache and len(_cell_location_cache) >= 16:
        oldest_key = min(
            _cell_location_cache,
            key=lambda key: _cell_location_cache[key]['cached_at'],
        )
        _cell_location_cache.pop(oldest_key, None)
    _cell_location_cache[cache_key] = {
        'cached_at': time.monotonic(),
        'data': data,
    }
    return data


def _clear_cell_location_inflight(cache_key, task):
    if _cell_location_inflight.get(cache_key) is task:
        _cell_location_inflight.pop(cache_key, None)


async def _resolve_cell_location():
    state = get_modem_4g_state()
    if not state.get('online'):
        raise CellLocationError(409, 'device_offline')

    provider = os.getenv('CELL_GEOLOCATION_PROVIDER', 'google').strip().lower()
    if provider == 'google':
        credential = (
            os.getenv('GOOGLE_GEOLOCATION_API_KEY', '').strip()
            or os.getenv('CELL_GEOLOCATION_API_KEY', '').strip()
        )
        if not credential:
            raise CellLocationError(
                503,
                'cell_geolocation_not_configured',
                required=[
                    'CELL_GEOLOCATION_PROVIDER=google',
                    'GOOGLE_GEOLOCATION_API_KEY or CELL_GEOLOCATION_API_KEY',
                ],
            )
        serving, payload, identities = _build_cell_geolocation_request(state)
    elif provider == 'unwired':
        credential = os.getenv('UNWIRED_LOCATION_API_TOKEN', '').strip()
        if not credential:
            raise CellLocationError(
                503,
                'cell_geolocation_not_configured',
                required=[
                    'CELL_GEOLOCATION_PROVIDER=unwired',
                    'UNWIRED_LOCATION_API_TOKEN',
                ],
            )
        serving, payload, identities = _build_unwired_geolocation_request(
            state, credential
        )
    else:
        raise CellLocationError(
            503,
            'cell_geolocation_not_configured',
            required=['CELL_GEOLOCATION_PROVIDER=google or unwired'],
        )

    # Neighbor order can change between scans. Canonicalize it so reordering
    # alone does not create a new paid provider request during the cache TTL.
    cache_key = (provider, identities[0], tuple(sorted(identities[1:])))
    cached = _cell_location_cache.get(cache_key)
    if (cached and time.monotonic() - cached['cached_at']
            <= _cell_location_cache_ttl(provider)):
        return cached['data']

    task = _cell_location_inflight.get(cache_key)
    if task is None:
        task = asyncio.create_task(_fetch_and_cache_cell_location(
            cache_key, provider, credential, payload, serving, identities
        ))
        _cell_location_inflight[cache_key] = task
        task.add_done_callback(
            lambda done, key=cache_key: _clear_cell_location_inflight(key, done)
        )
    # One disconnected browser request must not cancel the provider request
    # shared by other tabs that are waiting for the same cell identity.
    return await asyncio.shield(task)


async def api_modem_cell_location(request):
    """Resolve the current LTE serving cell without exposing the provider key."""
    try:
        data = await _resolve_cell_location()
    except CellLocationError as error:
        return _cell_error_response(error)
    return web.json_response({'code': 200, 'data': data})


def _valid_gps_coordinate(gps):
    lat = gps.get('lat') if isinstance(gps, dict) else None
    lng = gps.get('lng') if isinstance(gps, dict) else None
    return (
        gps.get('online') is True
        and isinstance(lat, (int, float)) and not isinstance(lat, bool)
        and math.isfinite(lat) and -90 <= lat <= 90
        and isinstance(lng, (int, float)) and not isinstance(lng, bool)
        and math.isfinite(lng) and -180 <= lng <= 180
    )


def _haversine_distance_m(lat1, lng1, lat2, lng2):
    radius_m = 6371008.8
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lng2 - lng1)
    a = (math.sin(dphi / 2) ** 2
         + math.cos(phi1) * math.cos(phi2) * math.sin(dlambda / 2) ** 2)
    return radius_m * 2 * math.atan2(math.sqrt(a), math.sqrt(max(0.0, 1 - a)))


def _compose_location_result(gps, cell_enabled, cell=None, cell_error=None):
    gps_available = _valid_gps_coordinate(gps)
    gps_data = {
        'available': gps_available,
        'lat': gps.get('lat') if gps_available else None,
        'lng': gps.get('lng') if gps_available else None,
        'satellites': gps.get('satellites') if gps_available else None,
        'speed': gps.get('speed') if gps_available else None,
        'heading': gps.get('heading') if gps_available else None,
        'received_at': gps.get('received_at') if gps_available else None,
        'age_ms': gps.get('age_ms') if gps_available else None,
    }
    selected = None
    source = 'none'
    validation = None
    if gps_available:
        source = 'gps'
        selected = {
            'lat': gps_data['lat'],
            'lng': gps_data['lng'],
            'source': 'gps',
            'accuracy': None,
            'heading': gps_data['heading'],
            'speed': gps_data['speed'],
            'satellites': gps_data['satellites'],
        }
        if cell is not None:
            distance = _haversine_distance_m(
                gps_data['lat'], gps_data['lng'], cell['lat'], cell['lng']
            )
            accuracy = cell.get('accuracy')
            validation = {
                'gps_primary': True,
                'distance_m': round(distance, 1),
                'cell_accuracy_m': accuracy,
                'within_cell_accuracy': (
                    distance <= accuracy if isinstance(accuracy, (int, float)) else None
                ),
            }
    elif cell is not None:
        source = 'cell'
        selected = {
            'lat': cell['lat'],
            'lng': cell['lng'],
            'source': 'cell',
            'accuracy': cell.get('accuracy'),
            'heading': None,
            'speed': None,
            'satellites': None,
        }
    return {
        'online': gps.get('online') is True,
        'cell_enabled': cell_enabled,
        'source': source,
        'selected': selected,
        'gps': gps_data,
        'cell': cell,
        'validation': validation,
        'cell_error': cell_error,
    }


async def api_location_latest(request):
    enabled_value = request.query.get('cell_enabled', '0').strip().lower()
    if enabled_value in ('1', 'true'):
        cell_enabled = True
    elif enabled_value in ('0', 'false'):
        cell_enabled = False
    else:
        return web.json_response(
            {'code': 400, 'message': 'invalid_cell_enabled'}, status=400
        )

    gps = get_gps_data()
    cell = None
    cell_error = None
    if cell_enabled:
        try:
            cell = await _resolve_cell_location()
        except CellLocationError as error:
            # GPS can remain fully usable when a third-party cell lookup is
            # unavailable, so surface the stable error code without failing
            # the entire positioning endpoint.
            cell_error = error.message
    return web.json_response({
        'code': 200,
        'data': _compose_location_result(gps, cell_enabled, cell, cell_error),
    })


async def api_devices(request):
    return web.json_response({'code': 200, 'data': device_registry.snapshot()})


async def api_health(request):
    runtime = request.app.get('runtime', {})
    started = runtime.get('started_monotonic', time.monotonic())
    devices = device_registry.snapshot()
    return web.json_response({
        'status': 'ok',
        'uptimeSeconds': round(time.monotonic() - started, 3),
        'devicesOnline': sum(1 for item in devices if item['online']),
    })


async def api_ready(request):
    ready = bool(request.app.get('runtime', {}).get('listeners_ready', False))
    return web.json_response(
        {'status': 'ready' if ready else 'starting'},
        status=200 if ready else 503,
    )
