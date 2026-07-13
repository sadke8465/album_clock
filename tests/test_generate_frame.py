"""Tests for the artwork generator's candidate selection and RGB565 encoding.

Run with:  python -m pytest tests/   (or)  python -m unittest discover tests
Requires Pillow (see requirements.txt).
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from PIL import Image  # noqa: E402

import generate_frame as g  # noqa: E402


def make_rows(n):
    return [g.Candidate(mode="fallback", title=f"t{i}", artist=f"a{i}") for i in range(n)]


class SelectionTests(unittest.TestCase):
    def test_empty(self):
        self.assertIsNone(g.select_csv_candidate([], window=5))

    def test_single(self):
        rows = make_rows(1)
        self.assertEqual(g.select_csv_candidate(rows, window=5), rows[0])

    def test_deterministic_within_window(self):
        rows = make_rows(60)
        for w in range(200):
            self.assertEqual(
                g.select_csv_candidate(rows, window=w),
                g.select_csv_candidate(rows, window=w),
            )

    def test_no_back_to_back_repeat(self):
        rows = make_rows(60)
        for w in range(1, 500):
            self.assertNotEqual(
                g.select_csv_candidate(rows, window=w),
                g.select_csv_candidate(rows, window=w - 1),
                f"window {w} repeated the previous album",
            )

    def test_spread(self):
        rows = make_rows(60)
        seen = {g.select_csv_candidate(rows, window=w).title for w in range(300)}
        self.assertGreater(len(seen), 30)


class Rgb565Tests(unittest.TestCase):
    def test_length(self):
        img = Image.new("RGB", (g.DISPLAY_SIZE, g.DISPLAY_SIZE), (10, 20, 30))
        self.assertEqual(len(g.image_to_rgb565_le(img)), g.FRAME_SIZE_BYTES)

    def test_endianness_pure_red(self):
        img = Image.new("RGB", (g.DISPLAY_SIZE, g.DISPLAY_SIZE), (255, 0, 0))
        data = g.image_to_rgb565_le(img)
        # 0xF800 little-endian -> low byte first.
        self.assertEqual(data[0], 0x00)
        self.assertEqual(data[1], 0xF8)

    def test_process_image_is_square_64(self):
        wide = Image.new("RGB", (200, 100), (128, 64, 200))
        out = g.process_image(wide)
        self.assertEqual(out.size, (g.DISPLAY_SIZE, g.DISPLAY_SIZE))


if __name__ == "__main__":
    unittest.main()
