#!/usr/bin/env python3
"""Build an atomically publishable Album Clock fallback pack.

The output directory contains:
  fallback-<sha256>.acpk  immutable binary pack
  current.json            manifest uploaded last
  report.json             skipped-entry and reuse details
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import time
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Callable, Dict, Iterable, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_frame as generator  # noqa: E402


MAGIC = b"ACPK"
FORMAT_VERSION = 1
HEADER = struct.Struct("<4sHHHHI")
HEADER_BYTES = HEADER.size
MAX_PACK_BYTES = 8 * 1024 * 1024
PROCESSING_VERSION = "fallback-v1"


@dataclass(frozen=True)
class CatalogEntry:
    stable_id: str
    name: str
    artist: str
    album: str
    source_fingerprint: str
    candidate: generator.Candidate


@dataclass(frozen=True)
class PackedEntry:
    id: str
    name: str
    artist: str
    album: str
    source_fingerprint: str


@dataclass
class ParsedPack:
    metadata: Dict[str, object]
    frames: List[bytes]


def normalized(value: str) -> str:
    return " ".join(value.strip().casefold().split())


def digest_text(*parts: str) -> str:
    return hashlib.sha256("\x1f".join(parts).encode("utf-8")).hexdigest()


def stable_id_for(row: Dict[str, str]) -> str:
    apple_id = row.get("id", "").strip()
    if apple_id:
        return f"apple:{apple_id}"
    return "row:" + digest_text(
        normalized(row.get("artist", "")),
        normalized(row.get("album", "")),
        normalized(row.get("name", "") or row.get("title", "")),
    )[:32]


def source_fingerprint(row: Dict[str, str]) -> str:
    fields = [PROCESSING_VERSION]
    fields.extend(
        f"{key}={normalized(value)}" for key, value in sorted(row.items())
    )
    return digest_text(*fields)


def load_catalog(path: Path) -> List[CatalogEntry]:
    candidates = generator.load_csv_candidates(path)
    raw_rows: List[Dict[str, str]] = []
    import csv

    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(
            line for line in handle if line.strip() and not line.lstrip().startswith("#")
        )
        for raw in reader:
            raw_rows.append(
                {key.strip().lower(): (value or "").strip() for key, value in raw.items() if key}
            )
    if len(raw_rows) != len(candidates):
        raise ValueError("CSV parser disagreement while building fallback pack")
    entries: List[CatalogEntry] = []
    seen = set()
    for row, candidate in zip(raw_rows, candidates):
        stable_id = stable_id_for(row)
        if stable_id in seen:
            raise ValueError(f"Duplicate fallback id: {stable_id}")
        seen.add(stable_id)
        entries.append(
            CatalogEntry(
                stable_id=stable_id,
                name=row.get("name", "") or row.get("title", ""),
                artist=row.get("artist", ""),
                album=row.get("album", ""),
                source_fingerprint=source_fingerprint(row),
                candidate=candidate,
            )
        )
    return entries


def parse_pack(data: bytes) -> ParsedPack:
    if len(data) < HEADER_BYTES:
        raise ValueError("Pack is shorter than its header")
    magic, version, width, height, count, metadata_length = HEADER.unpack_from(data)
    if magic != MAGIC or version != FORMAT_VERSION or width != 64 or height != 64:
        raise ValueError("Unsupported fallback pack header")
    metadata_end = HEADER_BYTES + metadata_length
    expected = metadata_end + count * generator.FRAME_SIZE_BYTES
    if metadata_length < 2 or expected != len(data):
        raise ValueError("Fallback pack length does not match its header")
    metadata = json.loads(data[HEADER_BYTES:metadata_end].decode("utf-8"))
    if not isinstance(metadata, dict) or not isinstance(metadata.get("entries"), list):
        raise ValueError("Fallback pack metadata is invalid")
    if len(metadata["entries"]) != count:
        raise ValueError("Fallback metadata count does not match the header")
    frames = [
        data[metadata_end + index * generator.FRAME_SIZE_BYTES : metadata_end + (index + 1) * generator.FRAME_SIZE_BYTES]
        for index in range(count)
    ]
    return ParsedPack(metadata=metadata, frames=frames)


def reusable_frames(previous: Optional[ParsedPack]) -> Dict[Tuple[str, str], bytes]:
    if previous is None:
        return {}
    result: Dict[Tuple[str, str], bytes] = {}
    entries = previous.metadata.get("entries", [])
    assert isinstance(entries, list)
    for index, raw in enumerate(entries):
        if not isinstance(raw, dict):
            continue
        stable_id = raw.get("id")
        fingerprint = raw.get("source_fingerprint")
        if isinstance(stable_id, str) and isinstance(fingerprint, str):
            result[(stable_id, fingerprint)] = previous.frames[index]
    return result


def resolve_frame(
    entry: CatalogEntry,
    lookup_cache: Dict[str, object],
    artwork_cache: Path,
) -> bytes:
    url, _source = generator.resolve_artwork_url(entry.candidate, lookup_cache)
    if not url:
        raise ValueError("no artwork resolver returned a URL")
    image = generator.load_image_from_url(url, artwork_cache)
    processed = generator.process_image(image)
    frame = generator.image_to_rgb565_le(processed)
    if len(frame) != generator.FRAME_SIZE_BYTES:
        raise ValueError(f"invalid frame length {len(frame)}")
    return frame


def build_pack(
    catalog: Iterable[CatalogEntry],
    frame_resolver: Callable[[CatalogEntry], bytes],
    previous: Optional[ParsedPack] = None,
) -> Tuple[bytes, Dict[str, object]]:
    catalog_entries = list(catalog)
    reusable = reusable_frames(previous)
    packed_entries: List[PackedEntry] = []
    frames: List[bytes] = []
    skipped: List[Dict[str, str]] = []
    reused = 0
    generated = 0
    for entry in catalog_entries:
        cache_key = (entry.stable_id, entry.source_fingerprint)
        frame = reusable.get(cache_key)
        if frame is not None:
            reused += 1
        else:
            try:
                frame = frame_resolver(entry)
                generated += 1
            except Exception as exc:  # each bad new row is intentionally isolated
                skipped.append({"id": entry.stable_id, "name": entry.name, "error": str(exc)})
                continue
        if len(frame) != generator.FRAME_SIZE_BYTES:
            skipped.append(
                {"id": entry.stable_id, "name": entry.name, "error": "invalid frame length"}
            )
            continue
        packed_entries.append(
            PackedEntry(
                id=entry.stable_id,
                name=entry.name,
                artist=entry.artist,
                album=entry.album,
                source_fingerprint=entry.source_fingerprint,
            )
        )
        frames.append(frame)
    if not frames:
        raise ValueError("Refusing to publish an empty fallback pack")

    catalog_hash = digest_text(*(entry.id + entry.source_fingerprint for entry in packed_entries))
    metadata = {
        "catalog_hash": catalog_hash,
        "processing_version": PROCESSING_VERSION,
        "entries": [asdict(entry) for entry in packed_entries],
    }
    metadata_bytes = json.dumps(
        metadata, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    header = HEADER.pack(
        MAGIC,
        FORMAT_VERSION,
        generator.DISPLAY_SIZE,
        generator.DISPLAY_SIZE,
        len(frames),
        len(metadata_bytes),
    )
    pack = header + metadata_bytes + b"".join(frames)
    if len(pack) > MAX_PACK_BYTES:
        raise ValueError(f"Pack is {len(pack)} bytes; maximum is {MAX_PACK_BYTES}")
    parse_pack(pack)  # final structural verification
    report = {
        "input_count": len(catalog_entries),
        "packed_count": len(frames),
        "reused_count": reused,
        "generated_count": generated,
        "skipped_count": len(skipped),
        "skipped": skipped,
    }
    return pack, report


def download_optional(url: str) -> Optional[bytes]:
    if not url:
        return None
    try:
        request = urllib.request.Request(url, headers={"User-Agent": generator.DEFAULT_USER_AGENT})
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.read()
    except Exception as exc:
        print(f"Previous pack unavailable; rebuilding changed entries: {exc}", file=sys.stderr)
        return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", type=Path, default=Path("data/albums.csv"))
    parser.add_argument("--output-dir", type=Path, default=Path("pack-output"))
    parser.add_argument("--previous-pack", type=Path)
    parser.add_argument("--previous-pack-url", default=os.environ.get("PREVIOUS_PACK_URL", ""))
    args = parser.parse_args()

    previous_bytes = args.previous_pack.read_bytes() if args.previous_pack else None
    if previous_bytes is None:
        previous_bytes = download_optional(args.previous_pack_url)
    previous = None
    if previous_bytes:
        try:
            previous = parse_pack(previous_bytes)
        except (ValueError, json.JSONDecodeError, UnicodeDecodeError) as exc:
            print(f"Previous pack is corrupt; rebuilding entries: {exc}", file=sys.stderr)

    cache_dir = Path(os.environ.get("CACHE_DIR", ".cache"))
    lookup_path = cache_dir / "lookup.json"
    lookup_cache = generator.load_lookup_cache(lookup_path)
    catalog = load_catalog(args.csv)

    def resolver(entry: CatalogEntry) -> bytes:
        print(f"Generating {entry.name} — {entry.artist}", file=sys.stderr)
        return resolve_frame(entry, lookup_cache, cache_dir / "artwork")

    pack, report = build_pack(catalog, resolver, previous)
    digest = hashlib.sha256(pack).hexdigest()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    pack_name = f"fallback-{digest}.acpk"
    (args.output_dir / pack_name).write_bytes(pack)
    parsed = parse_pack(pack)
    metadata_length = len(
        json.dumps(
            parsed.metadata, ensure_ascii=False, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
    )
    generated_at = int(time.time())
    manifest = {
        "api_version": 1,
        "format_version": FORMAT_VERSION,
        "version": digest,
        "sha256": digest,
        "size": len(pack),
        "count": len(parsed.frames),
        "metadata_length": metadata_length,
        "pack_key": f"fallback/packs/{digest}.acpk",
        "pack_url": f"/v1/fallback/packs/{digest}.acpk",
        "generated_at": generated_at,
        "skipped_count": report["skipped_count"],
        "report_key": f"fallback/reports/{digest}.json",
    }
    (args.output_dir / "current.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8"
    )
    report.update({"version": digest, "generated_at": generated_at, "pack_file": pack_name})
    (args.output_dir / "report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8"
    )
    generator.save_lookup_cache(lookup_path, lookup_cache)
    print(json.dumps({"manifest": manifest, "report": report}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
