#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>

#include "AlbumClockConfig.h"

#if __has_include("LocalSecrets.h")
#include "LocalSecrets.h"
#endif

MatrixPanel_I2S_DMA *display = nullptr;
Preferences prefs;

static uint16_t frame[FRAME_WIDTH * FRAME_HEIGHT];
static String frameUrl = DEFAULT_FRAME_URL;
static uint8_t brightness = DEFAULT_BRIGHTNESS;
static unsigned long lastFetchAttempt = 0;
static bool hasFrame = false;

void printCentered(const char *text, int16_t y, uint16_t color) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  display->setTextSize(1);
  display->setTextColor(color);
  display->getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display->setCursor(max(0, (FRAME_WIDTH - static_cast<int>(w)) / 2), y);
  display->print(text);
}

void showStatus(const char *line1, const char *line2, uint16_t color) {
  if (!display) {
    return;
  }
  display->fillScreen(0);
  display->drawRect(0, 0, FRAME_WIDTH, FRAME_HEIGHT, color);
  printCentered(line1, 20, color);
  printCentered(line2, 34, color);
}

void showProgressPattern() {
  for (uint8_t y = 0; y < FRAME_HEIGHT; y++) {
    for (uint8_t x = 0; x < FRAME_WIDTH; x++) {
      uint8_t r = x * 4;
      uint8_t g = y * 4;
      uint8_t b = ((x + y) / 2) * 4;
      uint16_t color = display->color565(r, g, b);
      display->drawPixel(x, y, color);
    }
  }
}

void setupMatrix() {
  HUB75_I2S_CFG mxconfig(FRAME_WIDTH, FRAME_HEIGHT, 1);
  mxconfig.gpio.r1 = MATRIX_R1_PIN;
  mxconfig.gpio.g1 = MATRIX_G1_PIN;
  mxconfig.gpio.b1 = MATRIX_B1_PIN;
  mxconfig.gpio.r2 = MATRIX_R2_PIN;
  mxconfig.gpio.g2 = MATRIX_G2_PIN;
  mxconfig.gpio.b2 = MATRIX_B2_PIN;
  mxconfig.gpio.a = MATRIX_A_PIN;
  mxconfig.gpio.b = MATRIX_B_PIN;
  mxconfig.gpio.c = MATRIX_C_PIN;
  mxconfig.gpio.d = MATRIX_D_PIN;
  mxconfig.gpio.e = MATRIX_E_PIN;
  mxconfig.gpio.lat = MATRIX_LAT_PIN;
  mxconfig.gpio.oe = MATRIX_OE_PIN;
  mxconfig.gpio.clk = MATRIX_CLK_PIN;
  mxconfig.clkphase = false;

  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  display->setBrightness8(brightness);
  display->clearScreen();
}

void loadSettings() {
  prefs.begin("album-clock", false);
  frameUrl = prefs.getString("frame_url", DEFAULT_FRAME_URL);
  brightness = prefs.getUChar("brightness", DEFAULT_BRIGHTNESS);
  brightness = constrain(brightness, 1, 255);
}

void saveSettings(const String &newFrameUrl, uint8_t newBrightness) {
  frameUrl = newFrameUrl.length() > 0 ? newFrameUrl : DEFAULT_FRAME_URL;
  brightness = constrain(newBrightness, 1, 255);
  prefs.putString("frame_url", frameUrl);
  prefs.putUChar("brightness", brightness);
  display->setBrightness8(brightness);
}

void maybeResetSettings() {
  pinMode(RESET_CONFIG_PIN, INPUT_PULLUP);
  delay(40);
  if (digitalRead(RESET_CONFIG_PIN) == LOW) {
    prefs.clear();
    frameUrl = DEFAULT_FRAME_URL;
    brightness = DEFAULT_BRIGHTNESS;
    WiFiManager wm;
    wm.resetSettings();
    showStatus("RESET", "CONFIG", display->color565(255, 180, 0));
    delay(1200);
  }
}

