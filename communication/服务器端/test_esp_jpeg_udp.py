import struct
import unittest
import zlib

from esp_jpeg_udp import (
    EspJpegReassembler,
    HEADER,
    HEADER_SIZE,
    MAGIC,
    MAX_CHUNK_PAYLOAD,
    VERSION,
    VERSION_V2,
    V2_MAX_CHUNK_PAYLOAD,
)


def packets_for(jpeg, seq=7, device_id=0x11223344, width=640, height=480,
                version=VERSION, chunk_payload_size=None):
    if chunk_payload_size is None:
        chunk_payload_size = (
            V2_MAX_CHUNK_PAYLOAD if version == VERSION_V2
            else MAX_CHUNK_PAYLOAD
        )
    crc = zlib.crc32(jpeg) & 0xFFFFFFFF
    count = (len(jpeg) + chunk_payload_size - 1) // chunk_payload_size
    result = []
    for index in range(count):
        payload = jpeg[
            index * chunk_payload_size:(index + 1) * chunk_payload_size
        ]
        result.append(HEADER.pack(
            MAGIC, version, 0, HEADER_SIZE, device_id, seq, 123456,
            len(jpeg), crc, index, count, len(payload), width, height,
        ) + payload)
    return result


def replace_header(packet, **changes):
    values = list(HEADER.unpack_from(packet))
    fields = {
        "magic": 0,
        "version": 1,
        "flags": 2,
        "header_len": 3,
        "device_id": 4,
        "frame_seq": 5,
        "timestamp_ms": 6,
        "frame_len": 7,
        "frame_crc32": 8,
        "chunk_index": 9,
        "chunk_count": 10,
        "payload_len": 11,
        "width": 12,
        "height": 13,
    }
    for name, value in changes.items():
        values[fields[name]] = value
    return HEADER.pack(*values) + packet[HEADER_SIZE:]


