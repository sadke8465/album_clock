#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

constexpr int FACE_SOURCE_SIZE=256, FACE_DETAIL_SIZE=512, FACE_DETECT_SIZE=192, FACE_MAX=5;
struct FaceRect { int x0,y0,x1,y1; };
struct FaceCrop { int x,y,side; };
constexpr int TEXT_MAX=4;
static inline int zoomClamp(int v,int lo,int hi){return v<lo?lo:(v>hi?hi:v);}
static inline FaceCrop centeredCrop(int x,int y,int side){
  side=zoomClamp(side,64,256);
  return {zoomClamp(x-side/2,0,256-side),zoomClamp(y-side/2,0,256-side),side};
}
static inline FaceCrop zoomDepthCrop(FaceCrop crop,int percent){
  if(crop.side==256)return crop;
  return centeredCrop(crop.x+crop.side/2,crop.y+crop.side/2,crop.side*100/zoomClamp(percent,50,150));
}
static inline FaceCrop faceScanTile(int index){
  if(index==0)return {0,0,256};
  if(index<=4){int i=index-1;return {(i%2)*96,(i/2)*96,160};}
  // Additional tightly framed center tile for small faces near boundaries.
  return {64,64,128};
}
static inline FaceRect mapFaceRect(FaceRect box,FaceCrop tile,int input){
  return {zoomClamp(tile.x+box.x0*tile.side/input,0,255),zoomClamp(tile.y+box.y0*tile.side/input,0,255),
    zoomClamp(tile.x+box.x1*tile.side/input,0,255),zoomClamp(tile.y+box.y1*tile.side/input,0,255)};
}
static inline bool sameFace(FaceRect a,FaceRect b){
  int x0=a.x0>b.x0?a.x0:b.x0,y0=a.y0>b.y0?a.y0:b.y0;
  int x1=a.x1<b.x1?a.x1:b.x1,y1=a.y1<b.y1?a.y1:b.y1;
  int w=x1-x0+1,h=y1-y0+1;if(w<=0||h<=0)return false;
  int overlap=w*h,aa=(a.x1-a.x0+1)*(a.y1-a.y0+1),bb=(b.x1-b.x0+1)*(b.y1-b.y0+1);
  return overlap*100>=35*(aa+bb-overlap)||overlap*100>=75*(aa<bb?aa:bb);
}
static inline void faceRgb888(uint16_t pixel,uint8_t *out) {
  int r=(pixel>>11)&31,g=(pixel>>5)&63,b=pixel&31;
  out[0]=(r<<3)|(r>>2);out[1]=(g<<2)|(g>>4);out[2]=(b<<3)|(b>>2);
}
// Gentle 1.33x zoom, clockwise around the artwork's four corners.
static inline FaceCrop quadrantCrop(int index) {
  constexpr int side=192, edge=FACE_SOURCE_SIZE-side;
  switch(index%4) {
    case 0:return {0,0,side};
    case 1:return {edge,0,side};
    case 2:return {edge,edge,side};
    default:return {0,edge,side};
  }
}
static inline FaceCrop interpolateCrop(FaceCrop a,FaceCrop b,float t) {
  if(t<0)t=0;if(t>1)t=1;
  t=t*t*(3-2*t);
  FaceCrop c{(int)lroundf(a.x+(b.x-a.x)*t),(int)lroundf(a.y+(b.y-a.y)*t),(int)lroundf(a.side+(b.side-a.side)*t)};
  if(c.x+c.side>FACE_SOURCE_SIZE)c.x=FACE_SOURCE_SIZE-c.side;
  if(c.y+c.side>FACE_SOURCE_SIZE)c.y=FACE_SOURCE_SIZE-c.side;
  return c;
}
// Center a square crop on the face, with breathing room around it.
static inline FaceCrop faceCrop(FaceRect b) {
  int w=b.x1-b.x0+1,h=b.y1-b.y0+1;
  int side=(int)ceilf((w>h?w:h)*1.9f);
  if(side<64)side=64;
  if(side>224)side=224;
  int x=(b.x0+b.x1+1-side)/2,y=(b.y0+b.y1+1-side)/2-h/10;
  if(x<0)x=0;if(y<0)y=0;
  if(x+side>FACE_SOURCE_SIZE)x=FACE_SOURCE_SIZE-side;
  if(y+side>FACE_SOURCE_SIZE)y=FACE_SOURCE_SIZE-side;
  return {x,y,side};
}
static inline uint16_t faceSample(const uint16_t *src,int width,int height,float x,float y) {
  if(x<0)x=0;if(y<0)y=0;
  if(x>width-1)x=width-1;if(y>height-1)y=height-1;
  int x0=(int)x,y0=(int)y,x1=x0+1<width?x0+1:x0,y1=y0+1<height?y0+1:y0;
  float fx=x-x0,fy=y-y0;
  uint16_t v[4]={src[y0*width+x0],src[y0*width+x1],src[y1*width+x0],src[y1*width+x1]};
  float weights[4]={(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy};
  float r=0,g=0,b=0;
  for(int i=0;i<4;i++){r+=((v[i]>>11)&31)*weights[i];g+=((v[i]>>5)&63)*weights[i];b+=(v[i]&31)*weights[i];}
  return ((uint16_t)lroundf(r)<<11)|((uint16_t)lroundf(g)<<5)|(uint16_t)lroundf(b);
}
static inline void faceResize(const uint16_t *src,int width,int height,uint16_t *dst,int size,FaceCrop crop) {
  for(int y=0;y<size;y++)for(int x=0;x<size;x++)
    dst[y*size+x]=faceSample(src,width,height,crop.x+(x+.5f)*crop.side/size-.5f,crop.y+(y+.5f)*crop.side/size-.5f);
}
// Four subpixel samples suppress aliasing when shrinking larger artwork.
static inline void faceResizeClean(const uint16_t *src,int width,uint16_t *dst,int size,FaceCrop crop) {
  float scale=width/(float)FACE_SOURCE_SIZE;
  float step=crop.side*scale/size;
  for(int y=0;y<size;y++)for(int x=0;x<size;x++){
    float sx=crop.x*scale+(x+.5f)*step-.5f,sy=crop.y*scale+(y+.5f)*step-.5f;
    if(step<=1.5f){dst[y*size+x]=faceSample(src,width,width,sx,sy);continue;}
    float d=step*.25f;
    uint16_t a=faceSample(src,width,width,sx-d,sy-d),b=faceSample(src,width,width,sx+d,sy-d),
      c=faceSample(src,width,width,sx-d,sy+d),e=faceSample(src,width,width,sx+d,sy+d);
    int r=((a>>11)+(b>>11)+(c>>11)+(e>>11)+2)/4;
    int g=(((a>>5)&63)+((b>>5)&63)+((c>>5)&63)+((e>>5)&63)+2)/4;
    int bl=((a&31)+(b&31)+(c&31)+(e&31)+2)/4;
    dst[y*size+x]=(r<<11)|(g<<5)|bl;
  }
}
