import { parseGIF, decompressFrame } from 'gifuct-js';
import UPNG from 'upng-js';
import { POKEMON_GAMES as games } from './pokemon-games.ts';

export const POKEMON_SIZES=[16,22,32,64] as const;
export const MAX_FRAMES=300;
export const MAX_PACKET=2_500_000;
const SOURCE='https://raw.githubusercontent.com/PokeAPI/sprites/c0542f7c51215d9672439012838c99f520596cbd/sprites/pokemon/versions/';
type Asset={url:string,variant:number,animated:boolean};
export type RGBAFrame={rgba:Uint8Array,delay:number};
export type SpriteFrames={width:number,height:number,frames:RGBAFrame[]};

export function pokemonAssets(gameId:number,id:number,variant:number,animatedOnly:boolean):Asset[]{
  const g=games.find(x=>x.id===gameId);
  if(!g||!Number.isInteger(id)||id<1||(id>g.max&&!g.extra?.includes(id))||!Number.isInteger(variant)||variant<0||variant>7)return [];
  if((variant&1)&&!g.shiny)return [];
  const back=!!(variant&2),female=!!(variant&4),shiny=!!(variant&1);
  const out:Asset[]=[];
  const add=(base:string,extension:string,actual:number,animated:boolean)=>{
    if(!extension)return;
    const dirs=[actual&2?'back':'',actual&1?'shiny':'',actual&4?'female':''].filter(Boolean).join('/');
    const url=SOURCE+g.path+'/'+base+(dirs?dirs+'/':'')+id+'.'+extension;
    if(!out.some(x=>x.url===url))out.push({url,variant:actual,animated});
  };
  const variants=female&&g.female?[variant,variant&~4]:[variant&~4];
  if(g.animated&&(!back||g.backAnimated))for(const v of variants)add(g.animatedAtRoot?'':'animated/',g.animated,v,true);
  if(!animatedOnly&&g.static){
    for(const v of variants){
      if(g.transparent)add('transparent/',g.static,v,false);
      add('',g.static,v,false);
    }
  }
  return out;
}

function bounds(width:number,height:number,count:number){
  if(!width||!height||width>512||height>512||count<1||count>MAX_FRAMES||width*height*count>16_000_000)throw Error('sprite dimensions/frame budget exceeded');
}

// GIF disposal and transparent patches are composed before resizing. APNG
// retains its original frame durations, including the GBA/DS entrance sprites.
export function decodePokemonAsset(bytes:ArrayBuffer):SpriteFrames{
  const b=new Uint8Array(bytes);
  if(b[0]===71&&b[1]===73&&b[2]===70){
    const gif=parseGIF(bytes),width=gif.lsd.width,height=gif.lsd.height;
    const frames=gif.frames.filter(f=>'image'in f);bounds(width,height,frames.length);
    const rgba=new Uint8Array(width*height*4),result:RGBAFrame[]=[];
    for(const raw of frames){
      if(!('image'in raw))continue;
      const f=decompressFrame(raw,gif.gct,true),d=f.dims;
      if(d.left<0||d.top<0||d.width<1||d.height<1||d.left+d.width>width||d.top+d.height>height)throw Error('invalid GIF rect');
      const restore=f.disposalType===3?rgba.slice():null;
      for(let y=0;y<d.height;y++)for(let x=0;x<d.width;x++){
        const p=(y*d.width+x)*4,q=((d.top+y)*width+d.left+x)*4;
        if(f.patch[p+3]){rgba[q]=f.patch[p];rgba[q+1]=f.patch[p+1];rgba[q+2]=f.patch[p+2];rgba[q+3]=f.patch[p+3];}
      }
      result.push({rgba:rgba.slice(),delay:f.delay||100});
      if(restore)rgba.set(restore);
      else if(f.disposalType===2)for(let y=0;y<d.height;y++)rgba.fill(0,((d.top+y)*width+d.left)*4,((d.top+y)*width+d.left+d.width)*4);
    }
    return {width,height,frames:result};
  }
  if(b[0]===137&&b[1]===80&&b[2]===78&&b[3]===71){
    // Inspect IHDR and acTL before UPNG allocates large buffers.
    const view=new DataView(bytes),width=view.getUint32(16),height=view.getUint32(20);let count=1;
    for(let at=8;at+12<=b.length;){const len=view.getUint32(at);if(len>b.length-at-12)throw Error('malformed PNG');
      if(b[at+4]===97&&b[at+5]===99&&b[at+6]===84&&b[at+7]===76&&len>=8)count=view.getUint32(at+8);
      at+=len+12;
    }
    bounds(width,height,count);
    const image=UPNG.decode(bytes),frames=UPNG.toRGBA8(image);bounds(width,height,frames.length);
    return {width,height,frames:frames.map((raw,i)=>({rgba:new Uint8Array(raw),delay:image.frames?.[i]?.delay||100}))};
  }
  throw Error('unsupported sprite source');
}