class EspJpegReassemblerTest(unittest.TestCase):
    def setUp(self):
        self.source = ("10.0.0.2", 54321)
        self.jpeg = b"\xff\xd8" + (b"camera" * 700) + b"\xff\xd9"

    def test_out_of_order_v1_frame_completes(self):
        receiver = EspJpegReassembler()
        complete = None
        for packet in reversed(packets_for(self.jpeg)):
            complete = receiver.push(packet, self.source) or complete
        self.assertIsNotNone(complete)
        self.assertEqual(complete.jpeg, self.jpeg)
        self.assertEqual((complete.width, complete.height), (640, 480))
        self.assertGreater(receiver.stats["reordered"], 0)

    def test_v2_frame_completes(self):
        jpeg = b"\xff\xd8" + (b"v2-camera" * 1800) + b"\xff\xd9"
        packets = packets_for(jpeg, version=VERSION_V2)
        self.assertEqual(len(packets), 3)

        receiver = EspJpegReassembler()
        complete = None
        for packet in packets:
            complete = receiver.push(packet, self.source) or complete

        self.assertIsNotNone(complete)
        self.assertEqual(complete.jpeg, jpeg)

    def test_v2_fhd_frame_above_old_256k_limit_completes(self):
        jpeg = b"\xff\xd8" + (b"fhd" * 100000) + b"\xff\xd9"
        self.assertGreater(len(jpeg), 256 * 1024)
        receiver = EspJpegReassembler()
        complete = None
        for packet in packets_for(
                jpeg, version=VERSION_V2, width=1920, height=1080):
            complete = receiver.push(packet, self.source) or complete
        self.assertIsNotNone(complete)
        self.assertEqual(complete.jpeg, jpeg)
        self.assertEqual((complete.width, complete.height), (1920, 1080))

    def test_out_of_order_v2_frame_completes(self):
        jpeg = b"\xff\xd8" + (b"v2-reordered" * 1600) + b"\xff\xd9"
        receiver = EspJpegReassembler()
        complete = None
        for packet in reversed(packets_for(jpeg, version=VERSION_V2)):
            complete = receiver.push(packet, self.source) or complete

        self.assertIsNotNone(complete)
        self.assertEqual(complete.jpeg, jpeg)
        self.assertGreater(receiver.stats["reordered"], 0)

    def test_v1_rejects_malformed_chunk_count(self):
        packet = packets_for(self.jpeg)[0]
        original_count = HEADER.unpack_from(packet)[10]
        malformed = replace_header(packet, chunk_count=original_count + 1)

        receiver = EspJpegReassembler()
        self.assertIsNone(receiver.push(malformed, self.source))
        self.assertEqual(receiver.stats["invalid"], 1)

    def test_v1_rejects_malformed_payload_length(self):
        packet = packets_for(self.jpeg)[0]
        original_length = HEADER.unpack_from(packet)[11]
        malformed = replace_header(packet, payload_len=original_length - 1)

        receiver = EspJpegReassembler()
        self.assertIsNone(receiver.push(malformed, self.source))
        self.assertEqual(receiver.stats["invalid"], 1)

    def test_v2_rejects_malformed_chunk_count(self):
        jpeg = b"\xff\xd8" + (b"v2-count" * 1200) + b"\xff\xd9"
        packet = packets_for(jpeg, version=VERSION_V2)[0]
        original_count = HEADER.unpack_from(packet)[10]
        malformed = replace_header(packet, chunk_count=original_count + 1)

        receiver = EspJpegReassembler()
        self.assertIsNone(receiver.push(malformed, self.source))
        self.assertEqual(receiver.stats["invalid"], 1)

    def test_v2_rejects_malformed_payload_length(self):
        jpeg = b"\xff\xd8" + (b"v2-length" * 1200) + b"\xff\xd9"
        packet = packets_for(jpeg, version=VERSION_V2)[0]
        original_length = HEADER.unpack_from(packet)[11]
        malformed = replace_header(packet, payload_len=original_length - 1)

        receiver = EspJpegReassembler()
        self.assertIsNone(receiver.push(malformed, self.source))
        self.assertEqual(receiver.stats["invalid"], 1)

    def test_v2_rejects_v1_sized_chunks(self):
        jpeg = b"\xff\xd8" + (b"wrong-size" * 1200) + b"\xff\xd9"
        malformed = packets_for(
            jpeg,
            version=VERSION_V2,
            chunk_payload_size=MAX_CHUNK_PAYLOAD,
        )[0]

        receiver = EspJpegReassembler()
        self.assertIsNone(receiver.push(malformed, self.source))
        self.assertEqual(receiver.stats["invalid"], 1)

    def test_duplicate_is_ignored(self):
        receiver = EspJpegReassembler()
        packets = packets_for(self.jpeg)
        self.assertIsNone(receiver.push(packets[0], self.source))
        self.assertIsNone(receiver.push(packets[0], self.source))
        complete = None
        for packet in packets[1:]:
            complete = receiver.push(packet, self.source) or complete
        self.assertIsNotNone(complete)
        self.assertEqual(receiver.stats["duplicates"], 1)

    def test_missing_chunk_expires(self):
        clock = [0.0]
        receiver = EspJpegReassembler(timeout=1.0, now_fn=lambda: clock[0])
        receiver.push(packets_for(self.jpeg)[0], self.source)
        clock[0] = 2.0
        receiver.push(packets_for(self.jpeg, seq=8)[0], self.source)
        self.assertEqual(receiver.stats["expired"], 1)
        self.assertGreater(receiver.stats["missing_chunks"], 0)

    def test_corrupt_payload_fails_crc(self):
        receiver = EspJpegReassembler()
        packets = packets_for(self.jpeg)
        packets[-1] = packets[-1][:-1] + bytes([packets[-1][-1] ^ 1])
        complete = None
        for packet in packets:
            complete = receiver.push(packet, self.source) or complete
        self.assertIsNone(complete)
        self.assertEqual(receiver.stats["crc_error"], 1)

    def test_interleaved_frames_both_complete(self):
        receiver = EspJpegReassembler()
        first = packets_for(self.jpeg, seq=10)
        second = packets_for(self.jpeg, seq=11)
        receiver.push(first[0], self.source)
        completed = []
        for packet in second:
            frame = receiver.push(packet, self.source)
            if frame is not None:
                completed.append(frame)
        for packet in first[1:]:
            frame = receiver.push(packet, self.source)
            if frame is not None:
                completed.append(frame)
        self.assertEqual([frame.frame_seq for frame in completed], [11, 10])

    def test_legacy_packet_is_rejected(self):
        receiver = EspJpegReassembler()
        self.assertIsNone(receiver.push(b"legacy frame\x64\x00\x00\x00", self.source))
        self.assertEqual(receiver.stats["invalid"], 1)


if __name__ == "__main__":
    unittest.main()
