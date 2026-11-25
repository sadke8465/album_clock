#pragma once
#include <string>
#include <stdint.h>

// WiiM's player-status endpoint sometimes HTML-escapes otherwise identical
// UTF-8 metadata. Decode a single entity layer, leaving unknown entities and
// malformed numeric references untouched.
static inline void appendWiimUtf8(std::string &out,uint32_t cp) {
  if(cp<=0x7f)out+=(char)cp;
  else if(cp<=0x7ff){out+=(char)(0xc0|(cp>>6));out+=(char)(0x80|(cp&63));}
  else if(cp<=0xffff){out+=(char)(0xe0|(cp>>12));out+=(char)(0x80|((cp>>6)&63));out+=(char)(0x80|(cp&63));}
  else {out+=(char)(0xf0|(cp>>18));out+=(char)(0x80|((cp>>12)&63));out+=(char)(0x80|((cp>>6)&63));out+=(char)(0x80|(cp&63));}
}
static inline bool wiimEntity(const std::string &entity,uint32_t &cp) {
  if(entity=="amp")cp='&';
  else if(entity=="quot")cp='"';
  else if(entity=="apos")cp='\'';
  else if(entity=="lt")cp='<';
  else if(entity=="gt")cp='>';
  else if(entity=="nbsp")cp=' ';
  else if(entity=="ndash")cp=0x2013;
  else if(entity=="mdash")cp=0x2014;
  else if(entity=="lsquo")cp=0x2018;
  else if(entity=="rsquo")cp=0x2019;
  else if(entity=="ldquo")cp=0x201c;
  else if(entity=="rdquo")cp=0x201d;
  else if(entity.size()>1&&entity[0]=='#'){
    size_t pos=1;unsigned base=10;cp=0;
    if(entity[pos]=='x'||entity[pos]=='X'){base=16;pos++;}
    if(pos==entity.size())return false;
    for(;pos<entity.size();pos++){
      char c=entity[pos];int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
      if(d<0||(unsigned)d>=base||cp>(0x10ffffu-d)/base)return false;
      cp=cp*base+d;
    }
    if(cp<32||cp==127||(cp>=0xd800&&cp<=0xdfff))return false;
    if(cp==160)cp=' ';
  } else return false;
  return true;
}
static inline std::string decodeWiimEntities(const std::string &s) {
  std::string out;out.reserve(s.size());
  for(size_t i=0;i<s.size();i++){
    if(s[i]=='&'){
      size_t end=s.find(';',i+1);uint32_t cp;
      if(end!=std::string::npos&&end-i<=16&&wiimEntity(s.substr(i+1,end-i-1),cp)){
        appendWiimUtf8(out,cp);i=end;continue;
      }
    }
    out+=s[i];
  }
  return out;
}
static inline std::string trimWiimMetadata(const std::string &s) {
  size_t first=s.find_first_not_of(" \t\r\n"),last=s.find_last_not_of(" \t\r\n");
  return first==std::string::npos?"":s.substr(first,last-first+1);
}
static inline bool wiimFieldMatches(const std::string &meta,const std::string &status) {
  std::string a=trimWiimMetadata(meta),b=trimWiimMetadata(status);
  if(a.empty()||b.empty())return false;
  if(a==b)return true;
  std::string decoded=trimWiimMetadata(decodeWiimEntities(b));
  return a==decoded||trimWiimMetadata(decodeWiimEntities(a))==decoded;
}
