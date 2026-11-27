#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <math.h>
#include "PokemonCatalog.h"
#include "PokemonPacket.h"

struct PokemonKey {uint16_t dex;uint8_t game,variant,size,animated;PokemonKey(uint16_t d=0,uint8_t g=0,uint8_t v=0,uint8_t s=64,uint8_t a=1):dex(d),game(g),variant(v),size(s),animated(a){}};
static inline bool pkSame(const PokemonKey&a,const PokemonKey&b){return a.dex==b.dex&&a.game==b.game&&a.variant==b.variant&&a.size==b.size&&a.animated==b.animated;}
struct PokemonRequest{uint32_t token;uint8_t cell;PokemonKey key;bool alternative;};
struct PokemonResult{PokemonRequest request;uint8_t *data;size_t size;};

// All Pokémon settings are independent of album art and audio reactive modes.
struct PokemonSettings {
  uint32_t magic=0x504b3031;
  int16_t layout=0,game=0,generation=0,type=0,stage=0,legend=0,selection=0,specific=25;
  int16_t shiny=0,shinyChance=8,facing=0,female=0,animatedOnly=1;
  int16_t interval=45,speed=100,loop=0,fps=25,scale=90,x=0,y=0,spacing=1;
  int16_t background=0,cellBackgrounds=0,palette=0,shadow=0,outline=0,glow=0,sparkle=0,staticMotion=0;
  int16_t transition=1,transitionMs=500,together=0,syncAnimations=0,labels=0,brightness=45;
  uint16_t colorA=0x0000,colorB=0x1082;
  uint8_t favorites[(POKEMON_COUNT+8)/8]{};
};
#define PK_FIELDS(X) X(layout,0,8) X(game,0,POKEMON_GAME_COUNT) X(generation,0,7) X(type,0,18) X(stage,0,3) X(legend,0,2) X(selection,0,4) X(specific,1,POKEMON_COUNT) \
  X(shiny,0,2) X(shinyChance,0,100) X(facing,0,2) X(female,0,1) X(animatedOnly,0,1) X(interval,5,1200) X(speed,25,250) X(loop,0,2) X(fps,5,30) \
  X(scale,20,120) X(x,-30,30) X(y,-30,30) X(spacing,0,5) X(background,0,4) X(cellBackgrounds,0,1) X(palette,0,3) X(shadow,0,1) X(outline,0,1) X(glow,0,100) X(sparkle,0,100) X(staticMotion,0,1) \
  X(transition,0,4) X(transitionMs,0,2000) X(together,0,1) X(syncAnimations,0,1) X(labels,0,7) X(brightness,1,255)

struct PokemonSlot {
  PokemonKey key{},target{},previous{},stagedKey{};PokemonPacket packet{};
  uint8_t *data=nullptr,*staged=nullptr;size_t stagedSize=0;uint16_t *pixels=nullptr,*oldPixels=nullptr;
  uint8_t oldW=0,oldH=0;uint16_t oldTransparent=0,index=0;
  uint32_t token=0,frameAt=0,changedAt=0,retryAt=0,transitionAt=0;
  int8_t direction=1;bool pinned=false,pending=false,preload=false,force=false;
};

