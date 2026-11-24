#pragma once
#include <stdint.h>
#include <math.h>

// Operates on unmodified RGB565 source frames; never writes into the cache.
struct ImageAdjustments {
  int16_t contrast = 100, saturation = 100, exposure = 0, warmth = 0, tint = 0;
  int16_t hue = 0, gamma = 100, blur = 0, red = 100, green = 100, blue = 100;
  int16_t blackpoint = 0, whitepoint = 255, blackcut = 0, whitecut = 0, shadows = 0, highlights = 0;
};

static inline ImageAdjustments cleanImagePreset() {
  ImageAdjustments a;
  a.blackpoint=8; a.whitepoint=247;
  a.blackcut=8; a.whitecut=8;
  a.shadows=35; a.highlights=35;
  a.gamma=92;
  return a;
}

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
  if ((a.shadows || a.highlights) && src==dst) {
    for(int i=0;i<4096;i++) scratch[i]=dst[i];
    src=scratch;
  }
  const float angle=a.hue*0.01745329252f, c=cosf(angle), s=sinf(angle);
  uint8_t lut[256];
  const int levelRange=a.whitepoint-a.blackpoint;
  for (int i=0;i<256;i++) {
    float level=imageClamp(i-a.blackpoint,0,levelRange)/(float)levelRange;
    lut[i]=(uint8_t)lroundf(255*(a.gamma==100?level:powf(level,100.f/a.gamma)));
  }
  for (int i=0;i<4096;i++) {
    float r=imageR(src[i]),g=imageG(src[i]),b=imageB(src[i]);
    if (a.shadows || a.highlights) {
      // Edge-aware 3x3 averaging only near the extremes. Reject neighbors
      // with different colors so dark text, faces and bright edges stay sharp.
      float l=.299f*r+.587f*g+.114f*b;
      float strength=l<80?a.shadows*(80-l)/8000.f:(l>175?a.highlights*(l-175)/8000.f:0);
      if (strength>0) {
        float rr=r*2, gg=g*2, bb=b*2; int count=2;
        int x=i%64,y=i/64;
        for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++) {
          if ((!dx&&!dy)||x+dx<0||x+dx>=64||y+dy<0||y+dy>=64) continue;
          uint16_t v=src[(y+dy)*64+x+dx];
          int nr=imageR(v),ng=imageG(v),nb=imageB(v);
          if(fabsf(nr-r)>32||fabsf(ng-g)>32||fabsf(nb-b)>32) continue;
          rr+=nr;gg+=ng;bb+=nb;count++;
        }
        r+=(rr/count-r)*strength;g+=(gg/count-g)*strength;b+=(bb/count-b)*strength;
      }
    }
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
    int ro=lut[imageClamp((int)lroundf(r),0,255)],go=lut[imageClamp((int)lroundf(g),0,255)],bo=lut[imageClamp((int)lroundf(b),0,255)];
    // Only snap when ALL channels are close to an endpoint (not saturated colors).
    if(a.blackcut && ro<=a.blackcut && go<=a.blackcut && bo<=a.blackcut) ro=go=bo=0;
    if(a.whitecut && ro>=255-a.whitecut && go>=255-a.whitecut && bo>=255-a.whitecut) ro=go=bo=255;
    dst[i]=image565(ro,go,bo);
  }
}
