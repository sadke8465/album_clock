#pragma once

// ---------------------------------------------------------------------------
// Frame source
// ---------------------------------------------------------------------------
// Firmware derives the service origin from this compatibility URL, confirms
// /v1/state, then uses the versioned state/frame protocol. After three v1
// failures it safely falls back to polling this legacy frame URL.
//
// Two valid sources exist:
//   1. Cloudflare Worker (RECOMMENDED, ~1 min latency):
//        https://albumclock.<your-subdomain>.workers.dev/frame.rgb565
//   2. GitHub Pages static backup (~10 min latency):
//        https://sadke8465.github.io/album_clock/frame.rgb565
//
// Default points at the deployed Worker for fast now-playing. The Pages
// backup (https://sadke8465.github.io/album_clock/frame.rgb565) can be set from
// the portal if the Worker is ever unavailable.
#define DEFAULT_FRAME_URL "https://albumclock.sadke8465.workers.dev/frame.rgb565"

// Access-point name shown for first-time setup and when BOOT is held at power-on.
#define WIFI_SETUP_AP_NAME "AlbumClock-Setup"
#define WIFI_SETUP_AP_PASSWORD ""  // open network; set a password to lock it

// mDNS hostname for the persistent config portal: http://albumclock.local
#define MDNS_HOSTNAME "albumclock"

#define FIRMWARE_VERSION "3.0.0"

// ---------------------------------------------------------------------------
// Frame + timing
// ---------------------------------------------------------------------------
#define FRAME_WIDTH 64
#define FRAME_HEIGHT 64
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 2)  // 8192
#define STATE_POLL_INTERVAL_MS 2000UL
#define LEGACY_POLL_INTERVAL_MS 10000UL
#define HTTP_TIMEOUT_MS 5000
#define STATE_HTTP_TIMEOUT_MS 9000
#define PACK_HTTP_TIMEOUT_MS 15000
#define FALLBACK_ROTATION_MS (10UL * 60UL * 1000UL)
#define MANUAL_FALLBACK_HOLD_MS (10UL * 60UL * 1000UL)
#define TRANSITION_FRAME_MS 33UL

// Cached frame path on the on-flash LittleFS partition (cold-boot without Wi-Fi).
#define FRAME_CACHE_PATH "/frame.bin"
#define FRAME_CACHE_TEMP_PATH "/frame.tmp"
#define FALLBACK_PACK_A_PATH "/fallback-a.acpk"
#define FALLBACK_PACK_B_PATH "/fallback-b.acpk"

// Hold BOOT / GPIO0 low at power-on to (re)enter the Wi-Fi setup portal.
#define RESET_CONFIG_PIN 0

// ---------------------------------------------------------------------------
// Brightness — tuned for dim indoor viewing. 1..255, adjustable in the portal.
// ---------------------------------------------------------------------------
#define DEFAULT_BRIGHTNESS 40

// Panel channel order, adjustable live in the portal (with an on-panel test
// card). 0=RGB 1=RBG 2=GRB 3=GBR 4=BRG 5=BGR. Frame data stays true RGB565 (web
// preview correct); the order is applied only when drawing to the panel.
// This Waveshare panel is verified as RBG.
#define DEFAULT_COLOR_ORDER 1

// ---------------------------------------------------------------------------
// Cover transition — all settings below are live-adjustable in the portal; the
// values here are just the power-on defaults.
//   method   0=fade through black  1=crossfade  2=slide/push
//   pattern  0=lum highlights-lead 1=lum highlights-linger 2=uniform
//            3=radial out 4=radial in 5=wipe> 6=wipe< 7=wipe v 8=wipe ^
//            9=diagonal 10=random
//   easing   0=linear 1=quadIn 2=quadOut 3=quadInOut 4=quartIn 5=quartOut
//            6=quartInOut 7=expoIn 8=expoOut 9=expoInOut
//   slideDir 0=left 1=right 2=up 3=down
// ---------------------------------------------------------------------------
#define DEFAULT_FADE_ENABLED 1
#define DEFAULT_TRANSITION_MS 3000  // total out+hold+in
#define DEFAULT_FADE_HOLD_MS 0      // black beat between old and new
#define DEFAULT_FADE_METHOD 0
#define DEFAULT_PATTERN_OUT 1       // highlights linger on the way out
#define DEFAULT_PATTERN_IN 0        // highlights lead on the way in
#define DEFAULT_EASE_OUT 4          // quart in  (stays bright, drops late)
#define DEFAULT_EASE_IN 5           // quart out (pops in fast)
#define DEFAULT_SLIDE_DIR 0
#define DEFAULT_BALANCE_PCT 50      // out/in split of the non-hold time
#define DEFAULT_SPREAD_PCT 60       // stagger (0=uniform, ~90=very)
#define DEFAULT_SOFT_PCT 35         // per-pixel ramp softness
#define DEFAULT_JITTER_PCT 15       // breaks up banding
#define DEFAULT_DESAT_PCT 30        // color bleeds to gray as it dims
#define DEFAULT_FADE_GAMMA 1        // perceptual dimming

// ---------------------------------------------------------------------------
// Verified Waveshare ESP32-S3-RGB-Matrix HUB75 GPIO map (do not change).
// ---------------------------------------------------------------------------
#define MATRIX_R1_PIN 4
#define MATRIX_G1_PIN 5
#define MATRIX_B1_PIN 6
#define MATRIX_R2_PIN 7
#define MATRIX_G2_PIN 15
#define MATRIX_B2_PIN 16
#define MATRIX_A_PIN 18
#define MATRIX_B_PIN 8
#define MATRIX_C_PIN 3
#define MATRIX_D_PIN 42
#define MATRIX_E_PIN 9
#define MATRIX_LAT_PIN 40
#define MATRIX_OE_PIN 2
#define MATRIX_CLK_PIN 41
