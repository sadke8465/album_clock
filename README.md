# Album Matrix Clock

A standalone 64×64 HUB75 LED matrix that shows the album art of whatever you're
playing (via Last.fm now-playing), and rotates through a catalog of fallback
albums when nothing is playing. No clock, no captions, no audio — just artwork.

- **Controller:** Waveshare ESP32-S3-RGB-Matrix (ESP32-S3-WROOM-2 N32R16, 32 MB
  OPI flash / 16 MB OPI PSRAM).
- **Panel:** one 64×64 HUB75 RGB matrix, separately powered over USB-C.

## Architecture

```text
Last.fm ─▶ Worker ─▶ SQLite Durable Object ─▶ immutable R2 frame
                                             │
albums.csv ─▶ pack workflow ─▶ immutable R2 fallback pack
                                             │
                                             ▼
                              ESP32 network task (2 s state poll)
                                             │
                       local A/B pack + non-blocking display task
                                             │
                                             ▼
                                      64×64 HUB75

GitHub Actions + Pages ── compatibility/disaster-recovery frame
```

Two independent refresh paths produce the exact same 8192-byte RGB565 frame:

| Path | Latency | Role |
| --- | --- | --- |
| **Cloudflare Worker** (`worker/`) | 3–6 s healthy target | **Primary.** Versioned state plus immutable frames and packs. |
| **GitHub Actions → Pages** | ~10 min | Backup. Static frame, survives even if the Worker is down. |

The device fetches whichever URL you configure in its portal. Point it at the
Worker for near-real-time now-playing; the Pages URL is the safe default so it
shows art out of the box.

## Repository layout

```text
worker/                     Cloudflare Worker (primary live endpoint) — see worker/README.md
scripts/generate_frame.py   Artwork generator for the GitHub Pages backup
data/albums.csv             Fallback album catalog (shared by both paths)
tests/                      Python tests for the generator
.github/workflows/          Scheduled generator + Pages deploy
firmware/                   PlatformIO / Arduino firmware for the ESP32-S3
  include/AlbumClockConfig.h  Frame settings + verified Waveshare GPIO map
  include/PortalPage.h        Embedded web config portal
  src/main.cpp                Production firmware
  src/panel_test.cpp          Hardware diagnostic (separate build env)
```

## 1. Deploy the Cloudflare Worker (recommended)

See [`worker/README.md`](worker/README.md) for the full walkthrough. Short form:

```bash
cd worker
npm install
npx wrangler login
npx wrangler r2 bucket create albumclock-frames
npx wrangler kv namespace create ALBUMCLOCK   # paste id into wrangler.jsonc
npx wrangler secret put LAST_FM_API_KEY       # your Last.fm API key
npx wrangler deploy                           # prints your https URL
```

The portal accepts either the Worker origin or the legacy `/frame.rgb565` URL.
Firmware confirms `/v1/state`, saves the origin, and falls back to legacy polling
after three v1 failures without erasing the configuration.

## 2. GitHub Pages backup (optional but recommended)

The scheduled workflow keeps a static frame at
`https://sadke8465.github.io/album_clock/frame.rgb565` — used as the firmware's
default and as a fallback if the Worker is unavailable.

1. Repo → **Settings → Secrets and variables → Actions**
   - Secret `LAST_FM_API_KEY` = your Last.fm API key.
   - Variable `LAST_FM_USER` = `Noamsadi95`.
   - Optional variable `USER_AGENT` = e.g. `album-matrix-clock/1.0 (you@example.com)`.
2. Repo → **Settings → Pages** → **Source: GitHub Actions**.
3. Run **Generate matrix frame** once from the Actions tab.

## 3. Build & flash the firmware

Two PlatformIO environments share the verified N32R16 OPI/OPI profile:

- `waveshare-panel-test` — hardware diagnostic (`src/panel_test.cpp`).
- `waveshare-album-clock` — production firmware (`src/main.cpp`).

```bash
cd firmware
pio run -e waveshare-album-clock -t upload      # build + flash over /dev/cu.usbmodem101
pio device monitor                              # watch serial (115200)
```

If USB upload isn't detected: hold **BOOT**, reconnect USB, release BOOT, upload,
then press **RESET**.

## 4. First-time device setup

1. On first boot (no Wi-Fi saved) the device hosts an access point
   **`AlbumClock-Setup`**. Join it from your phone; a captive portal opens.
2. Enter your home Wi-Fi SSID + password, the **Frame URL** (your Worker URL),
   and brightness. Save — the device restarts and joins your network.
