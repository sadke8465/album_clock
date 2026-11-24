#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <esp_tls.h>
#include <esp_heap_caps.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/sha256.h>
#include "WiimCertificate.h"

// Dedicated, verified TLS connection to the owner's fixed WiiM address.
static constexpr char WIIM_IP[] = "192.168.1.108";
static constexpr char WIIM_CN[] = "www.linkplay.com";
static bool wiimRead(const char *path, uint8_t *buffer, size_t capacity, size_t &length) {
  length = 0;
  esp_tls_cfg_t cfg{};
  cfg.cacert_buf = (const unsigned char *)WIIM_CERT;
  cfg.cacert_bytes = sizeof(WIIM_CERT);
  cfg.common_name = WIIM_CN;
  cfg.timeout_ms = 2500;
  esp_tls_t *tls = esp_tls_init();
  if (!tls) return false;
  bool ok = esp_tls_conn_new_sync(WIIM_IP, strlen(WIIM_IP), 443, &cfg, tls) == 1;
  if (ok) {
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&tls->ssl);
    unsigned char actual[32];
    static const unsigned char expected[32] = {
      0xaa,0x60,0xee,0x2f,0x28,0x0b,0xe0,0x2e,0x13,0x1b,0xbc,0xff,0xa8,0x80,0x4e,0x9c,
      0x43,0x95,0x8b,0x46,0x2c,0x90,0xa3,0xd8,0x53,0x6d,0x87,0x51,0xc2,0xbe,0xf0,0x0a};
    ok = peer && mbedtls_sha256_ret(peer->raw.p, peer->raw.len, actual, 0) == 0 &&
         memcmp(actual, expected, sizeof(actual)) == 0;
  }
  String request = String("GET ") + path + " HTTP/1.0\r\nHost: " + WIIM_IP +
                   "\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n";
  if (ok) {
    size_t sent = 0;
    while (sent < request.length()) {
      int n = esp_tls_conn_write(tls, request.c_str() + sent, request.length() - sent);
      if (n <= 0) { ok = false; break; }
      sent += n;
    }
  }
  String header;
  int expectedLength = -1;
  bool headersDone = false;
  uint8_t chunk[1024];
  uint32_t started = millis();
  while (ok && millis() - started < 10000) {
    int n = esp_tls_conn_read(tls, chunk, sizeof(chunk));
    if (n <= 0) { ok = n == 0 && headersDone && expectedLength < 0; break; }
    for (int i = 0; i < n; i++) {
      if (!headersDone) {
        header += (char)chunk[i];
        if (header.length() > 8192) { ok = false; break; }
        if (header.endsWith("\r\n\r\n")) {
          if (!(header.startsWith("HTTP/1.1 200 ") || header.startsWith("HTTP/1.0 200 "))) { ok = false; break; }
          header.toLowerCase();
          if (header.indexOf("transfer-encoding: chunked") >= 0) { ok = false; break; }
          int at = header.indexOf("content-length:");
          if (at >= 0) expectedLength = header.substring(at + 15).toInt();
          if (expectedLength > (int)capacity) { ok = false; break; }
          headersDone = true;
        }
      } else {
        if (length >= capacity) { ok = false; break; }
        buffer[length++] = chunk[i];
      }
    }
    if (headersDone && expectedLength >= 0 && length >= (size_t)expectedLength) break;
  }
  esp_tls_conn_destroy(tls);
  return ok && headersDone && length > 0 && (expectedLength < 0 || length == (size_t)expectedLength);
}

