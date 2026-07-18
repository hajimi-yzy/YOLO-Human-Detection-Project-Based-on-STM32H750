import unittest

import udp_sensor_receiver as receiver


class UnifiedTelemetryValidationTest(unittest.TestCase):
    def setUp(self):
        receiver._last_sequence.clear()

    @staticmethod
    def packet(seq=1):
        return {
            "protocol": "ESUT/1",
            "device_id": "esp32s3-01",
            "boot_id": "89ABCDEF",
            "seq": seq,
            "timestamp": 1234,
            "sensor": {
                "temperature": 25.1,
                "humidity": 50.2,
                "altitude": 12.3,
                "pressure": 1008.2,
                "gas": {"concentration": 0.1, "alarm": False},
                "person_detected": 0,
            },
            "gps": {
                "lat": 10.0,
                "lng": 20.0,
                "satellites": 10,
                "speed": 0,
                "heading": 0,
            },
        }

    def test_missing_group_is_accepted_as_na(self):
        packet = self.packet()
        del packet["gps"]
        normalized = receiver._normalize_packet(packet)
        self.assertIsNotNone(normalized)
        self.assertEqual(normalized["gps"], {})

    def test_heartbeat_is_accepted_without_stm32_data(self):
        heartbeat = {
            "protocol": "ESUT/1",
            "kind": "heartbeat",
            "device_id": "esp32s3-01",
            "boot_id": "89ABCDEF",
            "timestamp": 1234,
        }
        normalized = receiver._normalize_packet(heartbeat)
        self.assertTrue(normalized["heartbeat"])
        self.assertTrue(receiver._is_new_sequence(normalized))

    def test_heartbeat_accepts_ap_stream_state(self):
        heartbeat = {
            "protocol": "ESUT/1",
            "kind": "heartbeat",
            "device_id": "esp32s3-01",
            "boot_id": "89ABCDEF",
            "timestamp": 1234,
            "ap_stream": {
                "supported": True,
                "state": "enabled",
                "request_id": "request-7",
                "error": None,
            },
        }
        normalized = receiver._normalize_packet(heartbeat)
        self.assertEqual(normalized["ap_stream"]["state"], "enabled")
        self.assertEqual(normalized["ap_stream"]["request_id"], "request-7")

    def test_heartbeat_accepts_modem_cells_and_video_fps(self):
        heartbeat = {
            "protocol": "ESUT/1", "kind": "heartbeat",
            "device_id": "esp32s3-01", "boot_id": "89ABCDEF",
            "timestamp": 1234,
            "modem_4g": {
                "supported": True, "state": "success", "action": "query",
                "request_id": "request-4g", "error": None,
                "operator": "TEST_OPERATOR", "registration": 1, "rssi": 17,
                "rat": "LTE", "band_config": "example-band-mask",
                "cell_lock": "0", "cells": [{
                    "serving": True, "mcc": 999, "mnc": 99,
                    "tac": "00AA", "cell_id": "0011224",
                    "earfcn": 12345, "pci": 101, "band": 3,
                    "rxlev": 67, "rsrp": 57, "rsrq": 21,
                }],
            },
            "video_fps": {
                "supported": True, "fps": 20,
                "resolution": "1280x720", "width": 1280, "height": 720,
                "request_id": "request-fps", "error": None,
            },
        }
        normalized = receiver._normalize_packet(heartbeat)
        self.assertEqual(normalized["modem_4g"]["cells"][0]["earfcn"], 12345)
        self.assertEqual(normalized["video_fps"]["fps"], 20)
        self.assertEqual(normalized["video_fps"]["resolution"], "1280x720")
        self.assertEqual(normalized["video_fps"]["width"], 1280)

    def test_legacy_video_fps_state_defaults_to_vga(self):
        state = receiver._normalize_video_fps({
            "supported": True, "fps": 8, "request_id": None, "error": None,
        })
        self.assertEqual(state["resolution"], "640x480")
        self.assertEqual((state["width"], state["height"]), (640, 480))

    def test_video_state_rejects_unknown_or_mismatched_resolution(self):
        self.assertFalse(receiver._normalize_video_fps({
            "supported": True, "fps": 8, "resolution": "1024x768",
        }))
        self.assertFalse(receiver._normalize_video_fps({
            "supported": True, "fps": 8, "resolution": "1280x720",
            "width": 640, "height": 480,
        }))

    def test_heartbeat_accepts_wifi_state_and_strips_password(self):
        heartbeat = {
            "protocol": "ESUT/1", "kind": "heartbeat",
            "device_id": "esp32s3-01", "boot_id": "89ABCDEF",
            "timestamp": 1234,
            "wifi_sta": {
                "supported": True, "state": "working", "action": "scan",
                "request_id": "request-wifi", "error": None,
                "feature_enabled": True, "scanning": True, "connected": True,
                "ssid": "RescueNet", "ip": "198.51.100.23", "rssi": -51,
                "wifi_uplink_selected": True, "active_uplink": "wifi",
                "password": "must-never-leave-device",
                "networks": [{
                    "ssid": "RescueNet", "rssi": -51, "channel": 6,
                    "security": "wpa2", "secured": True, "supported": True,
                    "password": "also-secret",
                }],
            },
        }
        normalized = receiver._normalize_packet(heartbeat)
        wifi = normalized["wifi_sta"]
        self.assertEqual(wifi["state"], "working")
        self.assertIs(wifi["scanning"], True)
        self.assertEqual(wifi["request_id"], "request-wifi")
        self.assertEqual(wifi["networks"][0]["ssid"], "RescueNet")
        self.assertNotIn("password", wifi)
        self.assertNotIn("password", wifi["networks"][0])

    def test_invalid_wifi_state_is_rejected(self):
        heartbeat = {
            "protocol": "ESUT/1", "kind": "heartbeat",
            "device_id": "esp32s3-01", "boot_id": "89ABCDEF",
            "wifi_sta": {
                "supported": True, "state": "success", "action": "scan",
                "request_id": "request-wifi", "error": None,
                "feature_enabled": True, "scanning": False, "connected": False,
                "ssid": "", "ip": "", "rssi": None,
                "wifi_uplink_selected": False, "active_uplink": "l610",
                "networks": [{
                    "ssid": "BadChannel", "rssi": -60, "channel": 99,
                    "security": "wpa2", "secured": True, "supported": True,
                }],
            },
        }
        self.assertIsNone(receiver._normalize_packet(heartbeat))

    def test_wifi_scanning_must_be_boolean(self):
        heartbeat = {
            "protocol": "ESUT/1", "kind": "heartbeat",
            "device_id": "esp32s3-01", "boot_id": "89ABCDEF",
            "wifi_sta": {
                "supported": True, "state": "working", "action": "scan",
                "request_id": "request-wifi", "error": None,
                "feature_enabled": True, "scanning": 1, "connected": False,
                "ssid": "", "ip": "", "rssi": None,
                "wifi_uplink_selected": False, "active_uplink": "l610",
                "networks": [],
            },
        }
        self.assertIsNone(receiver._normalize_packet(heartbeat))

    def test_control_state_accepts_enabled_compatibility_field(self):
        status = {
            "protocol": "ESUT/1",
            "kind": "control_state",
            "device_id": "esp32s3-01",
            "boot_id": "89ABCDEF",
            "request_id": "request-8",
            "ap_stream": {"supported": True, "enabled": False},
        }
        normalized = receiver._normalize_packet(status)
        self.assertTrue(normalized["status_only"])
        self.assertEqual(normalized["ap_stream"]["state"], "disabled")
        self.assertEqual(normalized["ap_stream"]["request_id"], "request-8")

    def test_ap_stream_command_uses_esctl_device_identity(self):
        payload = receiver._prepare_command_payload(
            {"cmd": "ap_stream", "enabled": True, "request_id": "req-1"},
            "esp32s3-01",
            "89ABCDEF",
        )
        self.assertEqual(payload, {
            "protocol": "ESCTL/1",
            "kind": "command",
            "cmd": "ap_stream",
            "request_id": "req-1",
            "device_id": "esp32s3-01",
            "boot_id": "89ABCDEF",
            "params": {"enabled": True},
        })

    def test_ps2_command_uses_esctl_device_identity(self):
        payload = receiver._prepare_command_payload(
            {
                "cmd": "ps2_button",
                "button": "PAD_UP",
                "state": "down",
                "request_id": "req-2",
            },
            "esp32s3-01",
            "89ABCDEF",
        )
        self.assertEqual(payload, {
            "protocol": "ESCTL/1",
            "kind": "command",
            "cmd": "ps2_button",
            "request_id": "req-2",
            "device_id": "esp32s3-01",
            "boot_id": "89ABCDEF",
            "params": {"button": "PAD_UP", "state": "down"},
        })

    def test_modem_and_video_commands_use_typed_esctl_payloads(self):
        modem = receiver._prepare_command_payload({
            "cmd": "modem_4g", "action": "set_bands",
            "bands": [3, 8, 40], "request_id": "req-4g",
        }, "esp32s3-01", "89ABCDEF")
        self.assertEqual(modem["params"], {
            "action": "set_bands", "bands": [3, 8, 40],
        })
        video = receiver._prepare_command_payload({
            "cmd": "video_fps", "fps": 15, "request_id": "req-fps",
        }, "esp32s3-01", "89ABCDEF")
        self.assertEqual(video["params"], {"fps": 15})
        video_hd = receiver._prepare_command_payload({
            "cmd": "video_fps", "fps": 20, "resolution": "1280x720",
            "request_id": "req-video-hd",
        }, "esp32s3-01", "89ABCDEF")
        self.assertEqual(video_hd["params"], {
            "fps": 20, "resolution": "1280x720",
        })
        self.assertIsNone(receiver._prepare_command_payload({
            "cmd": "video_fps", "fps": 8, "resolution": "1280x800",
            "request_id": "req-video-bad",
        }, "esp32s3-01", "89ABCDEF"))

    def test_wifi_commands_use_typed_esctl_payloads(self):
        common = ("esp32s3-01", "89ABCDEF")
        commands = (
            ({"cmd": "wifi_sta", "action": "query", "request_id": "wifi-1"},
             {"action": "query"}),
            ({"cmd": "wifi_sta", "action": "set_enabled", "enabled": True,
              "request_id": "wifi-2"},
             {"action": "set_enabled", "enabled": True}),
            ({"cmd": "wifi_sta", "action": "scan", "request_id": "wifi-3"},
             {"action": "scan"}),
            ({"cmd": "wifi_sta", "action": "connect", "ssid": "RescueNet",
              "password": "secret123", "security": "wpa2",
              "request_id": "wifi-4"},
             {"action": "connect", "ssid": "RescueNet",
              "password": "secret123", "security": "wpa2"}),
            ({"cmd": "wifi_sta", "action": "select_uplink", "use_wifi": True,
              "request_id": "wifi-5"},
             {"action": "select_uplink", "use_wifi": True}),
        )
        for command, expected_params in commands:
            with self.subTest(action=command["action"]):
                payload = receiver._prepare_command_payload(command, *common)
                self.assertEqual(payload["cmd"], "wifi_sta")
                self.assertEqual(payload["params"], expected_params)

    def test_invalid_wifi_commands_are_rejected(self):
        invalid = (
            {"cmd": "wifi_sta", "action": "invalid", "request_id": "wifi-1"},
            {"cmd": "wifi_sta", "action": "set_enabled", "enabled": 1,
             "request_id": "wifi-2"},
            {"cmd": "wifi_sta", "action": "connect", "ssid": "RescueNet",
             "password": "short", "security": "wpa2", "request_id": "wifi-3"},
            {"cmd": "wifi_sta", "action": "connect", "ssid": "RescueNet",
             "password": "not-empty", "security": "open", "request_id": "wifi-4"},
            {"cmd": "wifi_sta", "action": "select_uplink", "use_wifi": 1,
             "request_id": "wifi-5"},
        )
        for command in invalid:
            with self.subTest(action=command["action"]):
                self.assertIsNone(receiver._prepare_command_payload(
                    command, "esp32s3-01", "89ABCDEF"
                ))

    def test_wifi_mixed_security_names_match_firmware_contract(self):
        for security in ("wpa/wpa2", "wpa2/wpa3"):
            with self.subTest(security=security):
                payload = receiver._prepare_command_payload({
                    "cmd": "wifi_sta", "action": "connect",
                    "ssid": "RescueNet", "password": "secret123",
                    "security": security, "request_id": "wifi-mixed",
                }, "esp32s3-01", "89ABCDEF")
                self.assertEqual(payload["params"]["security"], security)
        self.assertIsNone(receiver._prepare_command_payload({
            "cmd": "wifi_sta", "action": "connect",
            "ssid": "RescueNet", "password": "secret123",
            "security": "wpa2-wpa3", "request_id": "wifi-bad",
        }, "esp32s3-01", "89ABCDEF"))

    def test_unsupported_command_payload_is_rejected(self):
        command = {"cmd": "move", "direction": "forward", "speed": 50}
        self.assertIsNone(receiver._prepare_command_payload(
            command, "esp32s3-01", "89ABCDEF"
        ))

    def test_invalid_ps2_payload_is_rejected_before_udp_send(self):
        for button, state in (
                ("CAMERA_LEFT", "down"), ("PAD_UP", "hold"),
                (["PAD_UP"], "down"), ("PAD_UP", ["down"])):
            with self.subTest(button=button, state=state):
                self.assertIsNone(receiver._prepare_command_payload(
                    {
                        "cmd": "ps2_button",
                        "button": button,
                        "state": state,
                        "request_id": "req-invalid",
                    },
                    "esp32s3-01",
                    "89ABCDEF",
                ))

    def test_rejects_non_finite_values(self):
        packet = self.packet()
        packet["sensor"]["temperature"] = float("nan")
        self.assertIsNone(receiver._normalize_packet(packet))

    def test_numeric_gas_flag_is_normalized_to_alarm(self):
        alarm_packet = self.packet()
        alarm_packet["sensor"]["gas"] = 1
        self.assertEqual(
            receiver._normalize_packet(alarm_packet)["sensor"]["gas"],
            {"alarm": True},
        )

        normal_packet = self.packet(2)
        normal_packet["sensor"]["gas"] = 0
        self.assertEqual(
            receiver._normalize_packet(normal_packet)["sensor"]["gas"],
            {"alarm": False},
        )

    def test_numeric_alarm_inside_legacy_gas_object_is_supported(self):
        packet = self.packet()
        packet["sensor"]["gas"] = {"alarm": 1, "concentration": 85}
        gas = receiver._normalize_packet(packet)["sensor"]["gas"]
        self.assertIs(gas["alarm"], True)
        self.assertEqual(gas["concentration"], 85)

    def test_other_numeric_gas_values_are_rejected(self):
        packet = self.packet()
        packet["sensor"]["gas"] = 2
        self.assertIsNone(receiver._normalize_packet(packet))

    def test_duplicate_and_old_sequence_do_not_refresh_link(self):
        first = receiver._normalize_packet(self.packet(10))
        duplicate = receiver._normalize_packet(self.packet(10))
        old = receiver._normalize_packet(self.packet(9))
        new = receiver._normalize_packet(self.packet(11))
        self.assertTrue(receiver._is_new_sequence(first))
        self.assertFalse(receiver._is_new_sequence(duplicate))
        self.assertFalse(receiver._is_new_sequence(old))
        self.assertTrue(receiver._is_new_sequence(new))

    def test_legacy_flat_packet_remains_supported(self):
        packet = receiver._normalize_packet({"temperature": 25.0, "lat": 1.0, "lng": 2.0})
        self.assertIsNotNone(packet)
        self.assertTrue(packet["legacy"])


if __name__ == "__main__":
    unittest.main()
