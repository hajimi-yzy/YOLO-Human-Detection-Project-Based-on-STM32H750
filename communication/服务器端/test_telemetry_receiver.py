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
                "lat": 39.9,
                "lng": 116.4,
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
