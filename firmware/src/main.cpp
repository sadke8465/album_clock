#include <Arduino.h>
#include <math.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>

#include "AlbumClockConfig.h"
#include "AudioReactive.h"
#include "PortalPage.h"

#if __has_include("LocalSecrets.h")
#include "LocalSecrets.h"
#endif

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
MatrixPanel_I2S_DMA *display = nullptr;
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

static uint16_t frame[FRAME_WIDTH * FRAME_HEIGHT];  // current displayed frame
static uint8_t staging[FRAME_BYTES];                // atomic download buffer
static uint16_t transitionFrom[FRAME_WIDTH * FRAME_HEIGHT];
static uint16_t transitionTo[FRAME_WIDTH * FRAME_HEIGHT];
static uint16_t compositeFrame[FRAME_WIDTH * FRAME_HEIGHT];

static String wifiSsid;
static String wifiPass;
static String frameUrl = DEFAULT_FRAME_URL;
static uint8_t brightness = DEFAULT_BRIGHTNESS;

static uint8_t colorOrder = DEFAULT_COLOR_ORDER;
static bool testCard = false;
static bool fadeEnabled = DEFAULT_FADE_ENABLED;
static uint16_t transitionMs = DEFAULT_TRANSITION_MS;
static uint16_t holdMs = DEFAULT_FADE_HOLD_MS;
static uint8_t fadeMethod = DEFAULT_FADE_METHOD;
static uint8_t patOut = DEFAULT_PATTERN_OUT;
static uint8_t patIn = DEFAULT_PATTERN_IN;
static uint8_t easeOut = DEFAULT_EASE_OUT;
static uint8_t easeIn = DEFAULT_EASE_IN;
static uint8_t slideDir = DEFAULT_SLIDE_DIR;
static uint8_t balancePct = DEFAULT_BALANCE_PCT;
static uint8_t spreadPct = DEFAULT_SPREAD_PCT;
static uint8_t softPct = DEFAULT_SOFT_PCT;
static uint8_t jitterPct = DEFAULT_JITTER_PCT;
static uint8_t desatPct = DEFAULT_DESAT_PCT;
static bool gammaOn = DEFAULT_FADE_GAMMA;
static uint8_t fieldOld[FRAME_WIDTH * FRAME_HEIGHT];
static uint8_t fieldNew[FRAME_WIDTH * FRAME_HEIGHT];
static float gammaLUT[256];
static uint8_t displayMode = 0;  // 0 = album art, 1 = audio reactive
static bool audioReady = false;
static bool audioFailed = false;
static bool apMode = false;
static bool serverStarted = false;
static bool hasFrame = false;
static unsigned long lastReconnectAttempt = 0;
static int lastByteCount = 0;
static String lastResult = "none";

extern const uint8_t rootca_crt_bundle_start[]
    asm("_binary_data_cert_x509_crt_bundle_bin_start");

enum class NetworkCommandType : uint8_t { Refresh };
struct NetworkCommand { NetworkCommandType type; };
enum class NetworkEventType : uint8_t { State, Frame, Pack, Error };
struct NetworkEvent {
  NetworkEventType type;
  uint32_t generation;
  bool playing;
  uint32_t latencyMs;
  uint8_t *data;
  char message[96];
};

struct RemoteSnapshot {
  uint32_t revision = 0;
  uint32_t generation = 0;
  bool playing = false;
  String playback = "unknown";
  String serviceStatus = "starting";
  String frameHash;
  String framePath;
  String pendingTitle;
  String packVersion;
  String packPath;
  uint32_t packSize = 0;
  uint32_t packMetadataLength = 0;
  uint16_t packCount = 0;
  uint32_t retryAfterMs = STATE_POLL_INTERVAL_MS;
};

struct LocalPackState {
  bool valid = false;
  char slot = 'a';
  String version;
  uint32_t size = 0;
  uint32_t metadataLength = 0;
  uint16_t count = 0;
  uint16_t cursor = 0;
  uint16_t lastIndex = UINT16_MAX;
  uint16_t *order = nullptr;
};

struct TransitionState {
  bool active = false;
  bool haveOld = false;
  uint32_t generation = 0;
  uint32_t startedAt = 0;
  uint32_t lastDrawAt = 0;
};

static RemoteSnapshot remoteState;
static LocalPackState localPack;
static TransitionState transition;
static QueueHandle_t networkCommands = nullptr;
static QueueHandle_t networkEvents = nullptr;
static SemaphoreHandle_t fsMutex = nullptr;
static TaskHandle_t networkTaskHandle = nullptr;
static volatile bool playbackActive = false;
static volatile bool networkBusy = false;
static bool nextQueued = false;
static bool cachePending = false;
static uint32_t displayGeneration = 0;
static uint32_t nextDisplayGeneration = 0;
static uint32_t lastRemoteGeneration = 0;
static volatile uint32_t latestRequestedGeneration = 0;
static uint32_t lastNetworkLatencyMs = 0;
static uint32_t lastStateAt = 0;
static uint32_t lastFrameAt = 0;
static uint32_t manualHoldUntil = 0;
static uint32_t lastFallbackAt = 0;
static uint8_t v1Failures = 0;
static bool legacyMode = false;
static String serviceOrigin;

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------
// Reorder true (r,g,b) into the panel's wiring order, then draw with the same
// primitive the verified panel_test used (drawPixelRGB888).
void drawTrue(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t o0 = r, o1 = g, o2 = b;
  switch (colorOrder) {
    case 1: o0 = r; o1 = b; o2 = g; break;  // RBG
    case 2: o0 = g; o1 = r; o2 = b; break;  // GRB
    case 3: o0 = g; o1 = b; o2 = r; break;  // GBR
    case 4: o0 = b; o1 = r; o2 = g; break;  // BRG
    case 5: o0 = b; o1 = g; o2 = r; break;  // BGR
    default: break;                         // 0 = RGB
  }
  display->drawPixelRGB888(x, y, o0, o1, o2);
}

void drawTestCard() {
  // Quadrants of true color: TL red, TR green, BL blue, BR white.
  for (int y = 0; y < FRAME_HEIGHT; y++) {
    for (int x = 0; x < FRAME_WIDTH; x++) {
      uint8_t r = 0, g = 0, b = 0;
      bool top = y < 32, left = x < 32;
      if (top && left) r = 255;
      else if (top && !left) g = 255;
      else if (!top && left) b = 255;
      else { r = g = b = 255; }
      drawTrue(x, y, r, g, b);
    }
  }
}

void drawFrame() {
  if (!display) return;
  if (testCard) {
    drawTestCard();
    return;
  }
  for (int y = 0; y < FRAME_HEIGHT; y++) {
    for (int x = 0; x < FRAME_WIDTH; x++) {
      uint16_t v = frame[y * FRAME_WIDTH + x];
      uint8_t r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
      // Expand to 8-bit per channel.
      uint8_t r = (r5 << 3) | (r5 >> 2);
      uint8_t g = (g6 << 2) | (g6 >> 4);
      uint8_t b = (b5 << 3) | (b5 >> 2);
      drawTrue(x, y, r, g, b);
    }
  }
}

void showBootPattern() {
  if (!display) return;
  for (int y = 0; y < FRAME_HEIGHT; y++) {
    for (int x = 0; x < FRAME_WIDTH; x++) {
      display->drawPixelRGB888(x, y, x, y, (x + y) / 2);
    }
  }
}

// ---------------------------------------------------------------------------
// Cover transition engine — method / pattern / easing all runtime-configurable
// ---------------------------------------------------------------------------
static inline uint8_t luma8(uint16_t v) {
  uint8_t r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
  uint8_t r = (r5 << 3) | (r5 >> 2);
  uint8_t g = (g6 << 2) | (g6 >> 4);
  uint8_t b = (b5 << 3) | (b5 >> 2);
  return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);  // ~0.299/0.587/0.114
}

static inline float hashf(int x, int y) {
  uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
  h = (h ^ (h >> 13)) * 1274126177u;
  return (float)(h & 0xFFFFFF) / (float)0xFFFFFF;
}

void initGammaLUT() {
  for (int i = 0; i < 256; i++) gammaLUT[i] = powf(i / 255.0f, 2.2f);
}

