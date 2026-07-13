import json
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

import build_fallback_pack as pack  # noqa: E402
import generate_frame as generator  # noqa: E402


def entry(index: int) -> pack.CatalogEntry:
    candidate = generator.Candidate(
        mode="fallback", title=f"Track {index}", artist=f"Artist {index}", album=f"Album {index}"
    )
    return pack.CatalogEntry(
        stable_id=f"id:{index}",
        name=f"Track {index}",
        artist=f"Artist {index}",
        album=f"Album {index}",
        source_fingerprint=f"fingerprint:{index}",
        candidate=candidate,
    )


class FallbackPackTests(unittest.TestCase):
    def test_round_trip_and_frame_boundaries(self):
        entries = [entry(0), entry(1)]
        frames = {
            "id:0": bytes([0x11]) * generator.FRAME_SIZE_BYTES,
            "id:1": bytes([0x22]) * generator.FRAME_SIZE_BYTES,
        }
        data, report = pack.build_pack(entries, lambda item: frames[item.stable_id])
        parsed = pack.parse_pack(data)
        self.assertEqual(len(parsed.frames), 2)
        self.assertEqual(parsed.frames[0], frames["id:0"])
        self.assertEqual(parsed.frames[1], frames["id:1"])
        self.assertEqual(report["generated_count"], 2)

    def test_reuses_unchanged_entry_and_skips_failed_new_entry(self):
        first, _ = pack.build_pack([entry(0)], lambda _item: bytes([7]) * generator.FRAME_SIZE_BYTES)
        previous = pack.parse_pack(first)

        def resolver(item):
            if item.stable_id == "id:1":
                raise ValueError("missing cover")
            raise AssertionError("unchanged entry should have been reused")

        second, report = pack.build_pack([entry(0), entry(1)], resolver, previous)
        parsed = pack.parse_pack(second)
        self.assertEqual(len(parsed.frames), 1)
        self.assertEqual(report["reused_count"], 1)
        self.assertEqual(report["skipped_count"], 1)

    def test_rejects_corrupt_length(self):
        data, _ = pack.build_pack([entry(0)], lambda _item: bytes(8192))
        with self.assertRaises(ValueError):
            pack.parse_pack(data[:-1])

    def test_unicode_metadata_is_utf8(self):
        hebrew = entry(0)
        hebrew = pack.CatalogEntry(
            stable_id=hebrew.stable_id,
            name="שיר",
            artist="אמן",
            album="אלבום",
            source_fingerprint=hebrew.source_fingerprint,
            candidate=hebrew.candidate,
        )
        data, _ = pack.build_pack([hebrew], lambda _item: bytes(8192))
        parsed = pack.parse_pack(data)
        metadata = json.dumps(parsed.metadata, ensure_ascii=False)
        self.assertIn("שיר", metadata)


if __name__ == "__main__":
    unittest.main()
