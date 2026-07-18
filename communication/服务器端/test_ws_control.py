import unittest

import ws_control


class FakeWebSocket:
    def __init__(self):
        self.messages = []

    async def send_json(self, message):
        self.messages.append(message)

    async def prepare(self, request):
        return self

    def __aiter__(self):
        return self

    async def __anext__(self):
        raise StopAsyncIteration


class FakeRequest:
    remote = "127.0.0.1"


class ControlValidationTest(unittest.IsolatedAsyncioTestCase):
    async def test_first_connection_receives_snake_case_device_state(self):
        socket = FakeWebSocket()
        original_factory = ws_control.web.WebSocketResponse
        original_get_state = ws_control.get_ap_stream_state
        original_get_modem = ws_control.get_modem_4g_state
        original_get_video_fps = ws_control.get_video_fps_state
        original_get_wifi = ws_control.get_wifi_sta_state
        ws_control.web.WebSocketResponse = lambda **kwargs: socket
        ws_control.get_ap_stream_state = lambda: {
            "device_id": "esp32s3-01",
            "boot_id": "89ABCDEF",
            "online": True,
            "supported": True,
            "state": "enabled",
            "request_id": "request-0",
            "error": None,
            "received_at": 123.5,
        }
        ws_control.get_modem_4g_state = lambda: {
            "online": True, "supported": True, "state": "idle",
            "action": "query", "cells": [],
        }
        ws_control.get_video_fps_state = lambda: {
            "online": True, "supported": True, "fps": 8,
        }
        ws_control.get_wifi_sta_state = lambda: {
            "online": True, "supported": True, "state": "idle",
            "action": "query", "feature_enabled": False,
            "scanning": False, "connected": False,
            "ssid": "", "ip": "", "rssi": None,
            "wifi_uplink_selected": False, "active_uplink": "l610",
            "networks": [],
        }
        try:
            response = await ws_control.ws_control_handler(FakeRequest())
        finally:
            ws_control.web.WebSocketResponse = original_factory
            ws_control.get_ap_stream_state = original_get_state
            ws_control.get_modem_4g_state = original_get_modem
            ws_control.get_video_fps_state = original_get_video_fps
            ws_control.get_wifi_sta_state = original_get_wifi

        self.assertIs(response, socket)
        self.assertEqual(socket.messages, [{
            "type": "device_state",
            "device_id": "esp32s3-01",
            "boot_id": "89ABCDEF",
            "online": True,
            "ap_stream": {
                "supported": True,
                "state": "enabled",
                "request_id": "request-0",
                "error": None,
                "received_at": 123.5,
            },
            "modem_4g": {
                "supported": True, "state": "idle", "action": "query",
                "request_id": None, "error": None, "operator": None,
                "registration": None, "rssi": None, "rat": None,
                "band_config": "", "cell_lock": "", "cells": [],
                "received_at": None,
            },
            "video_fps": {
                "supported": True, "fps": 8,
                "resolution": "640x480", "width": 640, "height": 480,
                "request_id": None,
                "error": None, "received_at": None,
            },
            "wifi_sta": {
                "supported": True, "state": "idle", "action": "query",
                "request_id": None, "error": None,
                "feature_enabled": False, "scanning": False,
                "connected": False,
                "ssid": "", "ip": "", "rssi": None,
                "wifi_uplink_selected": False, "active_uplink": "l610",
                "networks": [], "received_at": None,
            },
        }])

    def test_ap_stream_requires_boolean_and_request_id(self):
        self.assertEqual(
            ws_control._validate_command(
                "ap_stream", {"enabled": True}, "request-1"
            ),
            {
                "cmd": "ap_stream",
                "enabled": True,
                "request_id": "request-1",
            },
        )
        self.assertIsNone(
            ws_control._validate_command("ap_stream", {"enabled": 1}, "request-1")
        )
        self.assertIsNone(
            ws_control._validate_command("ap_stream", {"enabled": True}, "")
        )
        self.assertIsNone(
            ws_control._validate_command("ap_stream", {"enabled": True}, "r" * 65)
        )

    def test_all_ps2_buttons_accept_down_and_up(self):
        expected_buttons = {
            "PAD_UP", "PAD_RIGHT", "PAD_DOWN", "PAD_LEFT",
            "L2", "R2", "L1", "R1",
            "TRIANGLE", "CIRCLE", "CROSS", "SQUARE",
            "SELECT", "START",
        }
        self.assertEqual(ws_control.VALID_PS2_BUTTONS, expected_buttons)
        for button in expected_buttons:
            for state in ("down", "up"):
                with self.subTest(button=button, state=state):
                    self.assertEqual(
                        ws_control._validate_command(
                            "ps2_button",
                            {"button": button, "state": state},
                            "request-ps2",
                        ),
                        {
                            "cmd": "ps2_button",
                            "button": button,
                            "state": state,
                            "request_id": "request-ps2",
                        },
                    )

    def test_ps2_button_rejects_invalid_button_state_and_request_id(self):
        self.assertIsNone(ws_control._validate_command(
            "ps2_button", {"button": "CAMERA_LEFT", "state": "down"}, "req-1"
        ))
        self.assertIsNone(ws_control._validate_command(
            "ps2_button", {"button": "PAD_UP", "state": "hold"}, "req-1"
        ))
        self.assertIsNone(ws_control._validate_command(
            "ps2_button", {"button": ["PAD_UP"], "state": "down"}, "req-1"
        ))
        self.assertIsNone(ws_control._validate_command(
            "ps2_button", {"button": "PAD_UP", "state": ["down"]}, "req-1"
        ))
        self.assertIsNone(ws_control._validate_command(
            "ps2_button", {"button": "PAD_UP", "state": "down"}, ""
        ))

    def test_legacy_motion_commands_are_rejected(self):
        self.assertIsNone(ws_control._validate_command(
            "move", {"direction": "left", "speed": 40}, "request-1"
        ))
        self.assertIsNone(ws_control._validate_command("stop", {}, "request-2"))
        self.assertIsNone(ws_control._validate_command(
            "emergency_stop", {}, "request-3"
        ))

    def test_modem_and_video_commands_are_strictly_validated(self):
        self.assertEqual(ws_control._validate_command(
            "modem_4g", {"action": "ping"}, "request-ping"
        ), {"cmd": "modem_4g", "action": "ping", "request_id": "request-ping"})
        self.assertEqual(ws_control._validate_command(
            "modem_4g", {"action": "set_cell_lock", "earfcn": 38950, "pci": 312}, "request-cell"
        ), {
            "cmd": "modem_4g", "action": "set_cell_lock",
            "earfcn": 38950, "pci": 312, "request_id": "request-cell",
        })
        self.assertIsNone(ws_control._validate_command(
            "modem_4g", {"action": "set_cell_lock", "earfcn": 38950, "pci": 504}, "bad"
        ))
        self.assertEqual(ws_control._validate_command(
            "video_fps", {"fps": 20}, "request-fps"
        ), {"cmd": "video_fps", "fps": 20, "request_id": "request-fps"})
        self.assertEqual(ws_control._validate_command(
            "video_fps", {"fps": 30, "resolution": "1920x1080"},
            "request-video-fhd"
        ), {
            "cmd": "video_fps", "fps": 30, "resolution": "1920x1080",
            "request_id": "request-video-fhd",
        })
        self.assertIsNone(ws_control._validate_command(
            "video_fps", {"fps": 28}, "request-fps"
        ))
        self.assertIsNone(ws_control._validate_command(
            "video_fps", {"fps": 8, "resolution": "1024x768"}, "request-fps"
        ))

    def test_wifi_commands_are_strictly_validated(self):
        self.assertEqual(ws_control._validate_command(
            "wifi_sta", {"action": "query"}, "wifi-query"
        ), {"cmd": "wifi_sta", "action": "query", "request_id": "wifi-query"})
        self.assertEqual(ws_control._validate_command(
            "wifi_sta", {"action": "set_enabled", "enabled": True}, "wifi-enable"
        ), {
            "cmd": "wifi_sta", "action": "set_enabled", "enabled": True,
            "request_id": "wifi-enable",
        })
        self.assertEqual(ws_control._validate_command(
            "wifi_sta", {
                "action": "connect", "ssid": "RescueNet",
                "password": "secret123", "security": "wpa2",
            }, "wifi-connect"
        ), {
            "cmd": "wifi_sta", "action": "connect", "ssid": "RescueNet",
            "password": "secret123", "security": "wpa2",
            "request_id": "wifi-connect",
        })
        for security in ("wpa/wpa2", "wpa2/wpa3"):
            with self.subTest(security=security):
                command = ws_control._validate_command(
                    "wifi_sta", {
                        "action": "connect", "ssid": "RescueNet",
                        "password": "secret123", "security": security,
                    }, f"wifi-{security.replace('/', '-')}",
                )
                self.assertEqual(command["security"], security)
        self.assertEqual(ws_control._validate_command(
            "wifi_sta", {"action": "select_uplink", "use_wifi": False}, "wifi-uplink"
        ), {
            "cmd": "wifi_sta", "action": "select_uplink", "use_wifi": False,
            "request_id": "wifi-uplink",
        })
        invalid = (
            ({"action": "set_enabled", "enabled": 1}, "bad-enabled"),
            ({"action": "connect", "ssid": "", "password": "secret123",
              "security": "wpa2"}, "bad-ssid"),
            ({"action": "connect", "ssid": "RescueNet", "password": "short",
              "security": "wpa2"}, "bad-password"),
            ({"action": "connect", "ssid": "RescueNet", "password": "",
              "security": "wep"}, "bad-security"),
            ({"action": "connect", "ssid": "RescueNet", "password": "secret123",
              "security": "wpa-wpa2"}, "bad-hyphen-security"),
            ({"action": "select_uplink", "use_wifi": 1}, "bad-uplink"),
            ({"action": "unknown"}, "bad-action"),
        )
        for params, request_id in invalid:
            with self.subTest(request_id=request_id):
                self.assertIsNone(ws_control._validate_command(
                    "wifi_sta", params, request_id
                ))

    async def test_successful_send_reports_sent(self):
        socket = FakeWebSocket()
        captured = []
        original = ws_control.send_command_to_bw21
        ws_control.send_command_to_bw21 = lambda command: captured.append(command) or True
        try:
            await ws_control.handle_control_message(socket, {
                "type": "command_request",
                "requestId": "request-2",
                "cmd": "ap_stream",
                "params": {"enabled": True},
            })
        finally:
            ws_control.send_command_to_bw21 = original

        self.assertEqual(captured[0]["request_id"], "request-2")
        self.assertEqual(socket.messages, [{
            "type": "command_ack",
            "requestId": "request-2",
            "accepted": True,
            "message": "queued",
        }])

    async def test_ps2_command_is_forwarded_without_motion_translation(self):
        socket = FakeWebSocket()
        captured = []
        original = ws_control.send_command_to_bw21
        ws_control.send_command_to_bw21 = lambda command: captured.append(command) or True
        try:
            await ws_control.handle_control_message(socket, {
                "type": "command_request",
                "requestId": "request-3",
                "cmd": "ps2_button",
                "params": {"button": "PAD_LEFT", "state": "down"},
            })
        finally:
            ws_control.send_command_to_bw21 = original

        self.assertEqual(captured, [{
            "cmd": "ps2_button",
            "button": "PAD_LEFT",
            "state": "down",
            "request_id": "request-3",
        }])
        self.assertEqual(socket.messages, [{
            "type": "command_ack",
            "requestId": "request-3",
            "accepted": True,
            "message": "sent",
        }])

    async def test_wifi_password_is_only_forwarded_to_device(self):
        socket = FakeWebSocket()
        captured = []
        original = ws_control.send_command_to_bw21
        ws_control.send_command_to_bw21 = lambda command: captured.append(command) or True
        try:
            await ws_control.handle_control_message(socket, {
                "type": "command_request",
                "requestId": "wifi-connect",
                "cmd": "wifi_sta",
                "params": {
                    "action": "connect", "ssid": "RescueNet",
                    "password": "secret123", "security": "wpa2",
                },
            })
        finally:
            ws_control.send_command_to_bw21 = original

        self.assertEqual(captured[0]["password"], "secret123")
        self.assertNotIn("password", socket.messages[0])
        self.assertEqual(socket.messages, [{
            "type": "command_ack", "requestId": "wifi-connect",
            "accepted": True, "message": "queued",
        }])


if __name__ == "__main__":
    unittest.main()
