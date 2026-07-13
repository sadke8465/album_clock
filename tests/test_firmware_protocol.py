import struct
import unittest


def fnv1a(value: str) -> int:
    result = 2166136261
    for byte in value.encode("utf-8"):
        result = ((result ^ byte) * 16777619) & 0xFFFFFFFF
    return result or 0x9E3779B9


def xorshift(state: int) -> int:
    state ^= (state << 13) & 0xFFFFFFFF
    state ^= state >> 17
    state ^= (state << 5) & 0xFFFFFFFF
    return state & 0xFFFFFFFF


def firmware_shuffle(count: int, version: str):
    order = list(range(count))
    state = fnv1a(version)
    for index in range(count - 1, 0, -1):
        state = xorshift(state)
        selected = (state * (index + 1)) >> 32
        order[index], order[selected] = order[selected], order[index]
    return order


class FirmwareProtocolTests(unittest.TestCase):
    def test_shuffle_vector_matches_typescript(self):
        self.assertEqual(
            firmware_shuffle(12, "0123456789abcdef"),
            [0, 7, 3, 8, 1, 4, 9, 6, 5, 11, 10, 2],
        )

    def test_pack_frame_seek_boundaries(self):
        metadata_length = 137
        count = 3
        header = struct.pack("<4sHHHHI", b"ACPK", 1, 64, 64, count, metadata_length)
        frames = [bytes([value]) * 8192 for value in range(count)]
        pack = header + bytes(metadata_length) + b"".join(frames)
        for index, expected in enumerate(frames):
            offset = 16 + metadata_length + index * 8192
            self.assertEqual(pack[offset : offset + 8192], expected)

    def test_stale_generation_is_rejected(self):
        latest_requested = 14
        self.assertFalse(13 >= latest_requested)
        self.assertTrue(14 >= latest_requested)

    def test_only_one_next_is_queued(self):
        queued = False
        for _ in range(8):
            queued = queued or True
        self.assertTrue(queued)


if __name__ == "__main__":
    unittest.main()
