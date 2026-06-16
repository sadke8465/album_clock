#!/usr/bin/env python3
import csv
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageOps, ImageStat


DISPLAY_SIZE = 64
FRAME_SIZE_BYTES = DISPLAY_SIZE * DISPLAY_SIZE * 2
DEFAULT_USER_AGENT = "album-matrix-clock/1.0 (https://github.com/sadke8465/album_clock)"


@dataclass
class Candidate:
    mode: str
    title: str = ""
    artist: str = ""
    album: str = ""
    image_url: str = ""
    apple_track_id: str = ""
    apple_album_id: str = ""
    musicbrainz_release_group: str = ""
    source_detail: str = ""


def env_bool(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def request_json(url: str, timeout: int = 20, headers: Optional[Dict[str, str]] = None) -> Dict[str, Any]:
    request_headers = {"User-Agent": os.environ.get("USER_AGENT", DEFAULT_USER_AGENT)}
    if headers:
        request_headers.update(headers)
    req = urllib.request.Request(url, headers=request_headers)
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def request_bytes(url: str, timeout: int = 30) -> bytes:
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": os.environ.get("USER_AGENT", DEFAULT_USER_AGENT),
            "Accept": "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return response.read()


def load_lookup_cache(path: Path) -> Dict[str, Any]:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def save_lookup_cache(path: Path, cache: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cache, indent=2, sort_keys=True), encoding="utf-8")


def pick_lastfm_image(images: Any) -> str:
    if not isinstance(images, list):
        return ""
    for item in reversed(images):
        url = item.get("#text", "") if isinstance(item, dict) else ""
        if url and "2a96cbd8b46e442fc41c2b86b821562f" not in url:
            return url
    return ""


def get_lastfm_now_playing() -> Optional[Candidate]:
    api_key = os.environ.get("LAST_FM_API_KEY", "").strip()
    username = os.environ.get("LAST_FM_USER", "").strip()
    if not api_key or not username:
        return None

    params = urllib.parse.urlencode(
        {
            "method": "user.getrecenttracks",
            "user": username,
            "api_key": api_key,
            "format": "json",
            "limit": "1",
        }
    )
    data = request_json(f"https://ws.audioscrobbler.com/2.0/?{params}")
    tracks = data.get("recenttracks", {}).get("track", [])
    if isinstance(tracks, dict):
        tracks = [tracks]
    if not tracks:
        return None

    track = tracks[0]
    attrs = track.get("@attr", {})
    if not isinstance(attrs, dict) or attrs.get("nowplaying") != "true":
        return None

    artist = track.get("artist", {}).get("#text", "") if isinstance(track.get("artist"), dict) else ""
    album = track.get("album", {}).get("#text", "") if isinstance(track.get("album"), dict) else ""
    title = track.get("name", "")
    image_url = pick_lastfm_image(track.get("image"))
    return Candidate(
        mode="lastfm",
        title=title,
        artist=artist,
        album=album,
        image_url=image_url,
        source_detail="Last.fm now-playing",
    )


def load_csv_candidates(path: Path) -> List[Candidate]:
    if not path.exists():
        return []

    rows: List[Candidate] = []
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(
            line for line in handle if line.strip() and not line.lstrip().startswith("#")
        )
        for raw in reader:
            row = {k.strip().lower(): (v or "").strip() for k, v in raw.items() if k}
            apple_url = row.get("url", "")
            apple_id = row.get("id", "")
            mode = row.get("type") or ("image_url" if row.get("value", "").startswith("http") else "search")
            value = row.get("value", "")
            if apple_id and apple_url.startswith("https://music.apple.com/"):
                mode = "apple_track_id"
            rows.append(
                Candidate(
                    mode=mode,
                    title=row.get("title", "") or row.get("name", ""),
                    artist=row.get("artist", ""),
                    album=row.get("album", ""),
                    image_url=row.get("image_url", "") or (value if mode == "image_url" else ""),
                    apple_track_id=row.get("apple_track_id", "") or (value if mode == "apple_track_id" else "") or apple_id,
                    apple_album_id=row.get("apple_album_id", "") or (value if mode == "apple_album_id" else ""),
                    musicbrainz_release_group=row.get("musicbrainz_release_group", "")
                    or (value if mode in {"musicbrainz_release_group", "mbid"} else ""),
                    source_detail="CSV fallback",
                )
            )
    return rows