// Per-pixel stagger field (0..255) for a pattern, with optional jitter.
void computeField(const uint16_t *buf, uint8_t *field, uint8_t pattern) {
  float jit = jitterPct / 100.0f;
  const float cx = (FRAME_WIDTH - 1) / 2.0f, cy = (FRAME_HEIGHT - 1) / 2.0f;
  const float maxr = sqrtf(cx * cx + cy * cy);
  for (int y = 0; y < FRAME_HEIGHT; y++) {
    for (int x = 0; x < FRAME_WIDTH; x++) {
      int i = y * FRAME_WIDTH + x;
      float v;
      switch (pattern) {
        case 1: v = luma8(buf[i]) / 255.0f; break;                          // highlights linger
        case 2: v = 0; break;                                               // uniform
        case 3: { float dx = x - cx, dy = y - cy; v = sqrtf(dx * dx + dy * dy) / maxr; break; }        // radial out
        case 4: { float dx = x - cx, dy = y - cy; v = 1.0f - sqrtf(dx * dx + dy * dy) / maxr; break; } // radial in
        case 5: v = (float)x / (FRAME_WIDTH - 1); break;                    // wipe >
        case 6: v = 1.0f - (float)x / (FRAME_WIDTH - 1); break;             // wipe <
        case 7: v = (float)y / (FRAME_HEIGHT - 1); break;                   // wipe v
        case 8: v = 1.0f - (float)y / (FRAME_HEIGHT - 1); break;            // wipe ^
        case 9: v = (float)(x + y) / (FRAME_WIDTH + FRAME_HEIGHT - 2); break;  // diagonal
        case 10: v = hashf(x, y); break;                                    // random
        default: v = 1.0f - luma8(buf[i]) / 255.0f; break;                  // 0 highlights lead
      }
      if (jit > 0) {
        v += (hashf(x * 3 + 1, y * 7 + 5) - 0.5f) * jit;
        if (v < 0) v = 0; else if (v > 1) v = 1;
      }
      field[i] = (uint8_t)(v * 255.0f);
    }
  }
}

// Staggered per-pixel ramp normalized to [0,1]: earliest pixel starts at p=0,
// latest finishes exactly at p=1 (full duration used, no dead black tail).
static inline float framp(float p, float fnorm, float s, float w) {
  float v = (p * (s + w) - fnorm * s) / w;
  return v < 0 ? 0 : (v > 1 ? 1 : v);
}

// Easing curves (0=linear .. 9=expoInOut). Quad/quart use multiplies; only
// expo needs a transcendental.
float easef(uint8_t e, float t) {
  if (t < 0) t = 0; else if (t > 1) t = 1;
  switch (e) {
    case 1: return t * t;
    case 2: { float u = 1 - t; return 1 - u * u; }
    case 3: { if (t < 0.5f) return 2 * t * t; float u = -2 * t + 2; return 1 - u * u / 2; }
    case 4: return t * t * t * t;
    case 5: { float u = 1 - t; return 1 - u * u * u * u; }
    case 6: { if (t < 0.5f) return 8 * t * t * t * t; float u = -2 * t + 2; return 1 - u * u * u * u / 2; }
    case 7: return t <= 0 ? 0 : exp2f(10 * t - 10);
    case 8: return t >= 1 ? 1 : 1 - exp2f(-10 * t);
    case 9: if (t <= 0) return 0; if (t >= 1) return 1;
            return t < 0.5f ? exp2f(20 * t - 10) / 2 : (2 - exp2f(-20 * t + 10)) / 2;
    default: return t;
  }
}

static inline float gcorr(float b) {
  if (!gammaOn) return b;
  int idx = (int)(b * 255.0f);
  if (idx < 0) idx = 0; else if (idx > 255) idx = 255;
  return gammaLUT[idx];
}

// Draw `v` (RGB565) at brightness `bri`, desaturating as it dims, through
// drawTrue so the panel color order applies.
void drawScaled(int x, int y, uint16_t v, float bri) {
  if (bri <= 0.0f) { display->drawPixelRGB888(x, y, 0, 0, 0); return; }
  uint8_t r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
  float r = (r5 << 3) | (r5 >> 2);
  float g = (g6 << 2) | (g6 >> 4);
  float b = (b5 << 3) | (b5 >> 2);
  float gray = 0.299f * r + 0.587f * g + 0.114f * b;
  float m = (1.0f - bri) * (desatPct / 100.0f);
  float d = gcorr(bri);
  drawTrue(x, y, (uint8_t)((r * (1 - m) + gray * m) * d),
           (uint8_t)((g * (1 - m) + gray * m) * d),
           (uint8_t)((b * (1 - m) + gray * m) * d));
}

