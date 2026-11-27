#pragma once
#include <math.h>
#include <stdint.h>

// Local microphone envelope. Nothing from the PCM buffer is sent over Wi-Fi.
// The slowly adapting ceiling keeps quiet and loud listening levels usable.
struct AudioPulse {
  float peak=500.0f, level=0.0f;
  uint32_t updatedAt=0;
  bool started=false;
  void reset(){*this=AudioPulse{};}
  int update(float rms,uint32_t now,int sensitivity,int gate,int attack,int release) {
    uint32_t dt=started?now-updatedAt:16;
    started=true;updatedAt=now;
    if(dt<1)dt=1;if(dt>250)dt=250;
    float signal=fmaxf(0.0f,rms-gate*10.0f);
    peak=fmaxf(500.0f,fmaxf(signal,peak*(1.0f-dt/8000.0f)));
    float target=fminf(1.0f,signal/peak*sensitivity/100.0f);
    float tau=target>level?attack:release;
    level+=(target-level)*(1.0f-expf(-(float)dt/fmaxf(1.0f,tau)));
    return (int)(level*255.0f+0.5f);
  }
};

static inline float stereoMicRms(const int16_t *samples,int n){
  if(n<=0)return 0;
  double sum[2]={0,0},sq[2]={0,0};
  for(int i=0;i<n;i++)for(int c=0;c<2;c++){double v=samples[i*2+c];sum[c]+=v;sq[c]+=v*v;}
  float a=sqrtf(fmaxf(0,sq[0]/n-(sum[0]/n)*(sum[0]/n)));
  float b=sqrtf(fmaxf(0,sq[1]/n-(sum[1]/n)*(sum[1]/n)));
  return fmaxf(a,b);
}