3. After that, the config portal is always reachable on your network at
   **`http://albumclock.local`** (or the device IP shown in the serial log).

To re-enter the setup portal later, hold **BOOT / GPIO0** while powering on.

### The portal (`http://albumclock.local`)

- **Live preview** updates only after a completed transition.
- Change Wi-Fi, Frame URL, and brightness (brightness applies instantly; a
  Wi-Fi change triggers a reconnect/restart).
- **Status/diagnostics**: playback/preparing/degraded state, local pack and
  cursor, active/queued transition, network latency, frame age, Wi-Fi, and errors.
- Buttons: **Refresh now**, **Next fallback** (instant local seek while idle),
  **Restart**. `POST /reset` clears all settings.
- **Color calibration:** a test card (R/G/B/W quadrants) plus a panel color-order
  selector to correct channel wiring live (this panel is RBG).

## Behavior & reliability

- Polls versioned state every **2 s** with ETags and downloads artwork only when
  its generation changes.
- **Atomic verified updates:** HTTP status, length, SHA-256, and generation are
  checked before a frame can begin transitioning.
- **Responsive transitions:** the 30 FPS state machine never blocks the portal;
  a song can smoothly retarget an in-progress fallback transition.
- **Offline Next:** fallback covers are read from an A/B LittleFS pack. One
  additional click is queued during a transition and immediate repeats are
  prevented across reboots.
- **Safe pack sync:** downloads resume in the inactive slot and activate only
  after header, size, dimensions, frame count, and SHA-256 validation.
- **Cold boot without Internet:** the last good frame is cached to on-flash
  LittleFS and shown immediately at power-on, before Wi-Fi connects.
- **Auto-recovery:** if Wi-Fi drops it keeps showing the last frame and
  reconnects automatically; no reflash or manual reset needed.

## Configuration reference

| What | Where | Default |
| --- | --- | --- |
| Frame URL | Portal, or `DEFAULT_FRAME_URL` in `AlbumClockConfig.h` | Pages backup URL |
| Brightness (1–255) | Portal (live) | `40` (dim indoor) |
| State poll interval | `STATE_POLL_INTERVAL_MS` | 2 s |
| Fallback rotation | `ROTATION_SECONDS` (Worker var / workflow env) | 600 s (10 min), shuffled |
| Last.fm user | Worker var `LAST_FM_USER` / repo var | `Noamsadi95` |

Artwork is shown **natural** (mild contrast/saturation only — no CRT/scanline
effects).

### Verified Waveshare HUB75 GPIO map

| HUB75 | GPIO | | HUB75 | GPIO |
| --- | ---: | --- | --- | ---: |
| R1 | 4 | | D | 42 |
| G1 | 5 | | E | 9 |
| B1 | 6 | | LAT | 40 |
| R2 | 7 | | OE | 2 |
| G2 | 15 | | CLK | 41 |
| B2 | 16 | | A | 18 |
| B | 8 | | C | 3 |

Do **not** switch the flash/PSRAM profile away from `opi_opi` / `dout` — other
combinations trigger an early boot assertion on this module.

## Troubleshooting

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| Blank screen | Panel power / ribbon | Confirm separate USB-C panel power; reseat ribbon (power off first). |
| Boot loop, blank | Wrong flash/PSRAM profile | Keep `opi_opi` + `dout` (`platformio.ini`). |
| Scrambled rows | GPIO map or scan | Compare against the pin table; run `waveshare-panel-test`. |
| Wrong colors (channels swapped) | Panel color order | Portal → Color calibration → Toggle test card, pick the order where quadrants read red/green/blue/white. This panel is **RBG** (`DEFAULT_COLOR_ORDER`). |
| Stuck on old image | Fetch failing | Portal → Status: check `last_result`; verify Frame URL returns HTTP 200 + 8192 bytes. |
| Can't reach `albumclock.local` | mDNS unsupported on your OS | Use the device IP from the serial log / router. |
| No now-playing art | Worker not deployed / wrong URL | Point Frame URL at the Worker; check `curl <worker>/status`. |
| Won't join Wi-Fi | Wrong credentials | Hold BOOT at power-on → setup portal → re-enter. |

## Local testing

```bash
# Generator (Python)
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
python -m unittest discover -s tests            # unit tests
python scripts/generate_frame.py                # writes public/ (needs network)

# Worker (Node)
cd worker && npm install
npm run typecheck && npm test
```

## Safety notes

- Disconnect power before moving the HUB75 ribbon.
- Start at low brightness; a full-white 64×64 frame is the highest-current test.
- Recovery is always possible over USB (BOOT + RESET), independent of the panel.