// Weighted blend of two RGB565 pixels (crossfade).
void drawBlend(int x, int y, uint16_t vo, uint16_t vn, float ow, float nw) {
  uint8_t or5 = (vo >> 11) & 0x1F, og6 = (vo >> 5) & 0x3F, ob5 = vo & 0x1F;
  uint8_t nr5 = (vn >> 11) & 0x1F, ng6 = (vn >> 5) & 0x3F, nb5 = vn & 0x1F;
  float r = ((or5 << 3) | (or5 >> 2)) * ow + ((nr5 << 3) | (nr5 >> 2)) * nw;
  float g = ((og6 << 2) | (og6 >> 4)) * ow + ((ng6 << 2) | (ng6 >> 4)) * nw;
  float b = ((ob5 << 3) | (ob5 >> 2)) * ow + ((nb5 << 3) | (nb5 >> 2)) * nw;
  if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
  drawTrue(x, y, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}

void drawFinal(const uint16_t *buf) {
  for (int y = 0; y < FRAME_HEIGHT; y++)
    for (int x = 0; x < FRAME_WIDTH; x++)
      drawScaled(x, y, buf[y * FRAME_WIDTH + x], 1.0f);
}

static uint16_t scaled565(uint16_t value, float amount) {
  amount = amount < 0 ? 0 : (amount > 1 ? 1 : amount);
  uint8_t r5 = (value >> 11) & 0x1F, g6 = (value >> 5) & 0x3F, b5 = value & 0x1F;
  float r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
  float gray = 0.299f * r + 0.587f * g + 0.114f * b;
  float mix = (1.0f - amount) * (desatPct / 100.0f), dim = gcorr(amount);
  uint8_t ro = (uint8_t)((r * (1 - mix) + gray * mix) * dim);
  uint8_t go = (uint8_t)((g * (1 - mix) + gray * mix) * dim);
  uint8_t bo = (uint8_t)((b * (1 - mix) + gray * mix) * dim);
  return ((ro & 0xF8) << 8) | ((go & 0xFC) << 3) | (bo >> 3);
}

static uint16_t blended565(uint16_t oldValue, uint16_t newValue, float oldWeight, float newWeight) {
  uint8_t or5 = (oldValue >> 11) & 31, og6 = (oldValue >> 5) & 63, ob5 = oldValue & 31;
  uint8_t nr5 = (newValue >> 11) & 31, ng6 = (newValue >> 5) & 63, nb5 = newValue & 31;
  uint8_t r = min(255, (int)(((or5 << 3) | (or5 >> 2)) * oldWeight + ((nr5 << 3) | (nr5 >> 2)) * newWeight));
  uint8_t g = min(255, (int)(((og6 << 2) | (og6 >> 4)) * oldWeight + ((ng6 << 2) | (ng6 >> 4)) * newWeight));
  uint8_t b = min(255, (int)(((ob5 << 3) | (ob5 >> 2)) * oldWeight + ((nb5 << 3) | (nb5 >> 2)) * newWeight));
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void draw565(int x, int y, uint16_t value) {
  uint8_t r5 = (value >> 11) & 31, g6 = (value >> 5) & 63, b5 = value & 31;
  drawTrue(x, y, (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2));
}

bool beginNextFallback(bool manual);

void startTransition(const uint16_t *target, uint32_t generation) {
  if (!display || !target) return;
  if (transition.active && memcmp(transitionTo, target, FRAME_BYTES) == 0) return;
  if (!transition.active && hasFrame && memcmp(frame, target, FRAME_BYTES) == 0) return;

  if (transition.active) memcpy(transitionFrom, compositeFrame, FRAME_BYTES);
  else if (hasFrame) memcpy(transitionFrom, frame, FRAME_BYTES);
  else memset(transitionFrom, 0, FRAME_BYTES);
  memcpy(transitionTo, target, FRAME_BYTES);
  memcpy(compositeFrame, transitionFrom, FRAME_BYTES);
  transition.haveOld = hasFrame || transition.active;
  transition.active = fadeEnabled;
  transition.generation = generation;
  transition.startedAt = millis();
  transition.lastDrawAt = 0;
  computeField(transitionFrom, fieldOld, patOut);
  computeField(transitionTo, fieldNew, patIn);

  if (!fadeEnabled) {
    memcpy(frame, transitionTo, FRAME_BYTES);
    memcpy(compositeFrame, frame, FRAME_BYTES);
    hasFrame = true;
    displayGeneration = generation;
    drawFrame();
    cachePending = true;
  }
}

void serviceTransition() {
  if (!transition.active || displayMode != 0) return;
  uint32_t now = millis();
  if (transition.lastDrawAt && now - transition.lastDrawAt < TRANSITION_FRAME_MS) return;
  transition.lastDrawAt = now;
  float total = max(1.0f, (float)(transition.haveOld ? transitionMs : transitionMs / 2));
  float elapsed = (float)(now - transition.startedAt);
  float progress = min(1.0f, elapsed / total);
  float spread = spreadPct / 100.0f, soft = softPct / 100.0f;

  for (int y = 0; y < FRAME_HEIGHT; y++) {
    for (int x = 0; x < FRAME_WIDTH; x++) {
      int i = y * FRAME_WIDTH + x;
      uint16_t value;
      if (!transition.haveOld) {
        float in = easef(easeIn, framp(progress, fieldNew[i] / 255.0f, spread, soft));
        value = scaled565(transitionTo[i], in);
      } else if (fadeMethod == 2) {
        int shift = (int)(easef(easeIn, progress) * FRAME_WIDTH + 0.5f);
        int sx = x, sy = y;
        const uint16_t *source;
        if (slideDir == 0) { if (x < FRAME_WIDTH - shift) { source = transitionFrom; sx = x + shift; } else { source = transitionTo; sx = x - (FRAME_WIDTH - shift); } }
        else if (slideDir == 1) { if (x >= shift) { source = transitionFrom; sx = x - shift; } else { source = transitionTo; sx = x + (FRAME_WIDTH - shift); } }
        else if (slideDir == 2) { if (y < FRAME_HEIGHT - shift) { source = transitionFrom; sy = y + shift; } else { source = transitionTo; sy = y - (FRAME_HEIGHT - shift); } }
        else { if (y >= shift) { source = transitionFrom; sy = y - shift; } else { source = transitionTo; sy = y + (FRAME_HEIGHT - shift); } }
        value = source[sy * FRAME_WIDTH + sx];
      } else if (fadeMethod == 1) {
        float local = framp(progress, fieldOld[i] / 255.0f, spread, soft);
        value = blended565(transitionFrom[i], transitionTo[i], 1.0f - easef(easeOut, local), easef(easeIn, local));
      } else {
        float nonHold = max(1.0f, total - (float)holdMs);
        float outEnd = max(1.0f, (balancePct / 100.0f) * nonHold);
        float inStart = outEnd + holdMs;
        float inDuration = max(1.0f, total - inStart);
        if (elapsed < outEnd) value = scaled565(transitionFrom[i], 1.0f - easef(easeOut, framp(elapsed / outEnd, fieldOld[i] / 255.0f, spread, soft)));
        else if (elapsed < inStart) value = 0;
        else value = scaled565(transitionTo[i], easef(easeIn, framp((elapsed - inStart) / inDuration, fieldNew[i] / 255.0f, spread, soft)));
      }
      compositeFrame[i] = value;
      if (!testCard) draw565(x, y, value);
    }
  }

  if (progress >= 1.0f) {
    memcpy(frame, transitionTo, FRAME_BYTES);
    memcpy(compositeFrame, frame, FRAME_BYTES);
    transition.active = false;
    hasFrame = true;
    displayGeneration = transition.generation;
    cachePending = true;
    if (!testCard) drawFinal(frame);
    if (nextQueued && !playbackActive) {
      nextQueued = false;
      beginNextFallback(false);
    }
  }
}

// ---------------------------------------------------------------------------
// Audio-reactive mode — minimalist 16-band spectrum
// ---------------------------------------------------------------------------
void renderSpectrum() {
  static float peak[AUD_BANDS] = {0};
  float bands[AUD_BANDS];
  audio::readBands(bands);
  for (int b = 0; b < AUD_BANDS; b++) {
    int h = (int)(bands[b] * FRAME_HEIGHT + 0.5f);
    if (h > FRAME_HEIGHT) h = FRAME_HEIGHT;
    if (peak[b] < bands[b]) peak[b] = bands[b]; else peak[b] *= 0.94f;
    int ph = (int)(peak[b] * (FRAME_HEIGHT - 1) + 0.5f);
    int x0 = b * 4;
    for (int dx = 0; dx < 4; dx++) {
      int x = x0 + dx;
      bool gap = (dx == 3);  // 1px gap between bars
      for (int y = 0; y < FRAME_HEIGHT; y++) {
        int fromBottom = FRAME_HEIGHT - 1 - y;
        uint8_t r = 0, g = 0, bl = 0;
        if (!gap) {
          if (fromBottom == ph) { r = g = bl = 255; }  // peak cap
          else if (fromBottom < h) {
            float t = (float)fromBottom / FRAME_HEIGHT;  // color by height
            r = (uint8_t)(40 + 215 * t);
            g = (uint8_t)(40 + 160 * (1 - t));
            bl = (uint8_t)(30 + 220 * (1 - t));
          }
        }
        drawTrue(x, y, r, g, bl);
      }
    }
  }
}

void serviceAudio() {
  if (!audioReady && !audioFailed) {
    if (audio::begin()) {
      audioReady = true;
      Serial.println("Audio: ES7210 + I2S up");
    } else {
      audioFailed = true;
      Serial.println("Audio: mic init failed (ES7210 not responding)");
    }
  }
  if (audioReady) {
    renderSpectrum();
  } else {
    // Mic unavailable: gentle idle sweep so the mode is visibly active.
    static uint8_t t = 0;
    t += 2;
    for (int y = 0; y < FRAME_HEIGHT; y++)
      for (int x = 0; x < FRAME_WIDTH; x++) {
        int d = (x + y + t) & 63;
        uint8_t v = d < 32 ? d : 63 - d;
        drawTrue(x, y, 0, v / 3, v / 2);
      }
    delay(30);
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
  // Match the verified panel_test diagnostic exactly. The driver/speed profile
  // determines how color bits are latched — the wrong one scrambles channels.
  mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_16M;
  mxconfig.driver = HUB75_I2S_CFG::SHIFTREG;

  display = new MatrixPanel_I2S_DMA(mxconfig);
  if (!display->begin()) {
    Serial.println("FATAL: matrix DMA init failed");
  }
  display->setBrightness8(brightness);
  display->clearScreen();
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------
void loadSettings() {
  prefs.begin("album-clock", false);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  frameUrl = prefs.getString("url", DEFAULT_FRAME_URL);
  brightness = prefs.getUChar("bright", DEFAULT_BRIGHTNESS);
  if (brightness < 1) brightness = DEFAULT_BRIGHTNESS;
  colorOrder = prefs.getUChar("colororder", DEFAULT_COLOR_ORDER);
  if (colorOrder > 5) colorOrder = 0;
  fadeEnabled = prefs.getUChar("fade", DEFAULT_FADE_ENABLED) != 0;
  transitionMs = prefs.getUShort("fadems", DEFAULT_TRANSITION_MS);
  transitionMs = constrain(transitionMs, 500, 6000);
  holdMs = prefs.getUShort("holdms", DEFAULT_FADE_HOLD_MS);
  holdMs = constrain(holdMs, 0, 2000);
  fadeMethod = prefs.getUChar("fmethod", DEFAULT_FADE_METHOD);
  patOut = prefs.getUChar("patout", DEFAULT_PATTERN_OUT);
  patIn = prefs.getUChar("patin", DEFAULT_PATTERN_IN);
  easeOut = prefs.getUChar("easeout", DEFAULT_EASE_OUT);
  easeIn = prefs.getUChar("easein", DEFAULT_EASE_IN);
  slideDir = prefs.getUChar("slidedir", DEFAULT_SLIDE_DIR);
  balancePct = constrain(prefs.getUChar("balance", DEFAULT_BALANCE_PCT), 10, 90);
  spreadPct = constrain(prefs.getUChar("spread", DEFAULT_SPREAD_PCT), 0, 95);
  softPct = constrain(prefs.getUChar("soft", DEFAULT_SOFT_PCT), 5, 95);
  jitterPct = constrain(prefs.getUChar("jitter", DEFAULT_JITTER_PCT), 0, 80);
  desatPct = constrain(prefs.getUChar("desat", DEFAULT_DESAT_PCT), 0, 100);
  gammaOn = prefs.getUChar("gamma", DEFAULT_FADE_GAMMA) != 0;
  displayMode = prefs.getUChar("mode", 0);
  if (displayMode > 1) displayMode = 0;

#ifdef LOCAL_WIFI_SSID
  // Optional compile-time override for development (LocalSecrets.h, untracked).
  wifiSsid = LOCAL_WIFI_SSID;
  wifiPass = LOCAL_WIFI_PASSWORD;
#endif
#ifdef LOCAL_FRAME_URL
  frameUrl = LOCAL_FRAME_URL;
#endif
}

bool isValidFrameUrl(const String &url) {
  return url.startsWith("http://") || url.startsWith("https://");
}

// ---------------------------------------------------------------------------
// Persistent frame cache (LittleFS) — survives power cycles without Internet
// ---------------------------------------------------------------------------
bool loadCachedFrame() {
  if (!LittleFS.exists(FRAME_CACHE_PATH)) return false;
  File f = LittleFS.open(FRAME_CACHE_PATH, "r");
  if (!f) return false;
  bool ok = false;
  if (f.size() == FRAME_BYTES) {
    ok = f.read(reinterpret_cast<uint8_t *>(frame), FRAME_BYTES) == FRAME_BYTES;
  }
  f.close();
  return ok;
}

void persistFrame() {
  if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(250)) != pdTRUE) return;
  File f = LittleFS.open(FRAME_CACHE_TEMP_PATH, "w");
  if (!f) { if (fsMutex) xSemaphoreGive(fsMutex); return; }
  size_t written = f.write(reinterpret_cast<uint8_t *>(frame), FRAME_BYTES);
  f.flush();
  f.close();
  if (written == FRAME_BYTES) {
    LittleFS.remove(FRAME_CACHE_PATH);
    LittleFS.rename(FRAME_CACHE_TEMP_PATH, FRAME_CACHE_PATH);
  } else {
    LittleFS.remove(FRAME_CACHE_TEMP_PATH);
  }
  if (fsMutex) xSemaphoreGive(fsMutex);
}

// ---------------------------------------------------------------------------
// Versioned state protocol + verified local fallback packs
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct PackHeader {
  char magic[4];
  uint16_t version;
  uint16_t width;
  uint16_t height;
  uint16_t count;
  uint32_t metadataLength;
};
#pragma pack(pop)
static_assert(sizeof(PackHeader) == 16, "fallback pack header must be 16 bytes");

struct PackInstallInfo {
  char slot;
  char version[65];
  uint32_t size;
  uint32_t metadataLength;
  uint16_t count;
};

String originFromUrl(const String &url) {
  int scheme = url.indexOf("://");
  if (scheme < 0) return "";
  int slash = url.indexOf('/', scheme + 3);
  return slash < 0 ? url : url.substring(0, slash);
}

String absoluteServiceUrl(const String &path) {
  if (path.startsWith("http://") || path.startsWith("https://")) return path;
  return serviceOrigin + (path.startsWith("/") ? path : "/" + path);
}

static int jsonValueAt(const String &json, const char *key, int from = 0) {
  String needle = "\"" + String(key) + "\"";
  int at = json.indexOf(needle, from);
  if (at < 0) return -1;
  at = json.indexOf(':', at + needle.length());
  if (at < 0) return -1;
  do { at++; } while (at < (int)json.length() && isspace((unsigned char)json[at]));
  return at;
}

static String jsonStringAt(const String &json, const char *key, int from = 0) {
  int at = jsonValueAt(json, key, from);
  if (at < 0 || json[at] != '"') return "";
  String out;
  bool escaped = false;
  for (int i = at + 1; i < (int)json.length(); i++) {
    char c = json[i];
    if (escaped) { out += c; escaped = false; }
    else if (c == '\\') escaped = true;
    else if (c == '"') break;
    else out += c;
  }
  return out;
}

static uint32_t jsonUIntAt(const String &json, const char *key, int from = 0) {
  int at = jsonValueAt(json, key, from);
  return at < 0 ? 0 : strtoul(json.c_str() + at, nullptr, 10);
}

bool parseRemoteSnapshot(const String &json, RemoteSnapshot &out) {
  if (jsonUIntAt(json, "api_version") != 1) return false;
  out.revision = jsonUIntAt(json, "revision");
  out.generation = jsonUIntAt(json, "generation");
  out.playback = jsonStringAt(json, "playback");
  out.playing = out.playback == "playing";
  out.serviceStatus = jsonStringAt(json, "service_status");
  out.retryAfterMs = max((uint32_t)500, jsonUIntAt(json, "retry_after_ms"));
  int frameAt = json.indexOf("\"frame\"");
  if (frameAt >= 0) {
    out.frameHash = jsonStringAt(json, "sha256", frameAt);
    out.framePath = jsonStringAt(json, "url", frameAt);
  }
  int pendingAt = json.indexOf("\"pending\"");
  if (pendingAt >= 0) out.pendingTitle = jsonStringAt(json, "title", pendingAt);
  int fallbackAt = json.indexOf("\"fallback\"");
  if (fallbackAt >= 0) {
    out.packVersion = jsonStringAt(json, "version", fallbackAt);
    out.packPath = jsonStringAt(json, "pack_url", fallbackAt);
    out.packSize = jsonUIntAt(json, "size", fallbackAt);
    out.packCount = (uint16_t)jsonUIntAt(json, "count", fallbackAt);
    out.packMetadataLength = jsonUIntAt(json, "metadata_length", fallbackAt);
  }
  return out.revision > 0;
}

String sha256Hex(const uint8_t *bytes, size_t length) {
  uint8_t digest[32];
  mbedtls_sha256_ret(bytes, length, digest, 0);
  static const char hex[] = "0123456789abcdef";
  char result[65];
  for (int i = 0; i < 32; i++) { result[i * 2] = hex[digest[i] >> 4]; result[i * 2 + 1] = hex[digest[i] & 15]; }
  result[64] = 0;
  return String(result);
}

bool beginHttp(HTTPClient &http, WiFiClient &plain, WiFiClientSecure &secure, const String &url) {
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (url.startsWith("https://")) {
    secure.setCACertBundle(rootca_crt_bundle_start);
    return http.begin(secure, url);
  }
  return http.begin(plain, url);
}

bool fetchExactFrame(const String &url, const String &expectedHash, uint8_t *output, String &error) {
  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  if (!beginHttp(http, plain, secure, url)) { error = "frame begin failed"; return false; }
  http.setTimeout(HTTP_TIMEOUT_MS);
  int code = http.GET();
  if (code != HTTP_CODE_OK || (http.getSize() >= 0 && http.getSize() != FRAME_BYTES)) {
    error = "frame HTTP " + String(code) + " len " + String(http.getSize()); http.end(); return false;
  }
  size_t read = http.getStreamPtr()->readBytes(output, FRAME_BYTES);
  bool extra = http.getStreamPtr()->available() > 0;
  http.end();
  if (read != FRAME_BYTES || extra) { error = "partial frame " + String(read); return false; }
  if (expectedHash.length() == 64 && sha256Hex(output, FRAME_BYTES) != expectedHash) {
    error = "frame checksum mismatch"; return false;
  }
  return true;
}

bool sha256File(const char *path, String &result) {
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts_ret(&context, 0);
  uint8_t buffer[1024];
  while (file.available()) {
    size_t count = file.read(buffer, sizeof(buffer));
    if (!count) { file.close(); mbedtls_sha256_free(&context); return false; }
    mbedtls_sha256_update_ret(&context, buffer, count);
  }
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&context, digest);
  mbedtls_sha256_free(&context);
  file.close();
  static const char hex[] = "0123456789abcdef";
  char value[65];
  for (int i = 0; i < 32; i++) { value[i * 2] = hex[digest[i] >> 4]; value[i * 2 + 1] = hex[digest[i] & 15]; }
  value[64] = 0; result = value;
  return true;
}

bool validatePackFile(const char *path, const RemoteSnapshot &state, PackHeader &header, String &error) {
  File file = LittleFS.open(path, "r");
  if (!file) { error = "pack open failed"; return false; }
  uint32_t size = file.size();
  bool read = file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) == sizeof(header);
  file.close();
  uint64_t expected = sizeof(PackHeader) + (uint64_t)header.metadataLength + (uint64_t)header.count * FRAME_BYTES;
  if (!read || memcmp(header.magic, "ACPK", 4) || header.version != 1 || header.width != FRAME_WIDTH ||
      header.height != FRAME_HEIGHT || !header.count || expected != size || size != state.packSize ||
      header.metadataLength != state.packMetadataLength || header.count != state.packCount || size > 8 * 1024 * 1024UL) {
    error = "pack structure invalid"; return false;
  }
  String digest;
  if (!sha256File(path, digest) || digest != state.packVersion) { error = "pack checksum mismatch"; return false; }
  return true;
}

