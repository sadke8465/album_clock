#pragma once
#include "FaceZoom.h"
#include <stdlib.h>

struct SmartRegions { int textCount=0; FaceCrop text[TEXT_MAX]{}; int detailOrder[4]{0,1,2,3}; };

// A bounded, non-OCR text-line heuristic. Looks for repeated high-contrast
// vertical strokes arranged in a horizontal band; it deliberately rejects
// isolated edges and very tall texture regions.
static inline SmartRegions analyzeSmartRegions(const uint8_t *gray,int n) {
  SmartRegions out;
  if(!gray||n<32||n>128)return out;
  int right[128]{},left[128]{},runs[128]{};
  for(int y=1;y<n-1;y++){
    left[y]=n;right[y]=0;
    for(int x=1;x<n-1;x++)if(abs((int)gray[y*n+x+1]-gray[y*n+x-1])>=48){
      if(x<left[y])left[y]=x;if(x>right[y])right[y]=x;
      runs[y]++;
    }
    if(runs[y]<6||right[y]-left[y]<n/10)runs[y]=0;
  }
  struct Candidate { FaceCrop crop; int score; };
  Candidate best[TEXT_MAX]{};
  for(int y=1;y<n-1;){
    if(!runs[y]){y++;continue;}
    int top=y,bottom=y,x0=n,x1=0,total=0,rows=0;
    while(y<n-1 && (runs[y]||(y+1<n-1&&runs[y+1]))){
      if(runs[y]){bottom=y;x0=left[y]<x0?left[y]:x0;x1=right[y]>x1?right[y]:x1;total+=runs[y];rows++;}
      y++;
    }
    int h=bottom-top+1,w=x1-x0+1;
    if(h<3||h>n/3||rows<3||w<h*2||total<rows*7)continue;
    int extent=(w+8)*256/n;
    FaceCrop crop=centeredCrop((x0+x1)*128/n,(top+bottom)*128/n,zoomClamp(extent,96,224));
    int score=total*4/(h+1)+w;
    for(int i=0;i<TEXT_MAX;i++)if(score>best[i].score){
      for(int j=TEXT_MAX-1;j>i;j--)best[j]=best[j-1];
      best[i]={crop,score};break;
    }
  }
  for(int i=0;i<TEXT_MAX;i++)if(best[i].score>0)out.text[out.textCount++]=best[i].crop;
  // Rank the four gentle quadrant crops by edge energy. Stable tie order
  // avoids random movement on plain covers.
  int energy[4]{};
  for(int q=0;q<4;q++){
    FaceCrop c=quadrantCrop(q);
    int x0=c.x*n/256,y0=c.y*n/256,x1=(c.x+c.side)*n/256,y1=(c.y+c.side)*n/256;
    for(int y=y0;y<y1-1;y++)for(int x=x0;x<x1-1;x++){
      int v=gray[y*n+x];energy[q]+=abs(v-gray[y*n+x+1])+abs(v-gray[(y+1)*n+x]);
    }
  }
  for(int i=1;i<4;i++){int q=out.detailOrder[i],j=i;while(j>0&&energy[q]>energy[out.detailOrder[j-1]]){out.detailOrder[j]=out.detailOrder[j-1];j--;}out.detailOrder[j]=q;}
  return out;
}
