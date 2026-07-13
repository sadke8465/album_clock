#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

#include "AlbumClockConfig.h"

namespace {
constexpr uint8_t kTestBrightness = 24;
MatrixPanel_I2S_DMA *matrix = nullptr;

void drawTestCard() {
  const uint16_t black = matrix->color565(0, 0, 0);
  const uint16_t white = matrix->color565(255, 255, 255);
  const uint16_t red = matrix->color565(255, 0, 0);
  const uint16_t green = matrix->color565(0, 255, 0);
  const uint16_t blue = matrix->color565(0, 0, 255);
  const uint16_t yellow = matrix->color565(255, 255, 0);

  matrix->fillScreen(black);
  matrix->fillRect(1, 1, 30, 30, red);
  matrix->fillRect(33, 1, 30, 30, green);
  matrix->fillRect(1, 33, 30, 30, blue);
  matrix->fillRect(33, 33, 30, 30, white);

  matrix->drawRect(0, 0, 64, 64, yellow);
  matrix->drawFastVLine(31, 0, 64, black);
  matrix->drawFastVLine(32, 0, 64, black);
  matrix->drawFastHLine(0, 31, 64, black);
  matrix->drawFastHLine(0, 32, 64, black);
  matrix->drawLine(0, 0, 63, 63, yellow);
  matrix->drawLine(63, 0, 0, 63, yellow);

  matrix->setTextSize(1);
  matrix->setTextWrap(false);
  matrix->setTextColor(white);
  matrix->setCursor(3, 3);
  matrix->print("R");
  matrix->setTextColor(black);
  matrix->setCursor(35, 3);
  matrix->print("G");
  matrix->setTextColor(white);
  matrix->setCursor(3, 35);
  matrix->print("B");
  matrix->setTextColor(black);
  matrix->setCursor(35, 35);
  matrix->print("W");

  for (int p = 8; p < 64; p += 8) {
    matrix->drawPixel(p, 0, white);
    matrix->drawPixel(0, p, white);
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Waveshare 64x64 HUB75 panel test");

  HUB75_I2S_CFG config(FRAME_WIDTH, FRAME_HEIGHT, 1);
  config.gpio.r1 = MATRIX_R1_PIN;
  config.gpio.g1 = MATRIX_G1_PIN;
  config.gpio.b1 = MATRIX_B1_PIN;
  config.gpio.r2 = MATRIX_R2_PIN;
  config.gpio.g2 = MATRIX_G2_PIN;
  config.gpio.b2 = MATRIX_B2_PIN;
  config.gpio.a = MATRIX_A_PIN;
  config.gpio.b = MATRIX_B_PIN;
  config.gpio.c = MATRIX_C_PIN;
  config.gpio.d = MATRIX_D_PIN;
  config.gpio.e = MATRIX_E_PIN;
  config.gpio.lat = MATRIX_LAT_PIN;
  config.gpio.oe = MATRIX_OE_PIN;
  config.gpio.clk = MATRIX_CLK_PIN;
  config.clkphase = false;
  config.i2sspeed = HUB75_I2S_CFG::HZ_16M;
  config.driver = HUB75_I2S_CFG::SHIFTREG;

  matrix = new MatrixPanel_I2S_DMA(config);
  if (!matrix->begin()) {
    Serial.println("ERROR: matrix DMA allocation failed");
    return;
  }

  matrix->setBrightness8(kTestBrightness);
  drawTestCard();
  Serial.println("Panel test card drawn at brightness 24/255");
}

void loop() {
  delay(1000);
}