bool downloadFallbackPack(const RemoteSnapshot &state, PackInstallInfo &installed, String &error) {
  if (state.packVersion.length() != 64 || !state.packSize || !state.packCount) return false;
  char inactive = localPack.valid && localPack.slot == 'a' ? 'b' : 'a';
  const char *finalPath = inactive == 'a' ? FALLBACK_PACK_A_PATH : FALLBACK_PACK_B_PATH;
  String partialPath = String(finalPath) + ".part";
  if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) { error = "filesystem busy"; return false; }
  File output = LittleFS.open(partialPath, "a");
  uint32_t offset = output ? output.size() : 0;
  if (offset > state.packSize) { if (output) output.close(); LittleFS.remove(partialPath); output = LittleFS.open(partialPath, "w"); offset = 0; }
  if (!output) { if (fsMutex) xSemaphoreGive(fsMutex); error = "pack file open failed"; return false; }
  if (fsMutex) xSemaphoreGive(fsMutex);

  if (offset == state.packSize) {
    output.close();
    PackHeader completed{};
    if (validatePackFile(partialPath.c_str(), state, completed, error)) {
      if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) { error = "filesystem busy"; return false; }
      LittleFS.remove(finalPath);
      bool renamed = LittleFS.rename(partialPath, finalPath);
      if (fsMutex) xSemaphoreGive(fsMutex);
      if (!renamed) { error = "pack activation rename failed"; return false; }
      installed.slot = inactive;
      strlcpy(installed.version, state.packVersion.c_str(), sizeof(installed.version));
      installed.size = state.packSize;
      installed.metadataLength = completed.metadataLength;
      installed.count = completed.count;
      return true;
    }
    error = "";
    if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) { error = "filesystem busy"; return false; }
    LittleFS.remove(partialPath);
    output = LittleFS.open(partialPath, "w");
    offset = 0;
    if (fsMutex) xSemaphoreGive(fsMutex);
    if (!output) { error = "pack file open failed"; return false; }
  }

  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  String url = absoluteServiceUrl(state.packPath);
  if (!beginHttp(http, plain, secure, url)) { output.close(); error = "pack begin failed"; return false; }
  http.setTimeout(PACK_HTTP_TIMEOUT_MS);
  if (offset) http.addHeader("Range", "bytes=" + String(offset) + "-");
  int code = http.GET();
  if ((offset && code != HTTP_CODE_PARTIAL_CONTENT) || (!offset && code != HTTP_CODE_OK)) {
    http.end(); output.close(); error = "pack HTTP " + String(code); return false;
  }
  Stream *stream = http.getStreamPtr();
  uint8_t buffer[1024];
  uint32_t lastDataAt = millis();
  uint32_t chunkEnd = min(state.packSize, offset + (uint32_t)(256UL * 1024UL));
  uint32_t chunkStartedAt = millis();
  while (offset < chunkEnd && millis() - lastDataAt < PACK_HTTP_TIMEOUT_MS &&
         millis() - chunkStartedAt < 750) {
    int available = stream->available();
    if (available <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    size_t wanted = min((uint32_t)sizeof(buffer), chunkEnd - offset);
    size_t count = stream->readBytes(buffer, min((int)wanted, available));
    if (count) {
      if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(100)) != pdTRUE) { error = "filesystem busy"; break; }
      bool wrote = output.write(buffer, count) == count;
      if (fsMutex) xSemaphoreGive(fsMutex);
      if (!wrote) { error = "pack flash write failed"; break; }
      offset += count; lastDataAt = millis();
    }
  }
  http.end();
  if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) { output.close(); error = "filesystem busy"; return false; }
  output.flush(); output.close();
  if (fsMutex) xSemaphoreGive(fsMutex);
  PackHeader header{};
  bool valid = error.length() == 0 && offset == state.packSize && validatePackFile(partialPath.c_str(), state, header, error);
  if (valid) {
    if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) { error = "filesystem busy"; return false; }
    LittleFS.remove(finalPath);
    valid = LittleFS.rename(partialPath, finalPath);
    if (fsMutex) xSemaphoreGive(fsMutex);
    if (!valid) error = "pack activation rename failed";
  }
  if (!valid) return false;
  installed.slot = inactive;
  strlcpy(installed.version, state.packVersion.c_str(), sizeof(installed.version));
  installed.size = state.packSize;
  installed.metadataLength = header.metadataLength;
  installed.count = header.count;
  return true;
}

