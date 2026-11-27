#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static constexpr uint16_t PK_MAX_FRAMES=300;
static constexpr size_t PK_MAX_PACKET=2500000;
static inline uint16_t pkRead16(const uint8_t *p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
struct PokemonPacket {
  const uint8_t *bytes=nullptr;size_t length=0;uint8_t width=0,height=0,variant=0;uint16_t frames=0,transparent=0;
  bool parse(const uint8_t *data,size_t size){
    if(!data||size<17||size>PK_MAX_PACKET||memcmp(data,"PKA1",4))return false;
    const uint8_t w=data[4],h=data[5];const uint16_t n=pkRead16(data+6);
    if(!w||!h||w>64||h>64||!n||n>PK_MAX_FRAMES||data[10]>7||data[11]!=(n>1?1:0))return false;
    size_t at=12;unsigned pixels=unsigned(w)*h;
    for(unsigned i=0;i<n;i++){
      if(at+5>size||!pkRead16(data+at))return false;
      const uint16_t len=pkRead16(data+at+2);const uint8_t kind=data[at+4];at+=5;
      if(!len||at+len>size)return false;
      if(kind==0){if(len!=pixels*2)return false;}
      else if(kind==1){if(len%3)return false;unsigned count=0;for(unsigned j=0;j<len;j+=3){if(!data[at+j])return false;count+=data[at+j];}if(count!=pixels)return false;}
      else return false;
      at+=len;
    }
    if(at!=size)return false;
    bytes=data;length=size;width=w;height=h;frames=n;transparent=pkRead16(data+8);variant=data[10];return true;
  }
  size_t offset(uint16_t index)const{
    if(!bytes||index>=frames)return 0;size_t at=12;
    for(unsigned i=0;i<index;i++)at+=5+pkRead16(bytes+at+2);
    return at;
  }
  uint16_t delay(uint16_t index)const{const size_t at=offset(index);return at?pkRead16(bytes+at):100;}
  bool decode(uint16_t index,uint16_t *out)const{
    const size_t at=offset(index);if(!at||!out)return false;
    const uint16_t len=pkRead16(bytes+at+2);const uint8_t *p=bytes+at+5;
    if(bytes[at+4]==0){for(unsigned i=0;i<len/2;i++)out[i]=pkRead16(p+i*2);}
    else {unsigned n=0;for(unsigned i=0;i<len;i+=3){const uint16_t c=pkRead16(p+i+1);for(unsigned j=0;j<p[i];j++)out[n++]=c;}}
    return true;
  }
};

struct PokemonGrid {uint8_t cols,rows;};
static inline PokemonGrid pkGrid(uint8_t layout){
  switch(layout){case 1:return{2,1};case 2:return{2,2};case 3:return{3,3};case 4:return{4,4};case 5:return{1,2};case 6:return{2,3};case 7:return{3,1};case 8:return{1,3};default:return{1,1};}
}
struct PokemonRect{int x,y,w,h;};
static inline PokemonRect pkCell(uint8_t layout,uint8_t index){
  const PokemonGrid g=pkGrid(layout);int x=index%g.cols,y=index/g.cols;
  return{x*64/g.cols,y*64/g.rows,(x+1)*64/g.cols-x*64/g.cols,(y+1)*64/g.rows-y*64/g.rows};
}
static inline uint8_t pkResolution(uint8_t layout){
  const PokemonGrid g=pkGrid(layout);const int div=g.cols>g.rows?g.cols:g.rows;
  return div==1?64:div==2?32:div==3?22:16;
}