static const uint16_t PK_TYPE_COLORS[]={0x4208,0xad52,0xe146,0x8bfa,0xa29a,0xe48b,0xc5ab,0x8da2,0x61d4,0x9cf6,0xf9c5,0x337f,0x37a7,0xfe86,0xf99a,0x7edf,0x817f,0x628a,0xf3fd};
static const char *const PK_TYPE_NAMES[]={"All","Normal","Fighting","Flying","Poison","Ground","Rock","Bug","Ghost","Steel","Fire","Water","Grass","Electric","Psychic","Ice","Dragon","Dark","Fairy"};
static inline uint16_t pkMix(uint16_t a,uint16_t b,int alpha){alpha=constrain(alpha,0,255);int q=255-alpha;return uint16_t((((((a>>11)*q+(b>>11)*alpha)/255)<<11)|(((((a>>5)&63)*q+((b>>5)&63)*alpha)/255)<<5)|(((a&31)*q+(b&31)*alpha)/255)));}
static inline uint32_t pkHash(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
static inline String pkEscape(const char *s){String out;for(;*s;s++){if(*s=='"'||*s=='\\')out+='\\';if(uint8_t(*s)>=32)out+=*s;}return out;}

class PokemonMode {
 public:
  PokemonSettings config{};
  volatile bool active=false;
  bool paused=false;
  uint16_t *screen=nullptr;
  uint32_t renderGeneration=0;
  using FetchFn=uint8_t*(*)(const PokemonKey&,size_t&);
  using HashFn=String(*)(const uint8_t*,size_t);
  using DrawFn=void(*)(int,int,uint16_t);

  void begin(Preferences &p){prefs=&p;if(p.getBytesLength("pk_cfg1")==sizeof(config))p.getBytes("pk_cfg1",&config,sizeof(config));
    if(config.magic!=0x504b3031)config=PokemonSettings{};validate();requests=xQueueCreate(16,sizeof(PokemonRequest));results=xQueueCreate(16,sizeof(PokemonResult));}
  void enter(){if(active)return;drain();if(requests)xQueueReset(requests);active=true;paused=false;lastDraw=0;rotationAt=millis();
    screen=(uint16_t*)heap_caps_malloc(8192,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(screen)memset(screen,0,8192);
    for(auto &s:slots){release(s);s.force=true;}lastError=screen?"Loading original sprites":"Not enough memory for Pokémon mode";}
  void exit(){active=false;++serial;if(requests)xQueueReset(requests);drain();for(auto &s:slots)release(s);free(screen);screen=nullptr;}
  int count()const{const auto g=pkGrid(config.layout);return g.cols*g.rows;}
  bool favorite(uint16_t id)const{return id>=1&&id<=POKEMON_COUNT&&(config.favorites[id/8]&(1u<<(id%8)));}
  void save(){if(prefs)prefs->putBytes("pk_cfg1",&config,sizeof(config));}
  void apply(WebServer &server){
    const PokemonSettings before=config;
    if(server.hasArg("reset")){uint8_t fav[sizeof(config.favorites)];memcpy(fav,config.favorites,sizeof(fav));config=PokemonSettings{};memcpy(config.favorites,fav,sizeof(fav));}
#define PK_SET(name,lo,hi) if(server.hasArg(#name))config.name=constrain(server.arg(#name).toInt(),lo,hi);
    PK_FIELDS(PK_SET)
#undef PK_SET
    if(server.hasArg("colorA"))config.colorA=constrain(server.arg("colorA").toInt(),0,65535);
    if(server.hasArg("colorB"))config.colorB=constrain(server.arg("colorB").toInt(),0,65535);
    if(server.hasArg("game")&&config.game&&!POKEMON_GAMES[config.game].animated)config.animatedOnly=0;
    validate();save();
    if(!active)return;
    const bool layout=before.layout!=config.layout;
    const bool selection=layout||before.game!=config.game||before.generation!=config.generation||before.type!=config.type||before.stage!=config.stage||before.legend!=config.legend||before.selection!=config.selection||before.specific!=config.specific||before.shiny!=config.shiny||before.shinyChance!=config.shinyChance||before.facing!=config.facing||before.female!=config.female||before.animatedOnly!=config.animatedOnly;
    if(layout){for(auto &s:slots)release(s);invalidate(false);}
    else if(selection)invalidate(true);
    if(before.interval!=config.interval||before.together!=config.together){rotationAt=millis();for(auto&s:slots)s.changedAt=rotationAt;}
    if(before.syncAnimations!=config.syncAnimations||before.speed!=config.speed||before.loop!=config.loop)for(auto&s:slots)if(s.data){s.index=0;s.direction=1;s.packet.decode(0,s.pixels);s.frameAt=millis();}
    lastDraw=0;
  }

  bool action(const String &action,int cell,int id){
    if(!active)return false;
    if(action=="pause"){paused=!paused;if(!paused){rotationAt=millis();for(auto&s:slots){s.changedAt=rotationAt;s.frameAt=rotationAt;}}return true;}
    if(action=="favorite"&&id>=1&&id<=POKEMON_COUNT){config.favorites[id/8]^=1u<<(id%8);save();return true;}
    if(action=="next"&&cell<0){for(int i=0;i<count();i++)if(!slots[i].pinned)forceNext(i);rotationAt=millis();return true;}
    if(cell<0||cell>=count())return false;auto &s=slots[cell];
    if(action=="pin"){if(!s.key.dex)return false;s.pinned=!s.pinned;if(s.pinned)cancel(s);return true;}
    if(action=="next"){forceNext(cell);return true;}
    if(action=="previous"&&s.previous.dex){const auto old=s.previous;cancel(s);return enqueue(cell,old,false,false);}
    if(action=="select"&&id>=1&&id<=POKEMON_COUNT){PokemonKey key=choose(cell,id);if(!key.dex)return false;cancel(s);return enqueue(cell,key,false,false);}
    return false;
  }

  // Runs on the existing network core; no image decoding or HTTP on the
  // display loop. The same method can read cached assets while disconnected.
  void networkStep(FetchFn fetch,HashFn hash,SemaphoreHandle_t mutex){
    if(!active||!requests||!results)return;PokemonRequest r;
    if(xQueueReceive(requests,&r,0)!=pdTRUE)return;
    size_t n=0;uint8_t *data=readCache(r.key,n,hash,mutex);
    if(data)++cacheHits;else if(fetch){data=fetch(r.key,n);if(data)writeCache(r.key,data,n,hash,mutex);}
    else if(r.alternative){PokemonKey cached=offlineKey(r.key,mutex);if(cached.dex){data=readCache(cached,n,hash,mutex);if(data){r.key=cached;++cacheHits;}}}
    PokemonPacket p;if(data&&(!p.parse(data,n)||(r.key.animated&&p.frames<2))){free(data);data=nullptr;n=0;}
    PokemonResult result{r,data,n};if(!active||xQueueSend(results,&result,pdMS_TO_TICKS(100))!=pdTRUE)free(data);
  }

  void service(DrawFn draw){
    if(!active||!screen)return;const uint32_t now=millis();consume(now);
    const uint32_t interval=uint32_t(config.interval)*1000,lead=min(15000UL,(unsigned long)(interval/2));
    if(!paused){
      // All-together mode stages replacement sprites before the shared deadline.
      if(config.together&&now-rotationAt>=interval-lead){
        bool ready=true;for(int i=0;i<count();i++){auto&s=slots[i];if(s.pinned||!s.data)continue;
          if(!s.staged&&!s.pending&&canRetry(s,now)){auto key=choose(i);if(key.dex)enqueue(i,key,true);}
          if(!s.staged)ready=false;
        }
        if(now-rotationAt>=interval&&(ready||now-rotationAt>=interval+15000)){for(int i=0;i<count();i++)if(!slots[i].pinned&&slots[i].staged)commitStaged(i,now);rotationAt=now;}
      }
      for(int i=0;i<count();i++){auto&s=slots[i];
        if(s.force&&canRetry(s,now)&&!s.pending){auto key=choose(i);if(key.dex){s.force=false;enqueue(i,key,false);}else{s.retryAt=now+5000;lastError="No Pokémon match these filters";}}
        else if(!s.data&&!s.pending&&!s.staged&&canRetry(s,now)){auto key=choose(i);if(key.dex)enqueue(i,key,false);else{s.retryAt=now+5000;lastError="No Pokémon match these filters";}}
        else if(s.data&&!config.together&&!s.pinned&&now-s.changedAt>=interval-lead){
          if(s.staged&&now-s.changedAt>=interval)commitStaged(i,now);
          else if(!s.staged&&!s.pending&&canRetry(s,now)){auto key=choose(i);if(key.dex)enqueue(i,key,true);}
        }
        animate(s,now);
      }
    }
    if(lastDraw&&now-lastDraw<uint32_t(1000/config.fps))return;lastDraw=now;
    render(now);for(int y=0;y<64;y++)for(int x=0;x<64;x++)draw(x,y,screen[y*64+x]);++renderGeneration;
  }

  String status()const{
    String j="{\"paused\":";j+=paused?"true":"false";j+=",\"cache_hits\":"+String(cacheHits)+",\"message\":\""+pkEscape(lastError.c_str())+"\",";
#define PK_STATUS(name,lo,hi) j+="\"" #name "\":"+String(config.name)+",";
    PK_FIELDS(PK_STATUS)
#undef PK_STATUS
    j+="\"colorA\":"+String(config.colorA)+",\"colorB\":"+String(config.colorB)+",\"cells\":[";
    for(int i=0;i<count();i++){const auto&s=slots[i];if(i)j+=',';j+="{\"id\":"+String(s.key.dex)+",\"game\":"+String(s.key.game)+",\"variant\":"+String(s.packet.variant)+",\"frames\":"+String(s.packet.frames)+",\"name\":\""+pkEscape(s.key.dex?POKEMON_SPECIES[s.key.dex].name:"")+"\",\"pinned\":"+String(s.pinned?"true":"false")+",\"pending\":"+String(s.pending?"true":"false")+",\"favorite\":"+String(favorite(s.key.dex)?"true":"false")+"}";}
    j+="],\"favorites\":[";bool first=true;for(uint16_t i=1;i<=POKEMON_COUNT;i++)if(favorite(i)){if(!first)j+=',';j+=String(i);first=false;}j+="]}";return j;
  }
  void catalog(WebServer &server){
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);server.send(200,"application/json","[{\"id\":0,\"name\":\"All\"}");
    String batch;batch.reserve(1600);for(uint16_t i=1;i<=POKEMON_COUNT;i++){const auto&s=POKEMON_SPECIES[i];batch+=",{\"id\":"+String(i)+",\"name\":\""+pkEscape(s.name)+"\",\"gen\":"+String(s.generation)+",\"type1\":"+String(s.type1)+",\"type2\":"+String(s.type2)+",\"family\":"+String(s.family)+"}";if(batch.length()>1300){server.sendContent(batch);batch="";}}batch+=']';server.sendContent(batch);server.sendContent("");
  }

 private:
  Preferences *prefs=nullptr;QueueHandle_t requests=nullptr,results=nullptr;PokemonSlot slots[16]{};
  uint32_t serial=0,lastDraw=0,rotationAt=0,cacheHits=0;uint16_t sequential=0;String lastError;
  static constexpr size_t CACHE_LIMIT=4*1024*1024,CACHE_RESERVE=2*1024*1024;
  void validate(){
#define PK_CLAMP(name,lo,hi) config.name=constrain(config.name,lo,hi);
    PK_FIELDS(PK_CLAMP)
#undef PK_CLAMP
  }
  void release(PokemonSlot&s){free(s.data);free(s.staged);free(s.pixels);free(s.oldPixels);s=PokemonSlot{};s.token=++serial;}
  void cancel(PokemonSlot&s){s.token=++serial;s.pending=false;s.preload=false;free(s.staged);s.staged=nullptr;s.stagedSize=0;s.stagedKey=PokemonKey{};s.retryAt=0;}
  void forceNext(int i){auto&s=slots[i];if(s.staged){commitStaged(i,millis());return;}auto key=choose(i);cancel(s);if(key.dex)enqueue(i,key,false);else{s.force=true;lastError="No Pokémon match these filters";}}
  void invalidate(bool keep){if(requests)xQueueReset(requests);for(int i=0;i<16;i++){auto&s=slots[i];cancel(s);if(!keep||!s.pinned){s.force=true;s.changedAt=millis();}}rotationAt=millis();}
  void drain(){if(!results)return;PokemonResult r;while(xQueueReceive(results,&r,0)==pdTRUE)free(r.data);}
  static bool canRetry(const PokemonSlot&s,uint32_t now){return !s.retryAt||int32_t(now-s.retryAt)>=0;}
  bool gameEligible(int game,int id)const{if(game<1||game>POKEMON_GAME_COUNT||id<1||id>POKEMON_COUNT)return false;const auto&g=POKEMON_GAMES[game];
    if(id>g.maxDex&&!(game==16&&(id==808||id==809)))return false;if(config.animatedOnly&&(!g.animated||(config.facing==1&&!g.backAnimated)))return false;
    if(config.shiny==1&&!g.shiny)return false;return true;}
  bool speciesEligible(int id)const{const auto&s=POKEMON_SPECIES[id];if(config.generation&&s.generation!=config.generation)return false;if(config.type&&s.type1!=config.type&&s.type2!=config.type)return false;
    if(config.stage&&s.stage!=config.stage)return false;if(config.legend==1&&!(s.flags&1))return false;if(config.legend==2&&(s.flags&1))return false;
    if(config.selection==3&&!favorite(id))return false;if(config.selection==4&&s.family!=POKEMON_SPECIES[config.specific].family)return false;return true;}
  PokemonKey keyFor(int id,int cell){uint8_t options[POKEMON_GAME_COUNT];int n=0;for(int g=1;g<=POKEMON_GAME_COUNT;g++)if((!config.game||config.game==g)&&gameEligible(g,id))options[n++]=g;
    if(!n)return{};int game=options[esp_random()%n];if(config.selection==2&&!config.game&&n>1){for(int k=0;k<n;k++){const int g=options[(cell+k)%n];bool used=false;for(int j=0;j<count();j++)if(j!=cell&&(slots[j].key.game==g||slots[j].target.game==g)){used=true;break;}if(!used){game=g;break;}}}
    const auto&g=POKEMON_GAMES[game];uint8_t variant=0;if(g.shiny&&(config.shiny==1||(config.shiny==2&&esp_random()%100<unsigned(config.shinyChance))))variant|=1;
    if(config.facing==1||(config.facing==2&&(!config.animatedOnly||g.backAnimated)&&(config.layout==1?cell==0:esp_random()%2)))variant|=2;
    if(config.female&&g.female&&(POKEMON_SPECIES[id].flags&2))variant|=4;
    return PokemonKey{uint16_t(id),uint8_t(game),variant,pkResolution(config.layout),uint8_t(config.animatedOnly)};
  }
  PokemonKey choose(int cell,int explicitId=0){if(explicitId||config.selection==2)return keyFor(explicitId?explicitId:config.specific,cell);
    const int start=config.selection==1?(sequential%POKEMON_COUNT)+1:esp_random()%POKEMON_COUNT+1;
    PokemonKey fallback{};for(int k=0;k<POKEMON_COUNT;k++){int id=(start-1+k)%POKEMON_COUNT+1;if(!speciesEligible(id))continue;auto key=keyFor(id,cell);if(!key.dex)continue;
      bool duplicate=false;for(int j=0;j<count();j++)if(slots[j].key.dex==id||(slots[j].pending&&slots[j].target.dex==id)||slots[j].stagedKey.dex==id){duplicate=true;break;}
      if(!fallback.dex||fallback.dex==slots[cell].key.dex)fallback=key;
      if(!duplicate){sequential=id;return key;}}
    return fallback;
  }
  bool enqueue(int i,PokemonKey key,bool preload,bool alternative=true){if(!requests||!key.dex)return false;auto&s=slots[i];PokemonRequest r{++serial,uint8_t(i),key,alternative&&config.selection!=2};s.token=r.token;s.target=key;s.preload=preload;s.pending=xQueueSend(requests,&r,0)==pdTRUE;
    if(!s.pending)s.retryAt=millis()+500;return s.pending;}
  void consume(uint32_t now){if(!results)return;PokemonResult r;while(xQueueReceive(results,&r,0)==pdTRUE){if(r.request.cell>=count()){free(r.data);continue;}auto&s=slots[r.request.cell];
    if(r.request.token!=s.token){free(r.data);continue;}s.pending=false;
    if(!r.data){s.retryAt=now+2200;lastError="Original sprite unavailable; trying another";continue;}
    if(s.preload){free(s.staged);s.staged=r.data;s.stagedSize=r.size;s.stagedKey=r.request.key;}
    else install(s,r.request.key,r.data,r.size,now);
  }}
  void commitStaged(int i,uint32_t now){auto&s=slots[i];auto d=s.staged;auto n=s.stagedSize;auto k=s.stagedKey;s.staged=nullptr;s.stagedSize=0;s.stagedKey=PokemonKey{};if(d)install(s,k,d,n,now);}
  void install(PokemonSlot&s,PokemonKey key,uint8_t*data,size_t n,uint32_t now){PokemonPacket p;if(!p.parse(data,n)){free(data);s.retryAt=now+2500;return;}
    uint16_t *pixels=(uint16_t*)heap_caps_malloc(p.width*p.height*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!pixels){free(data);s.retryAt=now+5000;lastError="Sprite memory busy";return;}
    p.decode(0,pixels);free(s.oldPixels);s.oldPixels=nullptr;if(s.pixels&&config.transition&&config.transitionMs){s.oldPixels=s.pixels;s.oldW=s.packet.width;s.oldH=s.packet.height;s.oldTransparent=s.packet.transparent;}else free(s.pixels);
    free(s.data);s.previous=s.key;s.key=key;s.data=data;s.packet=p;s.pixels=pixels;s.index=0;s.direction=1;s.frameAt=now;s.changedAt=now;s.transitionAt=now;s.retryAt=0;s.force=false;lastError="Original game sprites";
  }
  void animate(PokemonSlot&s,uint32_t now){if(!s.data||s.packet.frames<=1)return;for(int tries=0;tries<8;tries++){
    const uint32_t delay=max(10UL,uint32_t(s.packet.delay(s.index))*100UL/uint32_t(config.speed));if(now-s.frameAt<delay)return;s.frameAt+=delay;
    if(config.loop==1&&s.index+1>=s.packet.frames){s.frameAt=now;return;}
    if(config.loop==2){int next=int(s.index)+s.direction;if(next<0||next>=s.packet.frames){s.direction=-s.direction;next=int(s.index)+s.direction;}s.index=next;}
    else s.index=(s.index+1)%s.packet.frames;s.packet.decode(s.index,s.pixels);
  }s.frameAt=now;}
  uint16_t bg(const PokemonSlot&s,int y,const PokemonRect&r)const{if(config.background==0)return 0;if(config.background==1)return config.colorA;if(config.background==3)return pkMix(config.colorA,config.colorB,(y-r.y)*255/max(1,r.h-1));
    if(config.background==4)return y-r.y>r.h*3/4?pkMix(config.colorA,config.colorB,100):config.colorA;const uint16_t id=config.cellBackgrounds?s.key.dex:slots[0].key.dex;return id?pkMix(0,PK_TYPE_COLORS[min(18,int(POKEMON_SPECIES[id].type1))],75):0;}
  uint16_t palette(uint16_t c)const{if(!config.palette)return c;int l=((c>>11)*77*8+((c>>5)&63)*150*4+(c&31)*29*8)>>8;
    if(config.palette==1)return pkMix(0x0861,0xb7e0,l);if(config.palette==2)return pkMix(0,0xffff,l);return c^0xffff;}
  void pixel(int x,int y,uint16_t c,int opacity=255){if(x>=0&&x<64&&y>=0&&y<64)screen[y*64+x]=pkMix(screen[y*64+x],c,opacity);}
  void sprite(const PokemonRect&r,const uint16_t*p,int w,int h,uint16_t t,int shift,int opacity,int reveal,bool incoming,int ybob){if(!p||!w||!h)return;
    const int margin=config.spacing,labels=config.labels?6:0,aw=max(1,r.w-2*margin),ah=max(1,r.h-2*margin-labels);
    const float f=min(aw/float(w),ah/float(h))*config.scale/100.f;int dw=max(1,int(w*f+.5f)),dh=max(1,int(h*f+.5f));
    int x0=r.x+(r.w-dw)/2+r.w*config.x/100+shift,y0=r.y+(r.h-labels-dh)/2+r.h*config.y/100+ybob;
    if(config.shadow&&incoming){const int sy=constrain(y0+dh,r.y,r.y+r.h-labels-1),cx=x0+dw/2,rx=max(1,dw/3);for(int x=max(r.x,cx-rx);x<min(r.x+r.w,cx+rx);x++)pixel(x,sy,0,100);}
    for(int y=max(r.y,y0);y<min(r.y+r.h-labels,y0+dh);y++)for(int x=max(r.x,x0);x<min(r.x+r.w,x0+dw);x++){
      if(reveal>=0){const int gate=config.transition==4?min(255,((abs(x-r.x-r.w/2)+abs(y-r.y-r.h/2))*255)/max(1,(r.w+r.h)/2)):int(pkHash(x+y*64)&255);if((incoming&&gate>=reveal)||(!incoming&&gate<reveal))continue;}
      int sx=min(w-1,((x-x0)*w)/dw),sy=min(h-1,((y-y0)*h)/dh);const uint16_t c=p[sy*w+sx];
      if(c!=t)pixel(x,y,palette(c),opacity);
      else if((config.outline||config.glow)&&incoming){bool near=(sx>0&&p[sy*w+sx-1]!=t)||(sx+1<w&&p[sy*w+sx+1]!=t)||(sy>0&&p[(sy-1)*w+sx]!=t)||(sy+1<h&&p[(sy+1)*w+sx]!=t);if(near)pixel(x,y,config.outline?0xffff:PK_TYPE_COLORS[1],config.outline?opacity:opacity*config.glow/180);}
    }
  }
  static uint16_t glyph(char c){static const uint16_t digits[]={0x7b6f,0x2492,0x73e7,0x73cf,0x5bc9,0x79cf,0x79ef,0x7249,0x7bef,0x7bcf};
    static const uint16_t letters[]={0x2bed,0x6bae,0x7927,0x6b6e,0x79e7,0x79e4,0x796f,0x5bed,0x7497,0x124e,0x5bad,0x4927,0x7fed,0x6f6d,0x7b6f,0x7be4,0x7b7b,0x7bad,0x79cf,0x7492,0x5b6f,0x5b6a,0x5bfd,0x5aad,0x5a92,0x72a7};
    if(c>='0'&&c<='9')return digits[c-'0'];if(c>='a'&&c<='z')c-=32;if(c>='A'&&c<='Z')return letters[c-'A'];return 0;}
  void label(const PokemonRect&r,uint16_t dex){if(!config.labels||!dex)return;char text[28]{};const auto&s=POKEMON_SPECIES[dex];
    if(r.w<26||config.labels&2)snprintf(text,sizeof(text),"%03u",dex);else if(config.labels&1)strlcpy(text,s.name,sizeof(text));else if(config.labels&4)strlcpy(text,PK_TYPE_NAMES[min(18,int(s.type1))],sizeof(text));
    const int chars=min(int(strlen(text)),max(1,r.w/4));int x0=r.x+(r.w-chars*4+1)/2,y0=r.y+r.h-6;
    for(int y=y0;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++)pixel(x,y,0,150);
    for(int i=0;i<chars;i++){uint16_t g=glyph(text[i]);for(int y=0;y<5;y++)for(int x=0;x<3;x++)if(g&(1u<<(14-y*3-x)))pixel(x0+i*4+x,y0+y,0xffff);}
  }
  void render(uint32_t now){for(int i=0;i<count();i++){auto&s=slots[i];const auto r=pkCell(config.layout,i);
    for(int y=r.y;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++)screen[y*64+x]=bg(s,y,r);
    int p=config.transitionMs?min(255UL,(now-s.transitionAt)*255UL/uint32_t(config.transitionMs)):255;
    if(p>=255&&s.oldPixels){free(s.oldPixels);s.oldPixels=nullptr;}
    if(s.oldPixels&&config.transition){const int slide=config.transition==3?-(p*r.w/255):0;const int opacity=config.transition==1?255-p:255;const int reveal=config.transition>=2&&config.transition!=3?p:-1;sprite(r,s.oldPixels,s.oldW,s.oldH,s.oldTransparent,slide,opacity,reveal,false,0);}
    const int slide=s.oldPixels&&config.transition==3?(255-p)*r.w/255:0,opacity=s.oldPixels&&config.transition==1?p:255,reveal=s.oldPixels&&(config.transition==2||config.transition==4)?p:-1;
    int bob=(config.staticMotion&&s.packet.frames==1)?int(sinf(now/600.f)*max(1,r.h/20)):0;
    sprite(r,s.pixels,s.packet.width,s.packet.height,s.packet.transparent,slide,opacity,reveal,true,bob);
    if(config.sparkle&&s.data){for(int k=0;k<max(1,config.sparkle/20);k++){uint32_t h=pkHash(uint32_t(i*123+k*97+now/280));int x=r.x+h%r.w,y=r.y+(h>>16)%r.h;pixel(x,y,0xffff,config.sparkle*2);}}
    if(!s.data&&s.pending)pixel(r.x+r.w/2,r.y+r.h/2,0x7bef,(now/180)%2?220:60);label(r,s.key.dex);
  }}
  static String cachePath(const PokemonKey&k){return "/pk/"+String(k.game)+"-"+String(k.dex)+"-"+String(k.variant)+"-"+String(k.size)+"-"+String(k.animated)+".pka";}
  PokemonKey offlineKey(const PokemonKey&requested,SemaphoreHandle_t mutex){PokemonKey chosen{};unsigned matches=0;if(mutex&&xSemaphoreTake(mutex,pdMS_TO_TICKS(500))!=pdTRUE)return chosen;
    File dir=LittleFS.open("/pk");if(dir){File f=dir.openNextFile();while(f){String name=f.name();int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);int g=0,id=0,v=0,sz=0,a=0,n=0;
      if(!f.isDirectory()&&sscanf(name.c_str(),"%d-%d-%d-%d-%d.pka%n",&g,&id,&v,&sz,&a,&n)==5&&n==int(name.length())&&id>=1&&id<=POKEMON_COUNT&&v>=0&&v<=7&&sz==requested.size&&a==requested.animated&&(!config.game||g==config.game)&&gameEligible(g,id)&&speciesEligible(id)){
        bool variantOk=(config.shiny!=0||!(v&1))&&(config.shiny!=1||(v&1))&&(config.facing!=0||!(v&2))&&(config.facing!=1||(v&2));
        if(variantOk&&esp_random()%++matches==0)chosen=PokemonKey(id,g,v,sz,a);
      }f.close();f=dir.openNextFile();}dir.close();}
    if(mutex)xSemaphoreGive(mutex);return chosen;}
  uint8_t *readCache(const PokemonKey&k,size_t &n,HashFn hash,SemaphoreHandle_t mutex){n=0;if(mutex&&xSemaphoreTake(mutex,pdMS_TO_TICKS(500))!=pdTRUE)return nullptr;
    const String path=cachePath(k);File f=LittleFS.open(path,"r");uint8_t *data=nullptr;char digest[65]{};
    if(f&&f.size()>64&&f.size()<=PK_MAX_PACKET+64&&f.read((uint8_t*)digest,64)==64){n=f.size()-64;data=(uint8_t*)heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(data&&f.read(data,n)!=n){free(data);data=nullptr;}}
    if(f)f.close();if(mutex)xSemaphoreGive(mutex);PokemonPacket p;if(data&&(!p.parse(data,n)||hash(data,n)!=String(digest))){free(data);data=nullptr;}if(!data)n=0;return data;}
  void writeCache(const PokemonKey&k,const uint8_t*data,size_t n,HashFn hash,SemaphoreHandle_t mutex){if(!data||n+64>CACHE_LIMIT||!hash)return;if(mutex&&xSemaphoreTake(mutex,pdMS_TO_TICKS(500))!=pdTRUE)return;
    LittleFS.mkdir("/pk");const String path=cachePath(k);size_t total=0;String oldest;time_t oldestTime=0;
    auto scan=[&](){total=0;oldest="";oldestTime=0;File dir=LittleFS.open("/pk");if(!dir)return;File f=dir.openNextFile();while(f){String name=f.name();int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);String full="/pk/"+name;
      if(!f.isDirectory()&&full!=path&&name.endsWith(".pka")){total+=f.size();time_t t=f.getLastWrite();if(!oldest.length()||t<oldestTime){oldest=full;oldestTime=t;}}f.close();f=dir.openNextFile();}dir.close();};
    scan();for(int tries=0;tries<100&&oldest.length()&&(total+n+64>CACHE_LIMIT||LittleFS.totalBytes()-LittleFS.usedBytes()<n+CACHE_RESERVE);tries++){if(!LittleFS.remove(oldest))break;scan();}
    if(total+n+64<=CACHE_LIMIT&&LittleFS.totalBytes()-LittleFS.usedBytes()>=n+CACHE_RESERVE){const String tmp="/pk/.part";File f=LittleFS.open(tmp,"w");const String digest=hash(data,n);if(f){bool ok=f.write((const uint8_t*)digest.c_str(),64)==64&&f.write(data,n)==n;f.close();if(ok){LittleFS.remove(path);LittleFS.rename(tmp,path);}else LittleFS.remove(tmp);}}
    if(mutex)xSemaphoreGive(mutex);
  }
};
