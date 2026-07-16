import unittest
from types import SimpleNamespace

from udp_receiver import _accept_completed_frame


def frame(sequence, timestamp=1000, device_id=1):
    return SimpleNamespace(
        frame_seq=sequence,
        timestamp_ms=timestamp,
        device_id=device_id,
    )


class CompletedFrameOrderingTest(unittest.TestCase):
    def test_late_frame_cannot_replace_newer_frame(self):
        latest = {}
        self.assertTrue(_accept_completed_frame(frame(11), latest, 1.0))
        self.assertFalse(_accept_completed_frame(frame(10), latest, 1.1))

    def test_sequence_wrap_is_accepted(self):
        latest = {}
        self.assertTrue(_accept_completed_frame(frame(0xFFFFFFFF), latest, 1.0))
        self.assertTrue(_accept_completed_frame(frame(0), latest, 1.1))

    def test_timestamp_reset_starts_new_boot(self):
        latest = {}
        self.assertTrue(_accept_completed_frame(frame(900, 500000), latest, 1.0))
        self.assertTrue(_accept_completed_frame(frame(0, 100), latest, 1.1))


if __name__ == "__main__":
    unittest.main()
