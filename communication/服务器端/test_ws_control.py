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
        try:
            response = await ws_control.ws_control_handler(FakeRequest())
        finally:
            ws_control.web.WebSocketResponse = original_factory
            ws_control.get_ap_stream_state = original_get_state

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


if __name__ == "__main__":
    unittest.main()
