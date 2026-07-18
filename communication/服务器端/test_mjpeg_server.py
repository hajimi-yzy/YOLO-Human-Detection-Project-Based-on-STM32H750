import unittest

import mjpeg_server


class DirectJpegPublishTest(unittest.TestCase):
    def test_fhd_sized_jpeg_above_old_limit_is_published(self):
        jpeg = b"\xff\xd8" + (b"fhd" * 100000) + b"\xff\xd9"
        self.assertGreater(len(jpeg), 256 * 1024)
        self.assertTrue(mjpeg_server.feed_jpeg_frame(
            jpeg, device_id=1, width=1920, height=1080, frame_seq=7,
        ))
        _, published, metadata = mjpeg_server.get_latest_jpeg()
        self.assertEqual(published, jpeg)
        self.assertEqual((metadata["width"], metadata["height"]), (1920, 1080))

    def test_frame_above_bounded_limit_is_rejected(self):
        jpeg = b"\xff\xd8" + (b"x" * mjpeg_server.MAX_JPEG_SIZE) + b"\xff\xd9"
        self.assertFalse(mjpeg_server.feed_jpeg_frame(jpeg))


if __name__ == "__main__":
    unittest.main()
