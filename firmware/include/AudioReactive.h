#pragma once
// Microphone capture + band analysis for the audio-reactive display mode.
//
// Waveshare ESP32-S3-RGB-Matrix audio path: dual analog mics -> ES7210 ADC ->
// I2S. Pins and the ES7210 init below come from Espressif's esp-bsp ES7210
// driver and Waveshare's board BSP (config.h). On the ESP32-S3 the HUB75 panel
// uses the LCD/GDMA peripheral, so the I2S peripheral is free for the mic.
//
// NOTE: the ES7210/I2S bring-up cannot be verified without the board. If the
// spectrum doesn't move, gain (regs 0x43-0x46) and the I2S slot are the first
// things to tune; audioDebug() prints the raw RMS to serial to help.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "driver/i2s.h"

#define AUD_I2C_SDA 47
#define AUD_I2C_SCL 48
#define AUD_I2S_MCLK 12
#define AUD_I2S_BCLK 43
#define AUD_I2S_WS 38
#define AUD_I2S_DIN 39
#define ES7210_ADDR 0x40
#define AUD_SAMPLE_RATE 16000
#define AUD_SAMPLES 256
#define AUD_BANDS 16

namespace audio {

static float coeff[AUD_BANDS];
static float smooth[AUD_BANDS];
static float agc = 500.0f;
static int16_t sampleBuf[AUD_SAMPLES * 2];  // stereo interleaved
static float mono[AUD_SAMPLES];
static float lastRms = 0;

inline void es7210Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// ES7210 init: 16 kHz, 16-bit, standard I2S, MCLK = 256*fs = 4.096 MHz.
inline bool es7210Init() {
  Wire.begin(AUD_I2C_SDA, AUD_I2C_SCL, 100000);
  Wire.beginTransmission(ES7210_ADDR);
  if (Wire.endTransmission() != 0) return false;  // codec not responding

  es7210Write(0x00, 0xFF);  // reset
  es7210Write(0x00, 0x32);
  es7210Write(0x09, 0x30);  // init/power-up timing
  es7210Write(0x0A, 0x30);
  es7210Write(0x23, 0x2A);  // HPF
  es7210Write(0x22, 0x0A);
  es7210Write(0x21, 0x2A);
  es7210Write(0x20, 0x0A);
  es7210Write(0x11, 0x60);  // SDP1: 16-bit, standard I2S
  es7210Write(0x12, 0x00);  // SDP2: TDM off
  es7210Write(0x40, 0xC3);  // analog power
  es7210Write(0x41, 0x70);  // mic1/2 bias
  es7210Write(0x42, 0x70);  // mic3/4 bias
  es7210Write(0x07, 0x20);  // OSR  (coeff for 4.096MHz/16k)
  es7210Write(0x02, 0xC1);  // MAINCLK: adc_div=1, doubler=1, dll=1
  es7210Write(0x04, 0x01);  // LRCK div high
  es7210Write(0x05, 0x00);  // LRCK div low
  es7210Write(0x06, 0x04);  // power-down control
  es7210Write(0x43, 0x1A);  // mic1 gain (~27 dB): 0x10 | gain
  es7210Write(0x44, 0x1A);  // mic2 gain
  es7210Write(0x45, 0x1A);  // mic3 gain
  es7210Write(0x46, 0x1A);  // mic4 gain
  es7210Write(0x47, 0x08);  // mic power
  es7210Write(0x48, 0x08);
  es7210Write(0x49, 0x08);
  es7210Write(0x4A, 0x08);
  es7210Write(0x4B, 0x0F);  // mic1/2 bias+ADC+PGA power on
  es7210Write(0x4C, 0x0F);  // mic3/4
  es7210Write(0x00, 0x71);  // enable
  es7210Write(0x00, 0x41);
  return true;
}

// Log-spaced Goertzel band coefficients (80 Hz .. 7 kHz).
inline void initBands() {
  for (int b = 0; b < AUD_BANDS; b++) {
    float f = 80.0f * powf(7000.0f / 80.0f, (float)b / (AUD_BANDS - 1));
    float k = f / AUD_SAMPLE_RATE;
    coeff[b] = 2.0f * cosf(2.0f * PI * k);
    smooth[b] = 0;
  }
}

inline bool begin() {
  if (!es7210Init()) return false;

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = AUD_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;  // stereo (mic1/mic2)
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = AUD_SAMPLES;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;  // MCLK = 256 * 16k = 4.096 MHz
  cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = AUD_I2S_MCLK;
  pins.bck_io_num = AUD_I2S_BCLK;
  pins.ws_io_num = AUD_I2S_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = AUD_I2S_DIN;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) return false;
  i2s_zero_dma_buffer(I2S_NUM_0);
  initBands();
  return true;
}

// Capture a block and fill bands[0..AUD_BANDS-1] with smoothed 0..1 magnitudes.
inline void readBands(float *bands) {
  size_t got = 0;
  i2s_read(I2S_NUM_0, sampleBuf, sizeof(sampleBuf), &got, portMAX_DELAY);
  int n = got / 4;  // stereo 16-bit frames
  if (n <= 0) n = 0;
  if (n > AUD_SAMPLES) n = AUD_SAMPLES;

  // Left channel, DC-removed.
  float mean = 0;
  for (int i = 0; i < n; i++) mean += sampleBuf[i * 2];
  mean = n ? mean / n : 0;
  double sumsq = 0;
  for (int i = 0; i < n; i++) {
    mono[i] = sampleBuf[i * 2] - mean;
    sumsq += mono[i] * mono[i];
  }
  lastRms = n ? sqrtf(sumsq / n) : 0;

  // Goertzel magnitude per band.
  float maxmag = 1.0f;
  float mag[AUD_BANDS];
  for (int b = 0; b < AUD_BANDS; b++) {
    float s0, s1 = 0, s2 = 0, c = coeff[b];
    for (int i = 0; i < n; i++) {
      s0 = mono[i] + c * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    float m = sqrtf(s1 * s1 + s2 * s2 - c * s1 * s2) / (n ? n : 1);
    mag[b] = m;
    if (m > maxmag) maxmag = m;
  }

  // Slow AGC so the display reacts to relative loudness at any volume.
  agc = agc * 0.995f;
  if (maxmag > agc) agc = maxmag;
  float inv = 1.0f / (agc + 1.0f);

  for (int b = 0; b < AUD_BANDS; b++) {
    float v = mag[b] * inv;          // 0..1-ish
    v = powf(v < 0 ? 0 : (v > 1 ? 1 : v), 0.6f);  // perceptual lift
    // Fast attack, slow decay.
    smooth[b] = v > smooth[b] ? v : smooth[b] * 0.80f + v * 0.20f;
    bands[b] = smooth[b];
  }
}

inline float rms() { return lastRms; }

}  // namespace audio