void pushNetworkEvent(NetworkEventType type, uint32_t generation, bool playing, uint32_t latency,
                      uint8_t *data, const String &message) {
  NetworkEvent event{type, generation, playing, latency, data, {0}};
  strlcpy(event.message, message.c_str(), sizeof(event.message));
  if (xQueueSend(networkEvents, &event, pdMS_TO_TICKS(50)) != pdTRUE && data) {
    if (type == NetworkEventType::State) delete reinterpret_cast<RemoteSnapshot *>(data);
    else if (type == NetworkEventType::Pack) delete reinterpret_cast<PackInstallInfo *>(data);
    else free(data);
  }
}

void networkTask(void *) {
  uint32_t lastPollAt = 0, lastLegacyAt = 0, fetchedGeneration = 0;
  String etag, knownPack = localPack.version;
  for (;;) {
    NetworkCommand command;
    bool forced = xQueueReceive(networkCommands, &command, pdMS_TO_TICKS(50)) == pdTRUE;
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }
    uint32_t now = millis();
    if (!forced && !legacyMode && now - lastPollAt < STATE_POLL_INTERVAL_MS) continue;
    if (!forced && legacyMode && now - lastLegacyAt < LEGACY_POLL_INTERVAL_MS) continue;
    networkBusy = true;
    uint32_t requestStarted = millis();

    if (legacyMode) {
      lastLegacyAt = now;
      uint8_t *bytes = (uint8_t *)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_8BIT);
      String error;
      String legacyUrl = originFromUrl(frameUrl) == frameUrl ? frameUrl + "/frame.rgb565" : frameUrl;
      if (bytes && fetchExactFrame(legacyUrl, "", bytes, error)) {
        pushNetworkEvent(NetworkEventType::Frame, ++fetchedGeneration, false, millis() - requestStarted, bytes, "legacy frame ready");
      } else { if (bytes) free(bytes); pushNetworkEvent(NetworkEventType::Error, 0, false, millis() - requestStarted, nullptr, error); }
      networkBusy = false;
      continue;
    }

    lastPollAt = now;
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String stateUrl = serviceOrigin + "/v1/state";
    if (!beginHttp(http, plain, secure, stateUrl)) {
      pushNetworkEvent(NetworkEventType::Error, 0, false, millis() - requestStarted, nullptr, "state begin failed");
      networkBusy = false; continue;
    }
    const char *keys[] = {"ETag"};
    http.collectHeaders(keys, 1);
    http.setTimeout(STATE_HTTP_TIMEOUT_MS);
    if (etag.length()) http.addHeader("If-None-Match", etag);
    int code = http.GET();
    if (code == HTTP_CODE_NOT_MODIFIED) {
      v1Failures = 0;
      pushNetworkEvent(NetworkEventType::State, fetchedGeneration, playbackActive, millis() - requestStarted, nullptr, "not modified");
      http.end(); networkBusy = false; continue;
    }
    if (code != HTTP_CODE_OK) {
      http.end();
      if (++v1Failures >= 3) legacyMode = true;
      pushNetworkEvent(NetworkEventType::Error, 0, playbackActive, millis() - requestStarted, nullptr, "state HTTP " + String(code));
      networkBusy = false; continue;
    }
    String json = http.getString();
    String newEtag = http.header("ETag");
    http.end();
    RemoteSnapshot parsed;
    if (!parseRemoteSnapshot(json, parsed)) {
      if (++v1Failures >= 3) legacyMode = true;
      pushNetworkEvent(NetworkEventType::Error, 0, playbackActive, millis() - requestStarted, nullptr, "invalid v1 state");
      networkBusy = false; continue;
    }
    v1Failures = 0; etag = newEtag;
    RemoteSnapshot *snapshot = new RemoteSnapshot(parsed);
    pushNetworkEvent(NetworkEventType::State, parsed.generation, parsed.playing, millis() - requestStarted,
                     reinterpret_cast<uint8_t *>(snapshot), "state ready");

    if (parsed.playing && parsed.generation > fetchedGeneration && parsed.framePath.length() && parsed.frameHash.length() == 64) {
      latestRequestedGeneration = parsed.generation;
      uint8_t *bytes = (uint8_t *)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_8BIT);
      String error;
      if (bytes && fetchExactFrame(absoluteServiceUrl(parsed.framePath), parsed.frameHash, bytes, error)) {
        if (parsed.generation >= latestRequestedGeneration) {
          fetchedGeneration = parsed.generation;
          pushNetworkEvent(NetworkEventType::Frame, parsed.generation, true, millis() - requestStarted, bytes, "verified frame");
        } else free(bytes);
      } else { if (bytes) free(bytes); pushNetworkEvent(NetworkEventType::Error, parsed.generation, true, millis() - requestStarted, nullptr, error); }
    }

    if (parsed.packVersion.length() == 64 && parsed.packVersion != knownPack) {
      PackInstallInfo *info = new PackInstallInfo{};
      String error;
      if (downloadFallbackPack(parsed, *info, error)) {
        knownPack = parsed.packVersion;
        pushNetworkEvent(NetworkEventType::Pack, 0, parsed.playing, millis() - requestStarted,
                         reinterpret_cast<uint8_t *>(info), "fallback pack ready");
      } else { delete info; if (error.length()) pushNetworkEvent(NetworkEventType::Error, 0, parsed.playing, millis() - requestStarted, nullptr, error); }
    }
    networkBusy = false;
  }
}

