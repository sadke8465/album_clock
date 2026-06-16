# Album Matrix Clock

Cloud-generated album art for a 64x64 HUB75E LED matrix on ESP32.

The cloud side runs on GitHub Actions and GitHub Pages. It checks Last.fm now-playing first, falls back to `data/albums.csv`, generates a 64x64 preview, and publishes a raw RGB565 frame that the ESP32 displays.

## What It Publishes

After the GitHub Pages workflow runs, these files are available:

- `frame.rgb565` - 8192-byte RGB565 little-endian frame for the ESP32.
- `preview.png` - exact 64x64 preview.
- `status.json` - debug/status metadata.
- `index.html` - simple browser preview page.

## Required GitHub Settings

Use a public GitHub repo for strict-free scheduled GitHub Actions + Pages usage.

In the repo:

1. Go to `Settings -> Secrets and variables -> Actions`.
2. Add this repository secret:
   - `LAST_FM_API_KEY`: your free Last.fm API key.
3. Add this repository variable:
   - `LAST_FM_USER`: `Noamsadi95`
   - Optional but recommended: `USER_AGENT`, for example `album-matrix-clock/1.0 (your-email@example.com)`.
4. Go to `Settings -> Pages`.
5. Set `Source` to `GitHub Actions`.
6. Run `Generate matrix frame` once from the Actions tab.

Your frame URL will be:

```text
https://YOUR_GITHUB_USERNAME.github.io/YOUR_REPO/frame.rgb565
```

## CSV Fallback

Edit `data/albums.csv`.

Supported row types:

```csv
type,value,artist,album,title
image_url,https://example.com/image.jpg,,,Direct image
search,,Daft Punk,Discovery,Discovery
apple_album_id,1440857781,,,Apple album id
musicbrainz_release_group,MBID_GOES_HERE,,,MusicBrainz release group
```

Priority order:

1. Last.fm now-playing album art.
2. CSV fallback row selected by the current 10-minute slot.
3. Generated fallback image if all lookups fail.

Artwork lookup order:

1. Direct image URL.
2. MusicBrainz + Cover Art Archive.
3. Apple iTunes Search API fallback.

## Local Generator Test

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
LAST_FM_API_KEY="your-key" LAST_FM_USER="Noamsadi95" python scripts/generate_frame.py
```

Open `public/index.html` or `public/preview.png`.

## ESP32 Firmware

The firmware is in `firmware/` and is a PlatformIO Arduino project.

Flash:

```bash
cd firmware
pio run -t upload
```

After flashing:

1. The ESP32 starts an access point named `AlbumClock-Setup` if it needs configuration.
2. Join that WiFi network from your phone/computer.
3. Enter your home WiFi credentials.
4. Enter the final GitHub Pages `frame.rgb565` URL.
5. Set brightness from `1` to `255`.

To reset WiFi and URL settings, hold `BOOT` / GPIO0 while resetting the ESP32.

## Hardware Defaults

The firmware defaults to a 64x64 HUB75E panel with this pin map:

| HUB75 | ESP32 |
| --- | --- |
| R1 | GPIO25 |
| G1 | GPIO26 |
| B1 | GPIO27 |
| R2 | GPIO14 |
| G2 | GPIO12 |
| B2 | GPIO13 |
| A | GPIO23 |
| B | GPIO19 |
| C | GPIO5 |
| D | GPIO17 |
| E | GPIO18 |
| LAT | GPIO4 |
| OE | GPIO15 |
| CLK | GPIO16 |

If your matrix colors or scan order are wrong, edit `firmware/include/AlbumClockConfig.h`.

## Runtime Behavior

- ESP32 fetches `frame.rgb565` at boot.
- It refreshes every 10 minutes.
- If fetch fails, it keeps the last good frame.
- GitHub Actions regenerates the frame every 10 minutes. GitHub scheduled jobs can be delayed, so timing is approximate.
