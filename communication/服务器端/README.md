# ESP32-S3 / BW21 video gateway

## Ports and compatibility

- `8765/tcp`: HTTP, MJPEG and the existing WebSocket endpoints.
- `9091/udp`: ESP32-S3 ESJP v1/v2 JPEG chunks and legacy BW21 UDP video.
- `9092/tcp`: legacy BW21/L610 Base64 H.264 chunks.
- `9093/udp`: ESUT/1 unified sensor+GPS JSON, legacy flat JSON, and control return path.

Legacy support is enabled by default. Set `LEGACY_BW21_ENABLED=false` to
disable port 9092 and FFmpeg while keeping ESP32 ESJP video on UDP 9091 and
ESUT/1 telemetry on UDP 9093.

## Start

```bash
python3 -m pip install -r requirements.txt
python3 server.py
```

FFmpeg is only required for the legacy H.264-to-MJPEG path. OV5640 JPEG from
the ESP32 is validated and published directly without transcoding.

## Runtime endpoints

- `/live/mjpeg`: latest verified JPEG as an MJPEG stream.
- `/ws/video`: existing binary video WebSocket.
- `/api/health`: process uptime and online-device count.
- `/api/ready`: listener readiness (`200`) or startup state (`503`).
- `/api/devices`: current ESP32 device registry and measured video rates.
- `/api/video/stream`: client-reachable stream URLs and live video metadata.
- `/api/video/snapshot`: latest verified JPEG (`no-store`, cross-origin readable).
- `/api/device/status`: legacy plus ESP32/L610 status.
- `/api/telemetry/latest`: one freshness snapshot containing sensor and GPS data.
- `/api/modem/cell-location`: current LTE-cell lookup result from the configured provider.
- `/api/location/latest?cell_enabled=0|1`: GPS-primary map position, with optional
  LTE-cell fallback and consistency information.

Cell lookup is disabled unless the server process has one supported provider
configured. Unwired LocationAPI (Asia-Pacific endpoint) uses:

```bash
export CELL_GEOLOCATION_PROVIDER=unwired
export UNWIRED_LOCATION_API_TOKEN='your-server-side-token'
```

Google compatibility remains available:

```bash
export CELL_GEOLOCATION_PROVIDER=google
export GOOGLE_GEOLOCATION_API_KEY='your-server-side-key'
```

`CELL_GEOLOCATION_API_KEY` is accepted as a Google-key alias. Provider secrets
must stay in the server process environment and must not be placed in the web
frontend, source archive, URL, or logs. Unwired requests use at most seven LTE
cells with the serving cell first. The raw L610 GTCCINFO level is not sent as
signal strength because it is not dBm. Google results are cached for five
minutes; Unwired results are cached for one hour by default to protect a small
developer quota. A changed serving-cell identity bypasses the old cache; a
volatile neighbor-cell scan does not. `CELL_LOCATION_CACHE_SECONDS` can override either default
with a value from 30 to 86400 seconds.

`/ws/control` accepts the existing movement commands plus an acknowledged AP
stream command:

```json
{"type":"command_request","requestId":"req-1","cmd":"ap_stream","params":{"enabled":true}}
```

The immediate `command_ack` only reports `queued`. Actual ESP state is sent as
a `device_state` message whose `ap_stream` object contains `supported`,
`state`, `request_id`, `error`, and `received_at`. The same state is available
as `data.ap_stream` from `/api/device/status`.

Video settings reuse the compatible `video_fps` command. Existing FPS-only
clients remain valid; new clients may also select one of the fixed resolutions:

```json
{"type":"command_request","requestId":"video-1","cmd":"video_fps","params":{"fps":8,"resolution":"640x480"}}
```

Allowed FPS values are `5`, `8`, `15`, `20`, `30`; allowed resolutions are
`640x480`, `1280x720`, `1920x1080`. `device_state.video_fps` reports `fps`,
`resolution`, `width`, `height`, `request_id`, and `error`. The JPEG receiver
accepts bounded frames up to 512 KiB so FHD frames are not rejected by the old
VGA-oriented 256 KiB ceiling.

A device is online while a validated ESJP frame has been received during the
last 15 seconds. ESJP frames require matching length, version-specific chunk
count (1200-byte v1 or 7200-byte v2 payloads), CRC32 and JPEG SOI/EOI markers.
Incomplete frames are bounded by the reassembly timeout and in-flight limit.

Telemetry is independently online only while a complete, schema-valid packet
has arrived during the last 5 seconds. Duplicate/out-of-order ESUT sequences do
not refresh the link. On timeout, sensor and GPS WebSockets immediately publish
`online:false` with all values cleared to JSON `null`.

## Test

```bash
python3 -m unittest discover -v
python3 -m compileall -q .
```
