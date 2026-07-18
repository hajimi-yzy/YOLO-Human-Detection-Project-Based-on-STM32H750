import asyncio
import json
import os
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
        original_wifi_sta = api_handler.get_wifi_sta_state
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
        api_handler.get_wifi_sta_state = lambda: {
            "online": True, "supported": True, "state": "success",
            "action": "query", "request_id": "wifi-3", "error": None,
            "feature_enabled": True, "scanning": False, "connected": True,
            "ssid": "RescueNet", "ip": "198.51.100.23", "rssi": -51,
            "wifi_uplink_selected": False, "active_uplink": "l610",
            "networks": [], "received_at": 123.5,
        }
        try:
            response = await api_handler.api_device_status(None)
        finally:
            api_handler.device_registry.latest = original_latest
            api_handler.get_sensor_data = original_sensor
            api_handler.telemetry_online = original_online
            api_handler.get_ap_stream_state = original_ap_stream
            api_handler.get_wifi_sta_state = original_wifi_sta

        body = json.loads(response.text)
        self.assertNotIn("apStream", body["data"])
        self.assertEqual(body["data"]["ap_stream"]["state"], "enabled")
        self.assertEqual(body["data"]["ap_stream"]["request_id"], "req-3")
        self.assertEqual(body["data"]["wifi_sta"]["ssid"], "RescueNet")
        self.assertIs(body["data"]["wifi_sta"]["scanning"], False)
        self.assertNotIn("password", body["data"]["wifi_sta"])