def select_csv_candidate(rows: List[Candidate]) -> Optional[Candidate]:
    if not rows:
        return None
    rotation_seconds = int(os.environ.get("ROTATION_SECONDS", "600"))
    index = int(time.time() // max(rotation_seconds, 1)) % len(rows)
    return rows[index]


def cache_key(prefix: str, *parts: str) -> str:
    joined = "\x1f".join(part.strip().lower() for part in parts if part)
    return f"{prefix}:{hashlib.sha256(joined.encode('utf-8')).hexdigest()}"


def cover_art_for_release_group(mbid: str) -> str:
    if not mbid:
        return ""
    data = request_json(f"https://coverartarchive.org/release-group/{urllib.parse.quote(mbid)}", timeout=20)
    images = data.get("images", [])
    if not isinstance(images, list):
        return ""

    preferred = [img for img in images if isinstance(img, dict) and img.get("front")]
    for image in preferred + images:
        thumbnails = image.get("thumbnails", {}) if isinstance(image, dict) else {}
        if isinstance(thumbnails, dict):
            for size in ("500", "250", "large", "small"):
                if thumbnails.get(size):
                    return thumbnails[size]
        if isinstance(image, dict) and image.get("image"):
            return image["image"]
    return ""


def musicbrainz_cover_url(artist: str, album: str, cache: Dict[str, Any]) -> str:
    if not artist or not album:
        return ""
    key = cache_key("musicbrainz", artist, album)
    if key in cache:
        return cache[key].get("image_url", "")

    query = f'artist:"{artist}" AND releasegroup:"{album}"'
    params = urllib.parse.urlencode({"query": query, "type": "album", "fmt": "json", "limit": "5"})
    url = f"https://musicbrainz.org/ws/2/release-group/?{params}"
    data = request_json(url, timeout=20)
    groups = data.get("release-groups", [])
    image_url = ""
    for group in groups:
        mbid = group.get("id", "")
        try:
            image_url = cover_art_for_release_group(mbid)
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, json.JSONDecodeError):
            image_url = ""
        if image_url:
            break

    cache[key] = {"image_url": image_url, "updated_at": int(time.time())}
    return image_url


def itunes_artwork_url(candidate: Candidate, cache: Dict[str, Any]) -> str:
    if not env_bool("ENABLE_ITUNES_FALLBACK", True):
        return ""

    key = cache_key(
        "itunes",
        candidate.apple_track_id or candidate.apple_album_id or candidate.artist,
        candidate.album,
        candidate.title,
    )
    if key in cache:
        return cache[key].get("image_url", "")

    if candidate.apple_track_id:
        params = urllib.parse.urlencode({"id": candidate.apple_track_id, "limit": "1"})
        url = f"https://itunes.apple.com/lookup?{params}"
    elif candidate.apple_album_id:
        params = urllib.parse.urlencode({"id": candidate.apple_album_id, "entity": "album", "limit": "1"})
        url = f"https://itunes.apple.com/lookup?{params}"
    else:
        terms = " ".join(part for part in [candidate.artist, candidate.album or candidate.title] if part)
        if not terms:
            return ""
        params = urllib.parse.urlencode({"term": terms, "media": "music", "entity": "album", "limit": "5"})
        url = f"https://itunes.apple.com/search?{params}"

    data = request_json(url, timeout=20)
    image_url = ""
    for item in data.get("results", []):
        art = item.get("artworkUrl100", "")
        if art:
            image_url = art.replace("100x100bb", "1000x1000bb").replace("100x100", "1000x1000")
            break

    cache[key] = {"image_url": image_url, "updated_at": int(time.time())}
    return image_url


def resolve_artwork_url(candidate: Candidate, cache: Dict[str, Any]) -> Tuple[str, str]:
    if candidate.image_url:
        return candidate.image_url, "direct image"

    if candidate.musicbrainz_release_group:
        try:
            url = cover_art_for_release_group(candidate.musicbrainz_release_group)
            if url:
                return url, "Cover Art Archive release-group"
        except Exception as exc:
            print(f"Cover Art Archive lookup failed: {exc}", file=sys.stderr)

    if candidate.artist and candidate.album:
        try:
            url = musicbrainz_cover_url(candidate.artist, candidate.album, cache)
            if url:
                return url, "MusicBrainz + Cover Art Archive"
        except Exception as exc:
            print(f"MusicBrainz lookup failed: {exc}", file=sys.stderr)

    try:
        url = itunes_artwork_url(candidate, cache)
        if url:
            return url, "Apple iTunes Search API"
    except Exception as exc:
        print(f"iTunes lookup failed: {exc}", file=sys.stderr)

    return "", "generated fallback"


def load_image_from_url(url: str, cache_dir: Path) -> Image.Image:
    cache_dir.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(url.encode("utf-8")).hexdigest()
    cached_path = cache_dir / digest
    if cached_path.exists():
        data = cached_path.read_bytes()
    else:
        data = request_bytes(url)
        cached_path.write_bytes(data)
    return ImageOps.exif_transpose(Image.open(BytesIO(data))).convert("RGB")


