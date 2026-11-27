#pragma once
#include <stdint.h>
#include <string.h>

static inline bool otaParseVersion(const char *p,uint32_t v[3]){
  if(!p)return false;
  for(int i=0;i<3;i++){
    if(*p<'0'||*p>'9')return false;
    uint32_t n=0;
    do{n=n*10+(*p++-'0');if(n>9999)return false;}while(*p>='0'&&*p<='9');
    v[i]=n;if(i<2&&*p++!='.')return false;
  }
  return strcmp(p,"-wiim")==0;
}
static inline bool otaNewer(const char *candidate,const char *current){
  uint32_t a[3],b[3];if(!otaParseVersion(candidate,a)||!otaParseVersion(current,b))return false;
  for(int i=0;i<3;i++){if(a[i]!=b[i])return a[i]>b[i];}
  return false;
}
static inline bool otaHashValid(const char *hash){
  if(!hash||strlen(hash)!=64)return false;
  for(int i=0;i<64;i++)if(!((hash[i]>='0'&&hash[i]<='9')||(hash[i]>='a'&&hash[i]<='f')))return false;
  return true;
}