class CellLocationApiTest(unittest.IsolatedAsyncioTestCase):
    @staticmethod
    def state():
        return {
            "online": True,
            "cells": [
                {
                    "serving": False, "mcc": 999, "mnc": 99,
                    "tac": "00AA", "cell_id": "0011223",
                    "earfcn": 12345, "pci": 101, "band": 3,
                },
                {
                    "serving": True, "mcc": 999, "mnc": 99,
                    "tac": "00AA", "cell_id": "0011224",
                    "earfcn": 22345, "pci": 102, "band": 7,
                },
            ],
        }

    def test_provider_request_uses_serving_then_valid_neighbors(self):
        serving, payload, identities = api_handler._build_cell_geolocation_request(
            self.state()
        )
        self.assertTrue(serving["serving"])
        self.assertEqual(payload["radioType"], "lte")
        self.assertIs(payload["considerIp"], False)
        self.assertEqual(len(payload["cellTowers"]), 2)
        self.assertEqual(payload["cellTowers"][0]["cellId"], int("0011224", 16))
        self.assertEqual(payload["cellTowers"][1]["cellId"], int("0011223", 16))
        self.assertEqual(payload["cellTowers"][0]["locationAreaCode"], int("00AA", 16))
        self.assertNotIn("signalStrength", payload["cellTowers"][0])
        self.assertEqual(len(identities), 2)

    def test_invalid_neighbor_is_ignored_but_invalid_serving_is_rejected(self):
        state = self.state()
        state["cells"][0]["cell_id"] = "not-hex"
        _, payload, _ = api_handler._build_cell_geolocation_request(state)
        self.assertEqual(len(payload["cellTowers"]), 1)
        state["cells"][1]["cell_id"] = "not-hex"
        with self.assertRaises(api_handler.CellLocationError) as raised:
            api_handler._build_cell_geolocation_request(state)
        self.assertEqual(raised.exception.message, "invalid_serving_cell_identity")

    def test_unwired_request_uses_serving_first_and_at_most_seven_valid_cells(self):
        token = "unit-test-token-must-not-leak"
        state = self.state()
        state["cells"][0]["rsrp"] = 63
        # Even a negative raw value must not be treated as confirmed dBm.
        state["cells"][1]["rsrp"] = -80
        state["cells"].insert(0, {
            "serving": False, "mcc": 999, "mnc": 99,
            "tac": "00AA", "cell_id": "not-hex",
            "earfcn": 12346, "pci": 103, "band": 3, "rsrp": 63,
        })
        for index in range(8):
            state["cells"].append({
                "serving": False, "mcc": 999, "mnc": 99,
                "tac": "00AA", "cell_id": f"0012{index + 5:03X}",
                "earfcn": 12347 + index, "pci": 110 + index,
                "band": 3, "rsrp": 63,
            })

        serving, payload, identities = (
            api_handler._build_unwired_geolocation_request(state, token)
        )

        self.assertTrue(serving["serving"])
        self.assertEqual(payload["token"], token)
        self.assertEqual(payload["radio"], "lte")
        self.assertEqual(payload["mcc"], 999)
        self.assertEqual(payload["mnc"], 99)
        self.assertEqual(payload["address"], 0)
        self.assertEqual(len(payload["cells"]), 7)
        self.assertEqual(len(identities), 7)

        first = payload["cells"][0]
        self.assertEqual(first["radio"], "lte")
        self.assertEqual(first["mcc"], 999)
        self.assertEqual(first["mnc"], 99)
        self.assertEqual(first["lac"], int("00AA", 16))
        self.assertEqual(first["cid"], int("0011224", 16))
        self.assertEqual(first["psc"], 102)
        self.assertNotIn("signal", first)
        self.assertNotIn("rsrp", first)
        self.assertNotIn(token, repr(serving))
        self.assertNotIn(token, repr(identities))
        for cell in payload["cells"]:
            self.assertEqual(
                set(cell), {"radio", "mcc", "mnc", "lac", "cid", "psc"}
            )

    def test_unwired_success_response_preserves_accuracy_and_lac_fallback(self):
        serving = next(cell for cell in self.state()["cells"] if cell["serving"])
        identities = (
            (999, 99, int("00AA", 16), int("0011224", 16)),
            (999, 99, int("00AA", 16), int("0011223", 16)),
        )
        result = {
            "status": "ok",
            "balance": 5,
            "lat": 10.0,
            "lon": 20.0,
            "accuracy": 500,
            "fallback": "lacf",
            "aged": 1,
        }

        data = api_handler._parse_unwired_geolocation_response(
            result, serving, identities
        )

        self.assertEqual(data["lat"], 10.0)
        self.assertEqual(data["lng"], 20.0)
        self.assertEqual(data["accuracy"], 500)
        self.assertEqual(data["fallback"], "lacf")
        self.assertEqual(data["aged"], 1)
        self.assertEqual(data["provider"], "unwired")
        self.assertEqual(data["cells_used"], 2)
        self.assertNotIn("token", json.dumps(data).lower())

    def test_unwired_error_response_raises_without_leaking_token(self):
        token = "unit-test-token-must-not-leak"
        serving = next(cell for cell in self.state()["cells"] if cell["serving"])
        identities = (
            (999, 99, int("00AA", 16), int("0011224", 16)),
        )
        result = {
            "status": "error",
            "message": f"Invalid token: {token}",
            "help": f"Do not expose {token}",
            "balance": 5,
        }

        with self.assertRaises(api_handler.CellLocationError) as raised:
            api_handler._parse_unwired_geolocation_response(
                result, serving, identities
            )

        serialized_error = json.dumps({
            "message": raised.exception.message,
            "details": raised.exception.details,
        })
        self.assertNotIn(token, serialized_error)
        self.assertEqual(
            raised.exception.message, "cell_geolocation_lookup_failed"
        )

    def test_unwired_exhausted_balance_is_reported_as_quota_error(self):
        serving = next(cell for cell in self.state()["cells"] if cell["serving"])
        identities = ((999, 99, int("00AA", 16), int("0011224", 16)),)
        result = {
            "status": "error",
            "message": (
                "Token balance over; you have used up all your requests "
                "for today"
            ),
        }
        with self.assertRaises(api_handler.CellLocationError) as raised:
            api_handler._parse_unwired_geolocation_response(
                result, serving, identities
            )
        self.assertEqual(raised.exception.status, 429)
        self.assertEqual(
            raised.exception.message, "cell_geolocation_quota_exhausted"
        )

    async def test_concurrent_unwired_requests_share_one_paid_lookup(self):
        original_state = api_handler.get_modem_4g_state
        original_fetch = api_handler._fetch_and_cache_cell_location
        old_provider = os.environ.get("CELL_GEOLOCATION_PROVIDER")
        old_token = os.environ.get("UNWIRED_LOCATION_API_TOKEN")
        calls = 0

        async def fake_fetch(*args):
            nonlocal calls
            calls += 1
            await asyncio.sleep(0.01)
            return {"provider": "unwired", "lat": 1.0, "lng": 2.0}

        api_handler.get_modem_4g_state = self.state
        api_handler._fetch_and_cache_cell_location = fake_fetch
        api_handler._cell_location_cache.clear()
        api_handler._cell_location_inflight.clear()
        os.environ["CELL_GEOLOCATION_PROVIDER"] = "unwired"
        os.environ["UNWIRED_LOCATION_API_TOKEN"] = "unit-test-token"
        try:
            first, second = await asyncio.gather(
                api_handler._resolve_cell_location(),
                api_handler._resolve_cell_location(),
            )
        finally:
            api_handler.get_modem_4g_state = original_state
            api_handler._fetch_and_cache_cell_location = original_fetch
            api_handler._cell_location_cache.clear()
            api_handler._cell_location_inflight.clear()
            if old_provider is None:
                os.environ.pop("CELL_GEOLOCATION_PROVIDER", None)
            else:
                os.environ["CELL_GEOLOCATION_PROVIDER"] = old_provider
            if old_token is None:
                os.environ.pop("UNWIRED_LOCATION_API_TOKEN", None)
            else:
                os.environ["UNWIRED_LOCATION_API_TOKEN"] = old_token

        self.assertEqual(calls, 1)
        self.assertEqual(first, second)

    async def test_old_cell_lookup_cannot_erase_new_cell_cache(self):
        original_fetch = api_handler._fetch_cell_location
        api_handler._cell_location_cache.clear()

        async def fake_fetch(provider, credential, payload, serving, identities):
            await asyncio.sleep(payload["delay"])
            return {"lat": payload["lat"], "lng": 2.0}

        api_handler._fetch_cell_location = fake_fetch
        key_old = ("unwired", (999, 99, 1, 1), ())
        key_new = ("unwired", (999, 99, 2, 2), ())
        try:
            await asyncio.gather(
                api_handler._fetch_and_cache_cell_location(
                    key_old, "unwired", "token",
                    {"delay": 0.02, "lat": 1.0}, {}, (),
                ),
                api_handler._fetch_and_cache_cell_location(
                    key_new, "unwired", "token",
                    {"delay": 0.0, "lat": 2.0}, {}, (),
                ),
            )
            self.assertIn(key_old, api_handler._cell_location_cache)
            self.assertIn(key_new, api_handler._cell_location_cache)
        finally:
            api_handler._fetch_cell_location = original_fetch
            api_handler._cell_location_cache.clear()

    def test_gps_is_primary_and_cell_is_validation_only(self):
        gps = {
            "online": True, "lat": 10.0, "lng": 20.0,
            "satellites": 12, "speed": 1.5, "heading": 90,
            "received_at": 123.0, "age_ms": 100,
        }
        cell = {
            "lat": 10.001, "lng": 20.001, "accuracy": 1500,
            "provider": "google", "cells_used": 2,
        }
        data = api_handler._compose_location_result(gps, True, cell)
        self.assertEqual(data["source"], "gps")
        self.assertEqual(data["selected"]["lat"], gps["lat"])
        self.assertEqual(data["selected"]["source"], "gps")
        self.assertTrue(data["validation"]["gps_primary"])
        self.assertIsInstance(data["validation"]["distance_m"], float)

    def test_cell_is_used_only_when_gps_coordinates_are_missing(self):
        gps = {"online": True, "lat": None, "lng": None}
        cell = {
            "lat": 10.001, "lng": 20.001, "accuracy": 1500,
            "provider": "google", "cells_used": 1,
        }
        data = api_handler._compose_location_result(gps, True, cell)
        self.assertEqual(data["source"], "cell")
        self.assertEqual(data["selected"]["source"], "cell")
        self.assertIsNone(data["validation"])

    async def test_disabled_cell_location_never_calls_provider(self):
        class Request:
            query = {"cell_enabled": "0"}

        original_gps = api_handler.get_gps_data
        original_resolve = api_handler._resolve_cell_location
        api_handler.get_gps_data = lambda: {
            "online": True, "lat": 10.0, "lng": 20.0,
            "satellites": 10, "speed": 0, "heading": 0,
            "received_at": 123, "age_ms": 10,
        }

        async def forbidden():
            raise AssertionError("provider must not be called")

        api_handler._resolve_cell_location = forbidden
        try:
            response = await api_handler.api_location_latest(Request())
        finally:
            api_handler.get_gps_data = original_gps
            api_handler._resolve_cell_location = original_resolve
        body = json.loads(response.text)
        self.assertEqual(body["data"]["source"], "gps")
        self.assertIs(body["data"]["cell_enabled"], False)

    async def test_cell_failure_does_not_hide_valid_gps(self):
        class Request:
            query = {"cell_enabled": "1"}

        original_gps = api_handler.get_gps_data
        original_resolve = api_handler._resolve_cell_location
        api_handler.get_gps_data = lambda: {
            "online": True, "lat": 10.0, "lng": 20.0,
            "satellites": 10, "speed": 0, "heading": 0,
            "received_at": 123, "age_ms": 10,
        }

        async def failing():
            raise api_handler.CellLocationError(503, "cell_geolocation_not_configured")

        api_handler._resolve_cell_location = failing
        try:
            response = await api_handler.api_location_latest(Request())
        finally:
            api_handler.get_gps_data = original_gps
            api_handler._resolve_cell_location = original_resolve
        body = json.loads(response.text)
        self.assertEqual(response.status, 200)
        self.assertEqual(body["data"]["source"], "gps")
        self.assertEqual(
            body["data"]["cell_error"], "cell_geolocation_not_configured"
        )


if __name__ == "__main__":
    unittest.main()