def generated_fallback_image(text: str = "NO ART") -> Image.Image:
    img = Image.new("RGB", (DISPLAY_SIZE, DISPLAY_SIZE), (8, 9, 12))
    draw = ImageDraw.Draw(img)
    for y in range(DISPLAY_SIZE):
        color = (8 + y // 5, 18 + y // 3, 28 + y // 2)
        draw.line((0, y, DISPLAY_SIZE, y), fill=color)
    draw.rectangle((2, 2, 61, 61), outline=(180, 210, 230))
    draw.text((8, 25), text[:8], fill=(230, 236, 240))
    return img


def smart_square_crop(img: Image.Image) -> Image.Image:
    width, height = img.size
    side = min(width, height)
    if width == height:
        return img

    edge = img.convert("L").filter(ImageFilter.FIND_EDGES)
    candidates = []
    long_axis = width if width > height else height
    max_offset = long_axis - side
    steps = 12
    for i in range(steps + 1):
        offset = round(max_offset * i / steps)
        if width > height:
            box = (offset, 0, offset + side, side)
            center_delta = abs((offset + side / 2) - width / 2) / max(width / 2, 1)
        else:
            box = (0, offset, side, offset + side)
            center_delta = abs((offset + side / 2) - height / 2) / max(height / 2, 1)
        crop = edge.crop(box)
        stat = ImageStat.Stat(crop)
        edge_score = stat.mean[0] + stat.stddev[0]
        center_bias = 1.0 - 0.18 * center_delta
        candidates.append((edge_score * center_bias, box))

    _, best_box = max(candidates, key=lambda item: item[0])
    return img.crop(best_box)


def process_image(img: Image.Image) -> Image.Image:
    img = smart_square_crop(img)
    img = img.resize((DISPLAY_SIZE, DISPLAY_SIZE), Image.Resampling.LANCZOS)
    img = ImageEnhance.Contrast(img).enhance(float(os.environ.get("CONTRAST", "1.12")))
    img = ImageEnhance.Color(img).enhance(float(os.environ.get("COLOR", "1.08")))
    img = ImageEnhance.Brightness(img).enhance(float(os.environ.get("BRIGHTNESS", "1.0")))
    return img.convert("RGB")


def image_to_rgb565_le(img: Image.Image) -> bytes:
    out = bytearray()
    for r, g, b in img.getdata():
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out.append(value & 0xFF)
        out.append((value >> 8) & 0xFF)
    return bytes(out)


def write_index(output_dir: Path, status: Dict[str, Any]) -> None:
    title = status.get("title") or status.get("album") or "Album Matrix Clock"
    html = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="refresh" content="60">
  <title>{title}</title>
  <style>
    body {{ margin: 0; min-height: 100vh; display: grid; place-items: center; background: #101318; color: #e8edf2; font-family: system-ui, sans-serif; }}
    main {{ width: min(92vw, 520px); }}
    img {{ width: 256px; height: 256px; image-rendering: pixelated; border: 1px solid #39424e; }}
    code {{ color: #9bd4ff; }}
  </style>
</head>
<body>
  <main>
    <h1>Album Matrix Clock</h1>
    <img src="preview.png?ts={status.get("generated_at", "")}" alt="Current 64x64 frame">
    <p><strong>{title}</strong></p>
    <p>{status.get("artist", "")} {status.get("album", "")}</p>
    <p>Frame URL: <code>frame.rgb565</code></p>
  </main>
</body>
</html>
"""
    (output_dir / "index.html").write_text(html, encoding="utf-8")


def main() -> int:
    csv_path = Path(os.environ.get("CSV_PATH", "data/albums.csv"))
    output_dir = Path(os.environ.get("OUTPUT_DIR", "public"))
    cache_dir = Path(os.environ.get("CACHE_DIR", ".cache"))
    lookup_cache_path = cache_dir / "lookup.json"
    output_dir.mkdir(parents=True, exist_ok=True)

    lookup_cache = load_lookup_cache(lookup_cache_path)
    status: Dict[str, Any] = {
        "generated_at": int(time.time()),
        "generated_at_iso": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "display_width": DISPLAY_SIZE,
        "display_height": DISPLAY_SIZE,
    }

    candidate = None
    try:
        candidate = get_lastfm_now_playing()
    except Exception as exc:
        status["lastfm_error"] = str(exc)
        print(f"Last.fm lookup failed: {exc}", file=sys.stderr)

    if candidate is None:
        rows = load_csv_candidates(csv_path)
        candidate = select_csv_candidate(rows)
        status["csv_rows"] = len(rows)

    if candidate is None:
        candidate = Candidate(mode="fallback", title="No source configured", source_detail="generated fallback")

    status.update(
        {
            "mode": candidate.mode,
            "title": candidate.title,
            "artist": candidate.artist,
            "album": candidate.album,
            "source_detail": candidate.source_detail,
        }
    )

    artwork_url, artwork_source = resolve_artwork_url(candidate, lookup_cache)
    status["artwork_url"] = artwork_url
    status["artwork_source"] = artwork_source

    try:
        img = load_image_from_url(artwork_url, cache_dir / "artwork") if artwork_url else generated_fallback_image()
    except Exception as exc:
        status["image_error"] = str(exc)
        print(f"Image load failed: {exc}", file=sys.stderr)
        img = generated_fallback_image("ERR")

    processed = process_image(img)
    frame = image_to_rgb565_le(processed)
    if len(frame) != FRAME_SIZE_BYTES:
        raise RuntimeError(f"Bad frame size: expected {FRAME_SIZE_BYTES}, got {len(frame)}")

    (output_dir / "frame.rgb565").write_bytes(frame)
    processed.save(output_dir / "preview.png")
    (output_dir / "status.json").write_text(json.dumps(status, indent=2, sort_keys=True), encoding="utf-8")
    write_index(output_dir, status)
    save_lookup_cache(lookup_cache_path, lookup_cache)

    print(json.dumps(status, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