enum class WiimKind { Unknown, Idle, Music, Radio };
struct WiimPlayback {
  WiimKind kind;
  String key, title;
  WiimPlayback(WiimKind k = WiimKind::Unknown, String id = "", String name = "")
    : kind(k), key(id), title(name) {}
};
static bool meaningful(String s) {
  s.trim(); s.toLowerCase();
  return s.length() && s != "unknow" && s != "unknown" && s != "null";
}
static String decodeWiimHex(const String &value) {
  if (value.length() % 2) return "";
  String result;
  for (unsigned i = 0; i < value.length(); i += 2) {
    char pair[3] = {value[i], value[i+1], 0};
    char *end;
    long v = strtol(pair, &end, 16);
    if (*end || v <= 0 || v > 255) return "";
    result += (char)v;
  }
  return result;
}
static bool wiimRadioTitle(String title) {
  title.trim(); title.toLowerCase();
  return title == "radio 88 fm" || title == "kan 88" ||
    title == "kan 88 - כאן 88" || title == "כאן 88";
}
static WiimPlayback pollWiim() {
  uint8_t body[4096]; size_t n;
  JsonDocument status, metadata;
  if (!wiimRead("/httpapi.asp?command=getPlayerStatus", body, sizeof(body), n) ||
      deserializeJson(status, body, n)) return {};
  String state = status["status"] | "";
  if (state != "play") return {WiimKind::Idle, "", ""};
  if (!wiimRead("/httpapi.asp?command=getMetaInfo", body, sizeof(body), n) ||
      deserializeJson(metadata, body, n)) return {};
  JsonObjectConst meta = metadata["metaData"].as<JsonObjectConst>();
  String title = meta["title"] | "", artist = meta["artist"] | "", album = meta["album"] | "";
  String statusTitle = decodeWiimHex(status["Title"] | "");
  String statusArtist = decodeWiimHex(status["Artist"] | "");
  String statusAlbum = decodeWiimHex(status["Album"] | "");
  if (wiimRadioTitle(statusTitle))
    return {WiimKind::Radio, "kan88", title};
  if (wiimRadioTitle(title) && !meaningful(statusTitle))
    return {WiimKind::Idle, "", ""};
  String mode = status["mode"] | "";
  if (mode == "1" && title == statusTitle && artist == statusArtist && album == statusAlbum &&
      meaningful(title) && meaningful(artist) && meaningful(album))
    return {WiimKind::Music, title + "\n" + artist + "\n" + album, title};
  return {WiimKind::Idle, "", ""};
}

struct WiimJpegTarget { uint16_t *frame, *detail; int side, left, top; };
static WiimJpegTarget wiimJpegTarget;
static int wiimSampleLowerBound(int source,int origin,int side,int size) {
  int lo=0,hi=size;
  while(lo<hi){int mid=(lo+hi)/2;int value=origin+((2*mid+1)*side)/(2*size);
    if(value<source)lo=mid+1;else hi=mid;}
  return lo;
}
static void drawWiimTarget(JPEGDRAW *draw,uint16_t *target,int size) {
  auto &t = wiimJpegTarget;
  if(!target)return;
  int x0=wiimSampleLowerBound(draw->x,t.left,t.side,size),x1=wiimSampleLowerBound(draw->x+draw->iWidthUsed,t.left,t.side,size);
  int y0=wiimSampleLowerBound(draw->y,t.top,t.side,size),y1=wiimSampleLowerBound(draw->y+draw->iHeight,t.top,t.side,size);
  for (int y = y0; y < y1; ++y) {
    int sy = t.top + ((2*y+1)*t.side)/(2*size);
    if (sy < draw->y || sy >= draw->y + draw->iHeight) continue;
    for (int x = x0; x < x1; ++x) {
      int sx = t.left + ((2*x+1)*t.side)/(2*size);
      if (sx >= draw->x && sx < draw->x + draw->iWidthUsed)
        target[y*size+x] = draw->pPixels[(sy-draw->y)*draw->iWidth + sx-draw->x];
    }
  }
}
static int drawWiimJpeg(JPEGDRAW *draw) {
  drawWiimTarget(draw,wiimJpegTarget.frame,64);
  drawWiimTarget(draw,wiimJpegTarget.detail,FACE_DETAIL_SIZE);
  return 1;
}
static bool wiimArtwork(uint16_t *frame,uint16_t *detail=nullptr) {
  constexpr size_t limit = 2*1024*1024;
  uint8_t *bytes = (uint8_t *)heap_caps_malloc(limit, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!bytes) return false;
  size_t n;
  bool ok = wiimRead("/data/AirplayArtWorkData.jpeg", bytes, limit, n);
  JPEGDEC *jpeg = new JPEGDEC;
  if (ok && jpeg->openRAM(bytes, n, drawWiimJpeg)) {
    int w = jpeg->getWidth(), h = jpeg->getHeight(), side = min(w,h);
    Serial.printf("WiiM artwork source: %dx%d\n",w,h);
    wiimJpegTarget = {frame,detail,side,(w-side)/2,(h-side)/2};
    memset(frame, 0, 8192);
    if(detail)memset(detail,0,FACE_DETAIL_SIZE*FACE_DETAIL_SIZE*2);
    jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
    ok = side > 0 && side <= 4096 && jpeg->decode(0,0,0);
    if(ok&&detail)faceResizeClean(detail,FACE_DETAIL_SIZE,frame,64,{0,0,FACE_SOURCE_SIZE});
    jpeg->close();
  } else ok = false;
  delete jpeg;
  free(bytes);
  return ok;
}