uint32_t fallbackSeed(const String &value) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < value.length(); i++) { hash ^= (uint8_t)value[i]; hash *= 16777619u; }
  return hash ? hash : 0x9e3779b9u;
}

uint32_t nextRandom(uint32_t &state) {
  state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state;
}

void rebuildFallbackOrder() {
  if (localPack.order) { free(localPack.order); localPack.order = nullptr; }
  if (!localPack.valid || !localPack.count) return;
  localPack.order = (uint16_t *)malloc(localPack.count * sizeof(uint16_t));
  if (!localPack.order) { localPack.valid = false; lastResult = "fallback order allocation failed"; return; }
  for (uint16_t i = 0; i < localPack.count; i++) localPack.order[i] = i;
  uint32_t random = fallbackSeed(localPack.version);
  for (int i = localPack.count - 1; i > 0; i--) {
    uint16_t j = (uint16_t)(((uint64_t)nextRandom(random) * (uint32_t)(i + 1)) >> 32);
    uint16_t temporary = localPack.order[i]; localPack.order[i] = localPack.order[j]; localPack.order[j] = temporary;
  }
  localPack.cursor = prefs.getUShort("packcursor", 0) % localPack.count;
  if (localPack.count > 1 && localPack.order[localPack.cursor] == localPack.lastIndex) {
    uint16_t other = (localPack.cursor + 1) % localPack.count;
    uint16_t temporary = localPack.order[localPack.cursor]; localPack.order[localPack.cursor] = localPack.order[other]; localPack.order[other] = temporary;
  }
}

bool activateLocalPack(const PackInstallInfo &info, bool persist = true) {
  const char *path = info.slot == 'a' ? FALLBACK_PACK_A_PATH : FALLBACK_PACK_B_PATH;
  if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
  File file = LittleFS.open(path, "r");
  PackHeader header{};
  bool valid = file && file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) == sizeof(header) &&
               !memcmp(header.magic, "ACPK", 4) && header.version == 1 && header.width == FRAME_WIDTH &&
               header.height == FRAME_HEIGHT && header.count == info.count && header.metadataLength == info.metadataLength &&
               file.size() == info.size;
  if (file) file.close();
  String digest;
  if (valid && !persist) valid = sha256File(path, digest) && digest == String(info.version);
  if (fsMutex) xSemaphoreGive(fsMutex);
  if (!valid) return false;
  localPack.valid = true;
  localPack.slot = info.slot;
  localPack.version = info.version;
  localPack.size = info.size;
  localPack.metadataLength = info.metadataLength;
  localPack.count = info.count;
  if (persist) {
    prefs.putChar("packslot", info.slot);
    prefs.putString("packver", info.version);
    prefs.putULong("packsize", info.size);
    prefs.putULong("packmeta", info.metadataLength);
    prefs.putUShort("packcount", info.count);
    prefs.putUShort("packcursor", 0);
    prefs.putUShort("packlast", UINT16_MAX);
    localPack.lastIndex = UINT16_MAX;
  } else {
    localPack.lastIndex = prefs.getUShort("packlast", UINT16_MAX);
  }
  rebuildFallbackOrder();
  return localPack.valid;
}

void loadLocalPack() {
  PackInstallInfo info{};
  info.slot = prefs.getChar("packslot", 'a');
  String version = prefs.getString("packver", "");
  strlcpy(info.version, version.c_str(), sizeof(info.version));
  info.size = prefs.getULong("packsize", 0);
  info.metadataLength = prefs.getULong("packmeta", 0);
  info.count = prefs.getUShort("packcount", 0);
  if (version.length() == 64 && info.size && info.count) activateLocalPack(info, false);
}

bool readFallbackFrame(uint16_t index, uint8_t *output) {
  if (!localPack.valid || index >= localPack.count || !output) return false;
  const char *path = localPack.slot == 'a' ? FALLBACK_PACK_A_PATH : FALLBACK_PACK_B_PATH;
  if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  File file = LittleFS.open(path, "r");
  uint32_t offset = sizeof(PackHeader) + localPack.metadataLength + (uint32_t)index * FRAME_BYTES;
  bool ok = file && file.seek(offset, SeekSet) && file.read(output, FRAME_BYTES) == FRAME_BYTES;
  if (file) file.close();
  if (fsMutex) xSemaphoreGive(fsMutex);
  return ok;
}

bool beginNextFallback(bool manual) {
  if (playbackActive || !localPack.valid || !localPack.order) return false;
  if (manual) manualHoldUntil = millis() + MANUAL_FALLBACK_HOLD_MS;
  if (transition.active) { if (manual) nextQueued = true; return true; }
  uint16_t index = localPack.order[localPack.cursor];
  localPack.cursor = (localPack.cursor + 1) % localPack.count;
  if (localPack.count > 1 && index == localPack.lastIndex) {
    index = localPack.order[localPack.cursor];
    localPack.cursor = (localPack.cursor + 1) % localPack.count;
  }
  if (!readFallbackFrame(index, staging)) { lastResult = "fallback frame read failed"; return false; }
  localPack.lastIndex = index;
  prefs.putUShort("packcursor", localPack.cursor);
  prefs.putUShort("packlast", localPack.lastIndex);
  lastFallbackAt = millis();
  startTransition(reinterpret_cast<const uint16_t *>(staging), ++nextDisplayGeneration);
  lastResult = "local fallback ready";
  return true;
}

void processNetworkEvents() {
  NetworkEvent event;
  while (xQueueReceive(networkEvents, &event, 0) == pdTRUE) {
    lastNetworkLatencyMs = event.latencyMs;
    if (event.type == NetworkEventType::State) {
      lastStateAt = millis();
      if (event.data) {
        RemoteSnapshot *snapshot = reinterpret_cast<RemoteSnapshot *>(event.data);
        remoteState = *snapshot;
        delete snapshot;
        playbackActive = remoteState.playing;
        if (playbackActive) { manualHoldUntil = 0; nextQueued = false; }
        if (frameUrl != serviceOrigin) {
          frameUrl = serviceOrigin;
          prefs.putString("url", frameUrl);
        }
      }
      lastResult = event.message;
    } else if (event.type == NetworkEventType::Frame) {
      if (event.data && event.generation >= latestRequestedGeneration && event.generation >= lastRemoteGeneration) {
        lastRemoteGeneration = event.generation;
        lastFrameAt = millis();
        lastByteCount = FRAME_BYTES;
        startTransition(reinterpret_cast<const uint16_t *>(event.data), ++nextDisplayGeneration);
        lastResult = event.message;
      }
      if (event.data) free(event.data);
    } else if (event.type == NetworkEventType::Pack) {
      if (event.data) {
        PackInstallInfo *info = reinterpret_cast<PackInstallInfo *>(event.data);
        lastResult = activateLocalPack(*info) ? "fallback pack activated" : "fallback pack activation failed";
        delete info;
      }
    } else {
      lastResult = event.message;
    }
  }
}

