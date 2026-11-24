#pragma once
#include <stdint.h>
#include <math.h>

// Operates on unmodified RGB565 source frames; never writes into the cache.
struct ImageAdjustments {
  int16_t contrast = 100, saturation = 100, exposure = 0, warmth = 0, tint = 0;
  int16_t hue = 0, gamma = 100, blur = 0, red = 100, green = 100, blue = 100;
};

static inline int imageClamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int imageR(uint16_t v) { int n = (v >> 11) & 31; return (n << 3) | (n >> 2); }
static inline int imageG(uint16_t v) { int n = (v >> 5) & 63; return (n << 2) | (n >> 4); }
static inline int imageB(uint16_t v) { int n = v & 31; return (n << 3) | (n >> 2); }
static inline uint16_t image565(int r, int g, int b) {
  return ((imageClamp(r,0,255)&248)<<8) | ((imageClamp(g,0,255)&252)<<3) | (imageClamp(b,0,255)>>3);
}

// Separable box blur, clamping edge samples instead of wrapping.
static inline void imageBlur(const uint16_t *src, uint16_t *dst, int radius, bool horizontal) {
  for (int y=0;y<64;y++) for (int x=0;x<64;x++) {
    int r=0,g=0,b=0;
    for (int d=-radius;d<=radius;d++) {
      const uint16_t v=src[imageClamp(y+(horizontal?0:d),0,63)*64+imageClamp(x+(horizontal?d:0),0,63)];
      r+=imageR(v);g+=imageG(v);b+=imageB(v);
    }
    int n=radius*2+1;
    dst[y*64+x]=image565((r+n/2)/n,(g+n/2)/n,(b+n/2)/n);
  }
}

static inline void imageRender(const uint16_t *src, uint16_t *dst, uint16_t *scratch, const ImageAdjustments &a) {
  if (a.blur) { imageBlur(src,scratch,a.blur,true); imageBlur(scratch,dst,a.blur,false); src=dst; }
  const float angle=a.hue*0.01745329252f, c=cosf(angle), s=sinf(angle);
  uint8_t lut[256];
  for (int i=0;i<256;i++) lut[i]=a.gamma==100?i:(uint8_t)lroundf(255*powf(i/255.f,100.f/a.gamma));
  for (int i=0;i<4096;i++) {
    float r=imageR(src[i]),g=imageG(src[i]),b=imageB(src[i]);
    if (a.hue) {
      // Rotate chroma around luminance while preserving gray.
      float y=.299f*r+.587f*g+.114f*b, u=b-y, v=r-y;
      float uu=u*c-v*s,vv=u*s+v*c;
      r=y+vv; b=y+uu; g=(y-.299f*r-.114f*b)/.587f;
    }
    float l=.299f*r+.587f*g+.114f*b, sat=a.saturation/100.f;
    r=l+(r-l)*sat;g=l+(g-l)*sat;b=l+(b-l)*sat;
    const float contrast=a.contrast/100.f, offset=a.exposure*1.28f;
    r=((r-127.5f)*contrast+127.5f+offset)*a.red/100.f+a.warmth*.65f+a.tint*.3f;
    g=((g-127.5f)*contrast+127.5f+offset)*a.green/100.f-a.tint*.65f;
    b=((b-127.5f)*contrast+127.5f+offset)*a.blue/100.f-a.warmth*.65f+a.tint*.3f;
    dst[i]=image565(lut[imageClamp((int)lroundf(r),0,255)],lut[imageClamp((int)lroundf(g),0,255)],lut[imageClamp((int)lroundf(b),0,255)]);
  }
}
