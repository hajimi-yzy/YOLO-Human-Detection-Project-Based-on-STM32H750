import json
import unittest

import api_handler


class VideoSnapshotApiTest(unittest.IsolatedAsyncioTestCase):
    async def test_snapshot_returns_latest_jpeg_without_cache(self):
        jpeg = b"\xff\xd8payload\xff\xd9"
        original = api_handler.get_latest_jpeg
        api_handler.get_latest_jpeg = lambda: (
            9, jpeg, {"frame_seq": 42}
        )
        try:
            response = await api_handler.api_video_snapshot(None)
        finally:
            api_handler.get_latest_jpeg = original

        self.assertEqual(response.status, 200)
        self.assertEqual(response.body, jpeg)
        self.assertEqual(response.content_type, "image/jpeg")
        self.assertEqual(response.headers["Access-Control-Allow-Origin"], "*")
        self.assertIn("no-store", response.headers["Cache-Control"])
        self.assertEqual(response.headers["X-Frame-Sequence"], "42")

    async def test_snapshot_returns_404_before_first_frame(self):
        original = api_handler.get_latest_jpeg
        api_handler.get_latest_jpeg = lambda: (0, None, {})
        try:
            response = await api_handler.api_video_snapshot(None)
        finally:
            api_handler.get_latest_jpeg = original
        self.assertEqual(response.status, 404)
        self.assertIn("no-store", response.headers["Cache-Control"])

    async def test_device_status_exposes_snake_case_ap_stream_state(self):
        original_latest = api_handler.device_registry.latest
        original_sensor = api_handler.get_sensor_data
        original_online = api_handler.telemetry_online
        original_ap_stream = api_handler.get_ap_stream_state
        api_handler.device_registry.latest = lambda: None
        api_handler.get_sensor_data = lambda: {}
        api_handler.telemetry_online = lambda: True
        api_handler.get_ap_stream_state = lambda: {
            "online": True,
            "device_id": "esp32s3-01",
            "boot_id": "89ABCDEF",
            "supported": True,
            "state": "enabled",
            "request_id": "req-3",
            "error": None,
            "received_at": 123.5,
        }
        try:
            response = await api_handler.api_device_status(None)
        finally:
            api_handler.device_registry.latest = original_latest
            api_handler.get_sensor_data = original_sensor
            api_handler.telemetry_online = original_online
            api_handler.get_ap_stream_state = original_ap_stream

        body = json.loads(response.text)
        self.assertNotIn("apStream", body["data"])
        self.assertEqual(body["data"]["ap_stream"]["state"], "enabled")
        self.assertEqual(body["data"]["ap_stream"]["request_id"], "req-3")


if __name__ == "__main__":
    unittest.main()