// ---------------------------------------------------------------------------
// Config web server (served in both AP and STA modes)
// ---------------------------------------------------------------------------
String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      // skip
    } else {
      out += c;
    }
  }
  return out;
}

void handleRoot() { server.send_P(200, "text/html", PORTAL_HTML); }

void handleStatus() {
  bool connected = WiFi.status() == WL_CONNECTED;
  unsigned long agoSec = lastStateAt ? (millis() - lastStateAt) / 1000 : 0;
  String ip = apMode ? WiFi.softAPIP().toString()
                     : (connected ? WiFi.localIP().toString() : "");

  String j = "{";
  j += "\"fw\":\"" FIRMWARE_VERSION "\",";
  j += "\"ap_mode\":" + String(apMode ? "true" : "false") + ",";
  j += "\"wifi_connected\":" + String(connected ? "true" : "false") + ",";
  j += "\"ssid\":\"" + jsonEscape(wifiSsid) + "\",";
  j += "\"ip\":\"" + ip + "\",";
  j += "\"rssi\":" + String(connected ? WiFi.RSSI() : 0) + ",";
  j += "\"frame_url\":\"" + jsonEscape(frameUrl) + "\",";
  j += "\"brightness\":" + String(brightness) + ",";
  j += "\"color_order\":" + String(colorOrder) + ",";
  j += "\"test_card\":" + String(testCard ? "true" : "false") + ",";
  j += "\"fade_enabled\":" + String(fadeEnabled ? "true" : "false") + ",";
  j += "\"fade_ms\":" + String(transitionMs) + ",";
  j += "\"fade_hold_ms\":" + String(holdMs) + ",";
  j += "\"fade_method\":" + String(fadeMethod) + ",";
  j += "\"pat_out\":" + String(patOut) + ",";
  j += "\"pat_in\":" + String(patIn) + ",";
  j += "\"ease_out\":" + String(easeOut) + ",";
  j += "\"ease_in\":" + String(easeIn) + ",";
  j += "\"slide_dir\":" + String(slideDir) + ",";
  j += "\"balance\":" + String(balancePct) + ",";
  j += "\"spread\":" + String(spreadPct) + ",";
  j += "\"soft\":" + String(softPct) + ",";
  j += "\"jitter\":" + String(jitterPct) + ",";
  j += "\"desat\":" + String(desatPct) + ",";
  j += "\"gamma\":" + String(gammaOn ? "true" : "false") + ",";
  j += "\"display_mode\":" + String(displayMode) + ",";
  j += "\"audio_ready\":" + String(audioReady ? "true" : "false") + ",";
  j += "\"audio_rms\":" + String((int)audio::rms()) + ",";
  j += "\"has_frame\":" + String(hasFrame ? "true" : "false") + ",";
  j += "\"playback\":\"" + jsonEscape(remoteState.playback) + "\",";
  j += "\"service_status\":\"" + jsonEscape(remoteState.serviceStatus) + "\",";
  j += "\"pending_title\":\"" + jsonEscape(remoteState.pendingTitle) + "\",";
  j += "\"network_busy\":" + String(networkBusy ? "true" : "false") + ",";
  j += "\"legacy_mode\":" + String(legacyMode ? "true" : "false") + ",";
  j += "\"transition_active\":" + String(transition.active ? "true" : "false") + ",";
  j += "\"next_queued\":" + String(nextQueued ? "true" : "false") + ",";
  j += "\"display_generation\":" + String(displayGeneration) + ",";
  j += "\"remote_generation\":" + String(remoteState.generation) + ",";
  j += "\"pack_version\":\"" + jsonEscape(localPack.version) + "\",";
  j += "\"pack_count\":" + String(localPack.count) + ",";
  j += "\"pack_cursor\":" + String(localPack.cursor) + ",";
  j += "\"last_network_latency_ms\":" + String(lastNetworkLatencyMs) + ",";
  j += "\"frame_age_sec\":" + String(lastFrameAt ? (millis() - lastFrameAt) / 1000 : 0) + ",";
  j += "\"manual_hold_sec\":" + String(manualHoldUntil && (int32_t)(manualHoldUntil - millis()) > 0 ? (manualHoldUntil - millis()) / 1000 : 0) + ",";
  j += "\"last_result\":\"" + jsonEscape(lastResult) + "\",";
  j += "\"last_bytes\":" + String(lastByteCount) + ",";
  j += "\"last_fetch_sec_ago\":" + String(agoSec) + ",";
  j += "\"uptime_sec\":" + String(millis() / 1000) + ",";
  j += "\"free_heap\":" + String(ESP.getFreeHeap());
  j += "}";
  server.send(200, "application/json", j);
}

void handleFrameBin() {
  server.setContentLength(FRAME_BYTES);
  server.send(200, "application/octet-stream", "");
  server.client().write(reinterpret_cast<const uint8_t *>(frame), FRAME_BYTES);
}

void applyBrightness(uint8_t value) {
  brightness = constrain(value, 1, 255);
  prefs.putUChar("bright", brightness);
  if (display) display->setBrightness8(brightness);
}

void handleBrightness() {
  if (server.hasArg("value")) {
    applyBrightness(static_cast<uint8_t>(server.arg("value").toInt()));
  }
  server.send(200, "text/plain", "brightness " + String(brightness));
}

void handleColorOrder() {
  if (server.hasArg("value")) {
    int v = server.arg("value").toInt();
    if (v >= 0 && v <= 5) {
      colorOrder = static_cast<uint8_t>(v);
      prefs.putUChar("colororder", colorOrder);
      drawFrame();
    }
  }
  server.send(200, "text/plain", "color order " + String(colorOrder));
}

void handleTestCard() {
  testCard = !testCard;
  drawFrame();
  server.send(200, "text/plain", testCard ? "test card ON" : "test card OFF");
}

void handleFade() {
  if (server.hasArg("value")) {
    fadeEnabled = server.arg("value").toInt() != 0;
    prefs.putUChar("fade", fadeEnabled ? 1 : 0);
  }
  if (server.hasArg("ms")) {
    transitionMs = constrain(server.arg("ms").toInt(), 500, 6000);
    prefs.putUShort("fadems", transitionMs);
  }
  if (server.hasArg("hold")) {
    holdMs = constrain(server.arg("hold").toInt(), 0, 2000);
    prefs.putUShort("holdms", holdMs);
  }
  if (server.hasArg("method")) { fadeMethod = constrain(server.arg("method").toInt(), 0, 2); prefs.putUChar("fmethod", fadeMethod); }
  if (server.hasArg("patout")) { patOut = constrain(server.arg("patout").toInt(), 0, 10); prefs.putUChar("patout", patOut); }
  if (server.hasArg("patin")) { patIn = constrain(server.arg("patin").toInt(), 0, 10); prefs.putUChar("patin", patIn); }
  if (server.hasArg("easeout")) { easeOut = constrain(server.arg("easeout").toInt(), 0, 9); prefs.putUChar("easeout", easeOut); }
  if (server.hasArg("easein")) { easeIn = constrain(server.arg("easein").toInt(), 0, 9); prefs.putUChar("easein", easeIn); }
  if (server.hasArg("slidedir")) { slideDir = constrain(server.arg("slidedir").toInt(), 0, 3); prefs.putUChar("slidedir", slideDir); }
  if (server.hasArg("balance")) { balancePct = constrain(server.arg("balance").toInt(), 10, 90); prefs.putUChar("balance", balancePct); }
  if (server.hasArg("spread")) { spreadPct = constrain(server.arg("spread").toInt(), 0, 95); prefs.putUChar("spread", spreadPct); }
  if (server.hasArg("soft")) { softPct = constrain(server.arg("soft").toInt(), 5, 95); prefs.putUChar("soft", softPct); }
  if (server.hasArg("jitter")) { jitterPct = constrain(server.arg("jitter").toInt(), 0, 80); prefs.putUChar("jitter", jitterPct); }
  if (server.hasArg("desat")) { desatPct = constrain(server.arg("desat").toInt(), 0, 100); prefs.putUChar("desat", desatPct); }
  if (server.hasArg("gamma")) { gammaOn = server.arg("gamma").toInt() != 0; prefs.putUChar("gamma", gammaOn ? 1 : 0); }
  server.send(200, "text/plain",
              String("fade ") + (fadeEnabled ? "on" : "off") + " " + transitionMs +
                  "ms hold " + holdMs + "ms method " + fadeMethod);
}