// PKA1: common union crop, RGB565 + a reserved transparent code. A frame is
// either raw LE words or run-length encoded count/word triples. Delay is ms.
export function encodePokemonPacket(sprite:SpriteFrames,size:number,variant:number):Uint8Array{
  const {width,height,frames}=sprite;bounds(width,height,frames.length);
  if(!POKEMON_SIZES.includes(size as typeof POKEMON_SIZES[number]))throw Error('bad resolution');
  let left=width,top=height,right=-1,bottom=-1;
  for(const f of frames){if(f.rgba.length!==width*height*4)throw Error('bad pixels');
    for(let y=0;y<height;y++)for(let x=0;x<width;x++)if(f.rgba[(y*width+x)*4+3]>=128){left=Math.min(left,x);top=Math.min(top,y);right=Math.max(right,x);bottom=Math.max(bottom,y);}
  }
  if(right<left)throw Error('empty sprite');
  const cw=right-left+1,ch=bottom-top+1,scale=Math.min(1,size/Math.max(cw,ch));
  const w=Math.max(1,Math.round(cw*scale)),h=Math.max(1,Math.round(ch*scale));
  const used=new Uint8Array(65536),sampled:{pixels:Uint16Array;opaque:Uint8Array;delay:number}[]=[];
  for(const f of frames){
    const pixels=new Uint16Array(w*h),opaque=new Uint8Array(w*h);
    for(let y=0;y<h;y++)for(let x=0;x<w;x++){
      const sx=left+Math.min(cw-1,Math.floor((x+.5)*cw/w)),sy=top+Math.min(ch-1,Math.floor((y+.5)*ch/h));
      const p=(sy*width+sx)*4,i=y*w+x;if(f.rgba[p+3]<128)continue;
      const c=((f.rgba[p]&248)<<8)|((f.rgba[p+1]&252)<<3)|(f.rgba[p+2]>>3);pixels[i]=c;opaque[i]=1;used[c]=1;
    }
    sampled.push({pixels,opaque,delay:Math.min(65535,Math.max(10,Math.round(f.delay||100)))});
  }
  let transparent=used[1]?used.findIndex(v=>!v):1;if(transparent<0)throw Error('no transparent code');
  const encoded:{delay:number;data:Uint8Array;kind:number}[]=[];
  for(const f of sampled){
    const raw=new Uint8Array(w*h*2),rle:number[]=[];let last=-1,run=0;
    const flush=()=>{if(run){rle.push(run,last&255,last>>8);run=0;}};
    for(let i=0;i<f.pixels.length;i++){
      const c=f.opaque[i]?f.pixels[i]:transparent;raw[i*2]=c&255;raw[i*2+1]=c>>8;
      if(c!==last||run===255){flush();last=c;}run++;
    }flush();
    encoded.push({delay:f.delay,data:rle.length<raw.length?Uint8Array.from(rle):raw,kind:rle.length<raw.length?1:0});
  }
  const total=12+encoded.reduce((n,f)=>n+5+f.data.length,0);if(total>MAX_PACKET)throw Error('packet too large');
  const packet=new Uint8Array(total),v=new DataView(packet.buffer);packet.set([80,75,65,49,w,h]);
  v.setUint16(6,frames.length,true);v.setUint16(8,transparent,true);packet[10]=variant;packet[11]=frames.length>1?1:0;
  let at=12;for(const f of encoded){v.setUint16(at,f.delay,true);v.setUint16(at+2,f.data.length,true);packet[at+4]=f.kind;packet.set(f.data,at+5);at+=5+f.data.length;}
  return packet;
}

export async function preparePokemonSprite(gameId:number,id:number,variant:number,size:number,animatedOnly:boolean):Promise<Uint8Array|null>{
  if(!POKEMON_SIZES.includes(size as typeof POKEMON_SIZES[number]))return null;
  for(const asset of pokemonAssets(gameId,id,variant,animatedOnly)){
    const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),6500);
    try{
      const response=await fetch(asset.url,{signal:controller.signal,headers:{Accept:'image/gif,image/png'},cf:{cacheEverything:true,cacheTtl:86400}});
      if(!response.ok)continue;
      const content=await response.arrayBuffer();if(content.byteLength>6_000_000)continue;
      const source=decodePokemonAsset(content);if(animatedOnly&&source.frames.length<2)continue;
      return encodePokemonPacket(source,size,asset.variant);
    }catch{ /* unavailable original variant; never substitute fan art */ }finally{clearTimeout(timer);}
  }
  return null;
}
