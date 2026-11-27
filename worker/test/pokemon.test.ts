import test from 'node:test';
import assert from 'node:assert/strict';
import UPNG from 'upng-js';
import {decodePokemonAsset,encodePokemonPacket,pokemonAssets} from '../src/pokemon.ts';

test('original game assets support APNG, GIF and static eras, never fan-made species',()=>{
  assert.match(pokemonAssets(5,25,0,true)[0].url,/crystal\/animated\/25\.gif$/);
  assert.match(pokemonAssets(7,25,1,true)[0].url,/emerald\/animated\/shiny\/25\.png$/);
  assert.match(pokemonAssets(9,25,0,true)[0].url,/diamond-pearl\/animated\/25\.png$/);
  assert.match(pokemonAssets(12,25,3,true)[0].url,/black-white\/animated\/back\/shiny\/25\.gif$/);
  assert.match(pokemonAssets(1,25,0,false)[0].url,/red-blue\/transparent\/25\.png$/);
  assert.equal(pokemonAssets(1,25,0,true).length,0);
  assert.equal(pokemonAssets(12,650,0,true).length,0);
  assert.equal(pokemonAssets(1,152,0,false).length,0);
  assert.ok(pokemonAssets(16,808,0,true).length);
  assert.equal(pokemonAssets(16,200,0,true).length,0);
  assert.equal(pokemonAssets(1,25,1,false).length,0);
});

test('APNG delays, union framing and true black survive sprite packet encoding',()=>{
  const w=8,h=8,one=new Uint8Array(w*h*4),two=new Uint8Array(w*h*4);
  one.set([0,0,0,255],(1*w+1)*4);two.set([255,0,0,255],(2*w+2)*4);
  const apng=UPNG.encode([one.buffer,two.buffer],w,h,0,[40,120]);
  const decoded=decodePokemonAsset(apng);
  assert.equal(decoded.frames.length,2);
  assert.deepEqual(decoded.frames.map(f=>f.delay),[40,120]);
  const p=encodePokemonPacket(decoded,16,1),v=new DataView(p.buffer);
  assert.equal(new TextDecoder().decode(p.slice(0,4)),'PKA1');
  assert.equal(p[4],2);assert.equal(p[5],2);assert.equal(v.getUint16(6,true),2);
  assert.notEqual(v.getUint16(8,true),0);assert.equal(p[10],1);assert.equal(p[11],1);
  assert.equal(v.getUint16(12,true),40);
  const next=17+v.getUint16(14,true);assert.equal(v.getUint16(next,true),120);
});

test('packet resolution is capped and invalid assets rejected',()=>{
  const rgba=new Uint8Array(80*80*4);rgba.fill(255);
  const p=encodePokemonPacket({width:80,height:80,frames:[{rgba,delay:100}]},16,0);
  assert.equal(p[4],16);assert.equal(p[5],16);
  assert.ok(p.length<200);assert.equal(p[11],0);
  assert.throws(()=>encodePokemonPacket({width:1,height:1,frames:[{rgba:new Uint8Array(4),delay:10}]},64,0),/empty/);
  assert.throws(()=>decodePokemonAsset(new ArrayBuffer(8)),/unsupported/);
});
