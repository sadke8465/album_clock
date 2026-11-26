#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "ImageAdjustments.h"

struct AmbientSettings {
  int16_t mode=1,target=0,intensity=68,density=34,speed=80,size=2,threshold=40,
    softness=35,palette=1,direction=0,blend=0,fps=15,chroma=25;
};
static inline uint32_t ambientHash(uint32_t x){
  x^=x>>16;x*=0x7feb352dU;x^=x>>15;x*=0x846ca68bU;x^=x>>16;return x;
}
static inline int ambientLuma(uint16_t v){return (imageR(v)*77+imageG(v)*150+imageB(v)*29)>>8;}
static inline int ambientWeight(const uint16_t *base,int i,const AmbientSettings &s){
  uint16_t v=base[i];int l=ambientLuma(v),w=255;
  switch(s.target){
    case 0:w=l;break;
    case 1:w=255-l;break;
    case 2:{
      int x=i%64,y=i/64;
      int dx=x<63?abs(l-ambientLuma(base[i+1])):0;
      int dy=y<63?abs(l-ambientLuma(base[i+64])):0;
      w=imageClamp((dx+dy)*3,0,255);break;
    }
    case 4:{int r=imageR(v),g=imageG(v),b=imageB(v);
      int hi=r>g?(r>b?r:b):(g>b?g:b),lo=r<g?(r<b?r:b):(g<b?g:b);w=hi-lo;break;}
  }
  if(s.target==3)return 255;
  if(w<=s.threshold)return 0;
  // The threshold excludes unwanted pixels; a weighted floor makes the
  // retained highlights visible even at low panel brightness.
  return 96+(w-s.threshold)*159/(255-s.threshold);
}
static inline int ambientEnvelope(int phase){
  int t=phase<128?phase*2:(255-phase)*2;
  return t*t*(765-2*t)/65025;
}
static inline uint16_t ambientBlend(uint16_t base,int strength,const AmbientSettings &s,uint32_t seed){
  if(strength<=0)return base;
  int r=imageR(base),g=imageG(base),b=imageB(base),cr=255,cg=255,cb=255;
  switch(s.palette){
    case 0: cr=imageClamp(r+140,0,255);cg=imageClamp(g+140,0,255);cb=imageClamp(b+140,0,255);break;
    case 2:cr=255;cg=188;cb=105;break;
    case 3:cr=120;cg=192;cb=255;break;
    case 4:{uint32_t v=ambientHash(seed);cr=100+(v&127);cg=100+((v>>8)&127);cb=100+((v>>16)&127);break;}
  }
  strength=imageClamp(strength,0,255);
  if(s.blend==0||s.blend==3)return image565(r+(cr-r)*strength/255,g+(cg-g)*strength/255,b+(cb-b)*strength/255);
  if(s.blend==1)return image565(r+(255-r)*cr*strength/65025,g+(255-g)*cg*strength/65025,b+(255-b)*cb*strength/65025);
  return image565(r+cr*strength/255,g+cg*strength/255,b+cb*strength/255);
}
static inline uint16_t ambientLightShade(uint16_t base,int level,int weight,int pulse,const AmbientSettings &s,uint32_t seed){
  int signedPulse=pulse-128;
  int amount=level*weight*abs(signedPulse)/(255*128);
  if(signedPulse>=0)return ambientBlend(base,amount,s,seed);
  // Modulate highlights instead of simply adding white to already-white areas.
  // Limit dimming to 65% to preserve the overall image.
  int keep=255-amount*65/100;
  return image565(imageR(base)*keep/255,imageG(base)*keep/255,imageB(base)*keep/255);
}
// Native pixels shimmer in place. No overlay, color palette or spatial motion.
static inline uint16_t ambientPixelShimmer(uint16_t base,int amount,const AmbientSettings &s,uint32_t seed){
  if(!base||amount<=0)return base;
  int r=imageR(base),g=imageG(base),b=imageB(base);
  int hi=r>g?(r>b?r:b):(g>b?g:b),lo=r<g?(r<b?r:b):(g<b?g:b);
  // Bright colors have little headroom; darker pulses make them visible.
  bool dim=hi>=220||(seed&1);
  int gain=dim?256-amount*175/255:256+amount*220/255;
  int headroom=255*256/hi;
  if(gain>headroom)gain=headroom;
  int dr=0,dg=0,db=0;
  if(hi-lo>=16&&s.chroma){
    int c=amount*s.chroma/160;
    int phase=(seed>>8)%3;
    dr=phase==0?c:-c/2;dg=phase==1?c:-c/2;db=phase==2?c:-c/2;
  }
  // Multiplication preserves black and zero channels; color variation never
  // introduces artificial white highlights or adds a new color to black.
  return image565(r*imageClamp(gain+dr,0,512)/256,
                  g*imageClamp(gain+dg,0,512)/256,
                  b*imageClamp(gain+db,0,512)/256);
}
static inline void ambientPixelRender(const uint16_t *base,uint16_t *out,const AmbientSettings &s,uint32_t t,int level){
  int size=imageClamp(s.size,1,3);
  for(int y=0;y<64;y++)for(int x=0;x<64;x++){
    int i=y*64+x;
    if(!base[i])continue;
    int w=ambientWeight(base,i,s);if(!w)continue;
    uint32_t group=(y/size)*64+x/size;
    uint32_t identity=ambientHash(group+741);
    uint32_t life=1200+identity%2300;
    uint32_t elapsed=t+identity%7000;
    uint32_t cycle=elapsed/life;
    uint32_t seed=ambientHash(identity+cycle*1381);
    if((seed>>16)%100>=(unsigned)(s.density*2))continue;
    int phase=(elapsed%life)*255/life;
    int smooth=ambientEnvelope(phase);
    int triangle=phase<128?phase*2:(255-phase)*2;
    int pulse=(triangle*(100-s.softness)+smooth*s.softness)/100;
    int amount=level*w*pulse/65025;
    out[i]=ambientPixelShimmer(base[i],amount,s,seed);
  }
}
// Render onto a separate output buffer. The original artwork is immutable.
// A deterministic time-based effect avoids persistent particle allocations.
static inline void ambientRender(const uint16_t *base,uint16_t *out,const AmbientSettings &s,uint32_t ms){
  memcpy(out,base,4096*sizeof(uint16_t));
  if(!s.mode||!s.intensity)return;
  uint32_t t=(uint32_t)((uint64_t)ms*s.speed/100);
  int level=s.intensity*255/100;
  if(s.mode==6){ambientPixelRender(base,out,s,t,level);return;}
  if(s.mode==1||s.mode==4){
    int count=s.density;
    for(int p=0;p<count;p++){
      uint32_t offset=ambientHash(p+123)%5000;
      uint32_t life=1800+ambientHash(p+911)%2200;
      uint32_t cycle=(t+offset)/life,age=(t+offset)%life;
      uint32_t seed=ambientHash(cycle*1013+p*137+79);
      int anchor=-1,best=0;
      // A bounded sample of candidates gives luminance-guided spawning.
      for(int k=0;k<6;k++){
        int i=ambientHash(seed+k*197)%4096,w=ambientWeight(base,i,s);
        if(w>best){best=w;anchor=i;}
      }
      if(anchor<0)continue;
      // Luminance selects the anchor and weights each rendered pixel once.
      // Previously the mask was applied twice, making most sparkles invisible.
      int strength=level*ambientEnvelope(age*255/life)/255;
      int x=anchor%64,y=anchor/64;
      if(s.mode==4){
        int travel=(int)(age*10/life)-5;
        if(s.direction==0)x+=travel;else if(s.direction==1)x-=travel;
        else if(s.direction==2)y+=travel;else y-=travel;
      }
      int radius=s.size-1;
      for(int dy=-radius;dy<=radius;dy++)for(int dx=-radius;dx<=radius;dx++){
        int xx=x+dx,yy=y+dy;if(xx<0||xx>63||yy<0||yy>63)continue;
        int distance=abs(dx)+abs(dy);if(distance>radius)continue;
        int i=yy*64+xx,w=ambientWeight(base,i,s);
        int a=strength*w/255;
        if(radius)a=a*(100-s.softness*distance/radius)/100;
        out[i]=ambientBlend(out[i],a,s,seed);
      }
    }
  } else {
    uint32_t phase=t/22;
    for(int y=0;y<64;y++)for(int x=0;x<64;x++){
      int i=y*64+x,w=ambientWeight(base,i,s),pulse=0;
      if(!w)continue;
      if(s.mode==2){
        int axis=s.direction==0?x:s.direction==1?63-x:s.direction==2?y:63-y;
        int width=8+s.density,dist=abs((int)((axis+phase)%128)-64);
        pulse=dist<width?(width-dist)*255/width:0;
      }else if(s.mode==3)pulse=ambientEnvelope((t/24)%256);
      else{
        int dx=abs(x-31),dy=abs(y-31);
        pulse=ambientEnvelope(((dx+dy)*4+(phase%256))&255);
      }
      if(s.blend==3)out[i]=ambientLightShade(base[i],level,w,pulse,s,(uint32_t)i);
      else{
        if(s.softness>0)pulse=(pulse*(100-s.softness)+ambientEnvelope(pulse/2)*s.softness)/100;
        out[i]=ambientBlend(base[i],level*w*pulse/65025,s,(uint32_t)i);
      }
    }
  }
}