void connectWifiAndConfigure() {
  WiFi.mode(WIFI_STA);

#ifdef LOCAL_WIFI_SSID
  String localFrameUrl = frameUrl;
#ifdef LOCAL_FRAME_URL
  localFrameUrl = LOCAL_FRAME_URL;
#endif
  saveSettings(localFrameUrl, brightness);

  showStatus("WIFI", "JOIN", display->color565(80, 180, 255));
  WiFi.begin(LOCAL_WIFI_SSID, LOCAL_WIFI_PASSWORD);
  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 30000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    showStatus("WIFI", "READY", display->color565(40, 255, 120));
    delay(600);
    return;
  }
  showStatus("WIFI", "PORTAL", display->color565(255, 180, 0));
#endif

  char urlBuffer[256];
  char brightnessBuffer[8];
  frameUrl.toCharArray(urlBuffer, sizeof(urlBuffer));
  snprintf(brightnessBuffer, sizeof(brightnessBuffer), "%u", brightness);

  WiFiManagerParameter frameUrlParam("frame_url", "frame.rgb565 URL", urlBuffer, sizeof(urlBuffer));
  WiFiManagerParameter brightnessParam("brightness", "Brightness 1-255", brightnessBuffer, sizeof(brightnessBuffer));

  WiFiManager wm;
  wm.setConfigPortalTimeout(240);
  wm.addParameter(&frameUrlParam);
  wm.addParameter(&brightnessParam);

  showStatus("WIFI", "SETUP", display->color565(80, 180, 255));
  bool connected;
  if (strlen(WIFI_SETUP_AP_PASSWORD) > 0) {
    connected = wm.autoConnect(WIFI_SETUP_AP_NAME, WIFI_SETUP_AP_PASSWORD);
  } else {
    connected = wm.autoConnect(WIFI_SETUP_AP_NAME);
  }

  if (!connected) {
    showStatus("WIFI", "FAILED", display->color565(255, 30, 30));
    delay(2000);
    ESP.restart();
  }

  uint8_t parsedBrightness = static_cast<uint8_t>(atoi(brightnessParam.getValue()));
  if (parsedBrightness == 0) {
    parsedBrightness = DEFAULT_BRIGHTNESS;
  }
  saveSettings(String(frameUrlParam.getValue()), parsedBrightness);
  showStatus("WIFI", "READY", display->color565(40, 255, 120));
  delay(600);
}

bool readFrameFromStream(Stream *stream) {
  uint8_t *target = reinterpret_cast<uint8_t *>(frame);
  size_t total = 0;
  unsigned long lastProgress = millis();

  while (total < FRAME_BYTES && millis() - lastProgress < HTTP_TIMEOUT_MS) {
    int available = stream->available();
    if (available <= 0) {
      delay(10);
      continue;
    }

    size_t remaining = FRAME_BYTES - total;
    size_t chunk = min(static_cast<size_t>(available), remaining);
    int readNow = stream->readBytes(target + total, chunk);
    if (readNow > 0) {
      total += readNow;
      lastProgress = millis();
    }
  }

  return total == FRAME_BYTES;
}

bool fetchFrame() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  bool began = false;

  if (frameUrl.startsWith("https://")) {
    secureClient.setInsecure();
    began = http.begin(secureClient, frameUrl);
  } else {
    began = http.begin(plainClient, frameUrl);
  }

  if (!began) {
    Serial.println("HTTP begin failed");
    return false;
  }

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("GET failed: %d\n", code);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength > 0 && contentLength != FRAME_BYTES) {
    Serial.printf("Bad content length: %d\n", contentLength);
    http.end();
    return false;
  }

  bool ok = readFrameFromStream(http.getStreamPtr());
  http.end();

  if (ok) {
    display->drawRGBBitmap(0, 0, frame, FRAME_WIDTH, FRAME_HEIGHT);
    hasFrame = true;
  }
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  loadSettings();
  setupMatrix();
  maybeResetSettings();
  showProgressPattern();
  delay(500);
  connectWifiAndConfigure();

  showStatus("FETCH", "FRAME", display->color565(120, 180, 255));
  bool ok = fetchFrame();
  if (!ok) {
    showStatus("FETCH", "FAILED", display->color565(255, 60, 40));
  }
  lastFetchAttempt = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(2000);
  }

  if (millis() - lastFetchAttempt >= FETCH_INTERVAL_MS || (!hasFrame && millis() - lastFetchAttempt >= 30000)) {
    lastFetchAttempt = millis();
    bool ok = fetchFrame();
    if (!ok && !hasFrame) {
      showStatus("NO", "FRAME", display->color565(255, 60, 40));
    }
  }

  delay(100);
}
