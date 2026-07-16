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

`/ws/control` accepts the existing movement commands plus an acknowledged AP
stream command:

```json
{"type":"command_request","requestId":"req-1","cmd":"ap_stream","params":{"enabled":true}}
```

The immediate `command_ack` only reports `queued`. Actual ESP state is sent as
a `device_state` message whose `ap_stream` object contains `supported`,
`state`, `request_id`, `error`, and `received_at`. The same state is available
as `data.ap_stream` from `/api/device/status`.

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
