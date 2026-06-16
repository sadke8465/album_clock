#pragma once

// GitHub Pages URL created by the workflow. You can also set this from the
// captive setup portal after flashing.
#define DEFAULT_FRAME_URL "https://sadke8465.github.io/album_clock/frame.rgb565"

#define WIFI_SETUP_AP_NAME "AlbumClock-Setup"
#define WIFI_SETUP_AP_PASSWORD ""

#define FRAME_WIDTH 64
#define FRAME_HEIGHT 64
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 2)
#define FETCH_INTERVAL_MS (10UL * 60UL * 1000UL)
#define HTTP_TIMEOUT_MS 15000

// Hold BOOT / GPIO0 low during reset to clear saved WiFi and URL settings.
#define RESET_CONFIG_PIN 0

// HUB75E pin map matching the common ESP32 + 64x64 Clockwise/TopYuan wiring.
#define MATRIX_R1_PIN 25
#define MATRIX_G1_PIN 26
#define MATRIX_B1_PIN 27
#define MATRIX_R2_PIN 14
#define MATRIX_G2_PIN 12
#define MATRIX_B2_PIN 13
#define MATRIX_A_PIN 23
#define MATRIX_B_PIN 19
#define MATRIX_C_PIN 5
#define MATRIX_D_PIN 17
#define MATRIX_E_PIN 18
#define MATRIX_LAT_PIN 4
#define MATRIX_OE_PIN 15
#define MATRIX_CLK_PIN 16

#define DEFAULT_BRIGHTNESS 48