void handleMode() {
  if (server.hasArg("value")) {
    uint8_t m = constrain(server.arg("value").toInt(), 0, 1);
    if (m != displayMode) {
      displayMode = m;
      prefs.putUChar("mode", displayMode);
      if (displayMode == 0 && hasFrame) drawFrame();
    }
  }
  server.send(200, "text/plain", displayMode == 1 ? "audio-reactive mode" : "album-art mode");
}

void handleSave() {
  String newSsid = server.arg("ssid");
  String newPass = server.arg("pass");
  String newUrl = server.arg("url");

  bool wifiChanged = false;
  bool serviceChanged = false;
  if (newSsid.length() && newSsid != wifiSsid) wifiChanged = true;
  if (newPass.length() && newPass != wifiPass) wifiChanged = true;

  if (newSsid.length()) {
    wifiSsid = newSsid;
    prefs.putString("ssid", wifiSsid);
  }
  if (newPass.length()) {
    wifiPass = newPass;
    prefs.putString("pass", wifiPass);
  }
  if (isValidFrameUrl(newUrl)) {
    serviceChanged = newUrl != frameUrl;
    frameUrl = newUrl;
    prefs.putString("url", frameUrl);
  }
  if (server.hasArg("brightness")) {
    applyBrightness(static_cast<uint8_t>(server.arg("brightness").toInt()));
  }

  if (wifiChanged || serviceChanged || apMode) {
    server.send(200, "text/plain", "Saved. Restarting to apply network settings…");
    delay(400);
    ESP.restart();
  } else {
    NetworkCommand command{NetworkCommandType::Refresh};
    xQueueOverwrite(networkCommands, &command);
    server.send(202, "application/json", "{\"accepted\":true,\"message\":\"Saved; refresh queued\"}");
  }
}

void handleRefresh() {
  NetworkCommand command{NetworkCommandType::Refresh};
  xQueueOverwrite(networkCommands, &command);
  server.send(202, "application/json", "{\"accepted\":true,\"message\":\"Refresh queued\"}");
}

void handleNext() {
  if (playbackActive) {
    server.send(409, "application/json", "{\"error\":\"now_playing_active\",\"message\":\"Next fallback is available when playback is idle\"}");
    return;
  }
  if (!localPack.valid) {
    server.send(503, "application/json", "{\"error\":\"fallback_pack_unavailable\",\"message\":\"The fallback pack is still syncing\"}");
    return;
  }
  bool queued = transition.active;
  if (queued && nextQueued) {
    server.send(200, "application/json", "{\"accepted\":true,\"queued\":true,\"message\":\"One Next is already queued\"}");
    return;
  }
  bool ok = beginNextFallback(true);
  server.send(ok ? 202 : 500, "application/json",
              ok ? String("{\"accepted\":true,\"queued\":") + (queued ? "true" : "false") + ",\"message\":\"Local fallback ready\"}"
                 : "{\"error\":\"fallback_read_failed\",\"message\":\"Could not read the next local frame\"}");
}

void handleRestart() {
  server.send(200, "text/plain", "Restarting…");
  delay(300);
  ESP.restart();
}

void handleReset() {
  prefs.clear();
  server.send(200, "text/plain", "Settings cleared. Restarting…");
  delay(300);
  ESP.restart();
}

void handleNotFound() {
  if (apMode) {
    // Captive-portal redirect: send any probe to the config page.
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/frame.bin", HTTP_GET, handleFrameBin);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/brightness", HTTP_POST, handleBrightness);
  server.on("/colororder", HTTP_POST, handleColorOrder);
  server.on("/testcard", HTTP_POST, handleTestCard);
  server.on("/fade", HTTP_POST, handleFade);
  server.on("/mode", HTTP_POST, handleMode);
  server.on("/refresh", HTTP_POST, handleRefresh);
  server.on("/next", HTTP_POST, handleNext);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/reset", HTTP_POST, handleReset);
  server.onNotFound(handleNotFound);
  server.begin();
  serverStarted = true;
}

// ---------------------------------------------------------------------------
// Network modes
// ---------------------------------------------------------------------------
void startApPortal() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  if (strlen(WIFI_SETUP_AP_PASSWORD) > 0) {
    WiFi.softAP(WIFI_SETUP_AP_NAME, WIFI_SETUP_AP_PASSWORD);
  } else {
    WiFi.softAP(WIFI_SETUP_AP_NAME);
  }
  delay(200);
  dnsServer.start(53, "*", WiFi.softAPIP());
  setupRoutes();
  Serial.printf("AP portal up: %s  http://%s/\n", WIFI_SETUP_AP_NAME,
                WiFi.softAPIP().toString().c_str());
}

bool connectWifi(unsigned long timeoutMs) {
  apMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    serviceTransition();
    delay(20);
  }
  return WiFi.status() == WL_CONNECTED;
}

void startStaServices() {
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
  }
  setupRoutes();
  Serial.printf("STA ready: http://%s/  (http://%s.local/)\n",
                WiFi.localIP().toString().c_str(), MDNS_HOSTNAME);
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nAlbum Clock " FIRMWARE_VERSION);

  loadSettings();
  serviceOrigin = originFromUrl(frameUrl);
  if (!serviceOrigin.length()) serviceOrigin = originFromUrl(DEFAULT_FRAME_URL);
  initGammaLUT();
  setupMatrix();

  fsMutex = xSemaphoreCreateMutex();
  networkCommands = xQueueCreate(1, sizeof(NetworkCommand));
  networkEvents = xQueueCreate(6, sizeof(NetworkEvent));
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }
  loadLocalPack();
  if (loadCachedFrame()) {
    hasFrame = true;
    memcpy(compositeFrame, frame, FRAME_BYTES);
    if (fadeEnabled) {
      memcpy(staging, frame, FRAME_BYTES);
      hasFrame = false;
      startTransition(reinterpret_cast<const uint16_t *>(staging), ++nextDisplayGeneration);
    } else {
      drawFrame();
    }
    Serial.println("Restored cached frame");
  } else {
    showBootPattern();
  }

  pinMode(RESET_CONFIG_PIN, INPUT_PULLUP);
  delay(40);
  bool forcePortal = digitalRead(RESET_CONFIG_PIN) == LOW;

  if (forcePortal || wifiSsid.length() == 0) {
    Serial.println(forcePortal ? "BOOT held -> setup portal"
                               : "No Wi-Fi saved -> setup portal");
    startApPortal();
    return;
  }

  Serial.printf("Joining Wi-Fi \"%s\"…\n", wifiSsid.c_str());
  if (connectWifi(30000)) {
    Serial.printf("Wi-Fi ok, IP %s\n", WiFi.localIP().toString().c_str());
    startStaServices();
  } else {
    // Router may just be down. Keep showing cached art and retry in loop;
    // hold BOOT at power-on if you need the setup portal.
    Serial.println("Wi-Fi not connected yet; will keep retrying");
    startStaServices();
  }
  xTaskCreatePinnedToCore(networkTask, "album-network", 12288, nullptr, 1, &networkTaskHandle, 0);
}

void loop() {
  if (apMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    serviceTransition();
    return;
  }

  processNetworkEvents();
  serviceTransition();
  if (cachePending && !transition.active) { cachePending = false; persistFrame(); }

  if (displayMode == 1) {
    serviceAudio();
    server.handleClient();
    return;
  }

  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastReconnectAttempt >= 10000) {
      lastReconnectAttempt = millis();
      Serial.println("Wi-Fi down, reconnecting…");
      WiFi.reconnect();
    }
    delay(20);
    return;
  }

  bool holdExpired = !manualHoldUntil || (int32_t)(millis() - manualHoldUntil) >= 0;
  if (!playbackActive && localPack.valid && !transition.active && holdExpired &&
      (!lastFallbackAt || millis() - lastFallbackAt >= FALLBACK_ROTATION_MS)) {
    beginNextFallback(false);
  }

  delay(2);
}
