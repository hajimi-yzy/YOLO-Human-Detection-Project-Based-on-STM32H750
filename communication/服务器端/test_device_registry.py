import unittest

from device_registry import DeviceRegistry
from esp_jpeg_udp import CompletedJpegFrame


class DeviceRegistryTest(unittest.TestCase):
    def test_rates_and_offline_timeout(self):
        monotonic = [10.0]
        wall = [1000.0]
        registry = DeviceRegistry(lambda: monotonic[0], lambda: wall[0])

        def frame(seq):
            return CompletedJpegFrame(
                source=("10.0.0.2", 40000), device_id=0x1234,
                frame_seq=seq, timestamp_ms=seq * 40,
                width=640, height=480, jpeg=b"x" * 1024,
            )

        registry.record_jpeg(frame(1))
        monotonic[0] += 0.04
        wall[0] += 0.04
        registry.record_jpeg(frame(2))
        device = registry.latest()
        self.assertTrue(device["online"])
        self.assertEqual(device["device_id"], "00001234")
        self.assertAlmostEqual(device["fps"], 25.0, places=1)
        self.assertAlmostEqual(device["kib_per_second"], 25.0, places=1)

        monotonic[0] += 16.0
        self.assertFalse(registry.latest()["online"])


if __name__ == "__main__":
    unittest.main()
