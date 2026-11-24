#pragma once
#include <Arduino.h>

// Minimal, self-contained config portal. No external assets so it works while
// the device hosts its own access point with no Internet. Renders a live
// preview of the current 64x64 frame by decoding /frame.bin (RGB565 LE) in the
// browser.
static const char PORTAL_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Album Clock</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:#111;color:#eee;padding:16px;max-width:520px;margin:0 auto}
h1{font-size:18px;margin:0 0 8px}h3{margin:18px 0 6px;font-size:14px;color:#bbb}
canvas{width:256px;height:256px;image-rendering:pixelated;border:1px solid #444;display:block;margin:8px 0;background:#000}
label{display:block;margin:10px 0 2px;font-size:13px;color:#aaa}
input{width:100%;padding:7px;background:#1c1c1c;border:1px solid #444;color:#eee;border-radius:4px;box-sizing:border-box}
input[type=range]{padding:0}
button{padding:8px 12px;margin:4px 6px 4px 0;background:#2a2a2a;border:1px solid #555;color:#eee;border-radius:4px;cursor:pointer;font-size:13px}
button.primary{background:#2d6cdf;border-color:#2d6cdf}
button:disabled{opacity:.45;cursor:not-allowed}.summary{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin:10px 0}.card{background:#1a1a1a;border:1px solid #333;border-radius:6px;padding:8px;font-size:12px}.card b{display:block;color:#fff;margin-top:3px}
.row{display:flex;gap:6px;flex-wrap:wrap}
pre{background:#1a1a1a;padding:8px;border-radius:4px;font-size:12px;overflow:auto;white-space:pre-wrap}
small{color:#888}
</style></head><body>
<h1>Album Clock</h1>
<canvas id="c" width="64" height="64"></canvas>
<h3>Smart zoom</h3>
<label><input type="checkbox" id="facezoom" onchange="setFaceZoom()"> Enable automatic face zoom</label>
<label><input type="checkbox" id="quadzoom" onchange="setQuadrantZoom()"> Enable quadrant zoom (fallback if no faces)</label>
<label>Zoom transition: <span id="zoomdurv">3000</span> ms</label><input id="zoomdur" type="range" min="500" max="8000" step="500" value="3000" oninput="$('zoomdurv').textContent=this.value" onchange="setZoomDuration()">
<small id="faceStatus">Off</small>
<div class="row"><button id="nextFace" onclick="faceAction('next')">Test / next face</button><button id="nextQuad" onclick="faceAction('nextquad')">Test / next quadrant</button><button id="fullFace" onclick="faceAction('full')">Full cover</button></div>
<small>Tap the cover preview to cycle detected faces, or quadrants when no faces are available.</small>
<div id="summary" class="summary"></div>
<div class="row">
<button id="refreshBtn" onclick="act('/refresh',this)">Refresh now</button>
<button id="nextBtn" onclick="act('/next',this)">Next fallback</button>
<button onclick="act('/restart')">Restart</button>
</div>
<h3>Display mode</h3>
<select id="mode" onchange="setMode()"><option value="0">Album art</option><option value="1">Audio reactive (mic spectrum)</option></select>
<h3>Settings</h3>
<label>Wi-Fi network (SSID)</label><input id="ssid" autocomplete="off">
<label>Wi-Fi password <small>(blank = keep current)</small></label><input id="pass" type="password" autocomplete="off">
<label>Frame URL</label><input id="url" autocomplete="off">
<label>Brightness (1-255): <span id="bval"></span></label><input id="bright" type="range" min="1" max="255">
<div class="row"><button class="primary" onclick="save()">Save &amp; apply</button></div>
<h3>Artwork appearance</h3>
<small>Applies to WiiM, Kan 88 and random artwork. Changes are saved automatically.</small>
<div id="imageControls"></div>
<button onclick="recommendedImage()">Recommended clean preset</button>
<button onclick="resetImage()">Reset to original colors</button>
<h3>Color calibration</h3>
<label>Panel color order</label>
<select id="order" onchange="setOrder()">
<option value="0">0 - RGB</option><option value="1">1 - RBG</option>
<option value="2">2 - GRB</option><option value="3">3 - GBR</option>
<option value="4">4 - BRG</option><option value="5">5 - BGR</option>
</select>
<div class="row"><button onclick="act('/testcard')">Toggle test card</button></div>
<small>Test card quadrants: top-left RED, top-right GREEN, bottom-left BLUE, bottom-right WHITE. Change the order until they match, then turn the test card off.</small>
<h3>Transitions</h3>
<label><input type="checkbox" id="fade" onchange="setFade()"> Fade between covers</label>
<label>Method</label>
<select id="method" onchange="setFade()"><option value="0">Fade through black</option><option value="1">Crossfade / dissolve</option><option value="2">Slide / push</option></select>
<label>Pattern &mdash; disappear</label>
<select id="patout" onchange="setFade()"><option value="0">Luminance &mdash; highlights lead</option><option value="1">Luminance &mdash; highlights linger</option><option value="2">Uniform</option><option value="3">Radial &mdash; center out</option><option value="4">Radial &mdash; edge in</option><option value="5">Wipe &rarr;</option><option value="6">Wipe &larr;</option><option value="7">Wipe &darr;</option><option value="8">Wipe &uarr;</option><option value="9">Diagonal</option><option value="10">Random</option></select>
<label>Pattern &mdash; appear</label>
<select id="patin" onchange="setFade()"><option value="0">Luminance &mdash; highlights lead</option><option value="1">Luminance &mdash; highlights linger</option><option value="2">Uniform</option><option value="3">Radial &mdash; center out</option><option value="4">Radial &mdash; edge in</option><option value="5">Wipe &rarr;</option><option value="6">Wipe &larr;</option><option value="7">Wipe &darr;</option><option value="8">Wipe &uarr;</option><option value="9">Diagonal</option><option value="10">Random</option></select>
<label>Easing &mdash; out (disappear)</label>
<select id="easeout" onchange="setFade()"><option value="0">Linear</option><option value="1">Quad in</option><option value="2">Quad out</option><option value="3">Quad in-out</option><option value="4">Quart in</option><option value="5">Quart out</option><option value="6">Quart in-out</option><option value="7">Expo in</option><option value="8">Expo out</option><option value="9">Expo in-out</option></select>
<label>Easing &mdash; in (appear)</label>
<select id="easein" onchange="setFade()"><option value="0">Linear</option><option value="1">Quad in</option><option value="2">Quad out</option><option value="3">Quad in-out</option><option value="4">Quart in</option><option value="5">Quart out</option><option value="6">Quart in-out</option><option value="7">Expo in</option><option value="8">Expo out</option><option value="9">Expo in-out</option></select>
<label>Slide direction</label>
<select id="slidedir" onchange="setFade()"><option value="0">Left</option><option value="1">Right</option><option value="2">Up</option><option value="3">Down</option></select>
<label>Duration: <span id="fdurl"></span> ms</label><input id="fdur" type="range" min="500" max="6000" step="100" oninput="document.getElementById('fdurl').textContent=this.value" onchange="setFade()">
<label>Out / in balance: <span id="balv"></span> %</label><input id="bal" type="range" min="10" max="90" step="1" oninput="document.getElementById('balv').textContent=this.value" onchange="setFade()">
<label>Black hold: <span id="fholdl"></span> ms</label><input id="fhold" type="range" min="0" max="2000" step="50" oninput="document.getElementById('fholdl').textContent=this.value" onchange="setFade()">
<label>Stagger spread: <span id="sprv"></span></label><input id="spr" type="range" min="0" max="95" step="1" oninput="document.getElementById('sprv').textContent=this.value" onchange="setFade()">
<label>Edge softness: <span id="sftv"></span></label><input id="sft" type="range" min="5" max="95" step="1" oninput="document.getElementById('sftv').textContent=this.value" onchange="setFade()">
<label>Jitter: <span id="jitv"></span></label><input id="jit" type="range" min="0" max="80" step="1" oninput="document.getElementById('jitv').textContent=this.value" onchange="setFade()">
<label>Desaturate on dim: <span id="desv"></span></label><input id="des" type="range" min="0" max="100" step="1" oninput="document.getElementById('desv').textContent=this.value" onchange="setFade()">
<label><input type="checkbox" id="gam" onchange="setFade()"> Perceptual dimming (gamma)</label>
<h3>Status</h3>
<pre id="st">loading&hellip;</pre>
<script>
var $=function(i){return document.getElementById(i)},lastGeneration=-1,statusTimer=0,submitting=false,statusBusy=false,previewBusy=false;
var imageFields=[
 ['contrast','Contrast',0,200,100],['saturation','Saturation',0,200,100],
 ['exposure','Exposure',-100,100,0],['warmth','Warmth',-100,100,0],
 ['tint','Tint (green / magenta)',-100,100,0],['hue','Hue rotation',-180,180,0],
 ['gamma','Midtone brightness (gamma)',50,200,100],['blur','Blur (pixels)',0,4,0],
 ['red','Red gain',50,150,100],['green','Green gain',50,150,100],['blue','Blue gain',50,150,100],
 ['blackpoint','Black point',0,48,8],['whitepoint','White point',207,255,247],
 ['blackcut','Black threshold',0,32,8],['whitecut','White threshold',0,32,8],
 ['shadows','Shadow smoothing',0,100,35],['highlights','Highlight smoothing',0,100,35]
],imageBusy=false;
imageFields.forEach(function(f){
 var label=document.createElement('label'),v=document.createElement('span'),slider=document.createElement('input');
 label.textContent=f[1]+': ';v.id='iv_'+f[0];v.textContent=f[4];label.appendChild(v);
 slider.type='range';slider.id='im_'+f[0];slider.min=f[2];slider.max=f[3];slider.value=f[4];slider.step=1;
 slider.oninput=function(){v.textContent=slider.value;};
 slider.onchange=function(){var data=new URLSearchParams();data.set(f[0],slider.value);sendImage(data);};
 $('imageControls').appendChild(label);$('imageControls').appendChild(slider);
});
function sendImage(data){imageBusy=true;timedFetch('/image',{method:'POST',body:data},3000)
 .then(function(r){if(!r.ok)throw Error('Image adjustment '+r.status);return r.text();})
 .then(function(t){$('st').textContent=t;drawFrame();})
 .catch(function(e){$('st').textContent=e.message;})
 .finally(function(){imageBusy=false;loadStatus();});}
function resetImage(){var data=new URLSearchParams();data.set('reset','1');sendImage(data);}
function recommendedImage(){var data=new URLSearchParams();data.set('recommended','1');sendImage(data);}
function faceAction(action){var data=new URLSearchParams();data.set(action,'1');sendFace(data);}
function setFaceZoom(){var data=new URLSearchParams();data.set('value',$('facezoom').checked?'1':'0');sendFace(data);}
function setQuadrantZoom(){var data=new URLSearchParams();data.set('quadrant',$('quadzoom').checked?'1':'0');sendFace(data);}
function setZoomDuration(){var data=new URLSearchParams();data.set('duration',$('zoomdur').value);sendFace(data);}
function sendFace(data){timedFetch('/facezoom',{method:'POST',body:data},3000).then(function(r){return r.text().then(function(t){if(!r.ok)throw Error(t);return t})}).then(function(t){$('st').textContent=t;loadStatus();}).catch(function(e){$('faceStatus').textContent=e.message;});}
$('c').onclick=function(){if(!$('nextFace').disabled)faceAction('next');else if(!$('nextQuad').disabled)faceAction('nextquad');};
function timedFetch(url,options,timeout){var c=new AbortController();setTimeout(function(){c.abort()},Math.max(timeout||2500,5000));options=options||{};options.signal=c.signal;return fetch(url,options);}
function drawFrame(){if(previewBusy||document.hidden)return;previewBusy=true;timedFetch('/frame.bin?t='+Date.now(),{},5000).then(function(r){if(!r.ok)throw Error('preview '+r.status);return r.arrayBuffer()}).then(function(b){
 var d=new DataView(b),ctx=$('c').getContext('2d'),img=ctx.createImageData(64,64),i,v,r,g,bl;
 for(i=0;i<4096;i++){v=d.getUint16(i*2,true);
  r=(v>>11&31)*255/31|0;g=(v>>5&63)*255/63|0;bl=(v&31)*255/31|0;
  img.data[i*4]=r;img.data[i*4+1]=g;img.data[i*4+2]=bl;img.data[i*4+3]=255;}
 ctx.putImageData(img,0,0);}).catch(function(){}).finally(function(){previewBusy=false;});}
function esc(v){return String(v==null?'':v).replace(/[&<>]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;'}[c]})}
function loadStatus(){clearTimeout(statusTimer);if(document.hidden||statusBusy){statusTimer=setTimeout(loadStatus,5000);return;}statusBusy=true;timedFetch('/status',{},5000).then(function(r){if(!r.ok)throw Error('status '+r.status);return r.json()}).then(function(s){
 $('st').textContent=JSON.stringify(s,null,2);
 var busy=!!(s.network_busy||s.transition_active||s.next_queued||s.zoom_animating),playing=s.playback==='playing';
 $('facezoom').checked=!!s.face_enabled;$('nextFace').disabled=!s.face_enabled||!s.face_count||s.transition_active;
 $('quadzoom').checked=!!s.quadrant_enabled;$('nextQuad').disabled=!s.quadrant_enabled||!s.has_frame||s.transition_active;
 $('fullFace').disabled=(s.face_index==null||s.face_index<0)&&(s.quadrant_index==null||s.quadrant_index<0)&&!s.zoom_animating;
 if(document.activeElement.id!=='zoomdur'&&s.zoom_duration!=null){$('zoomdur').value=s.zoom_duration;$('zoomdurv').textContent=s.zoom_duration;}
 $('faceStatus').textContent=s.face_recovery?'Face detection disabled after interruption — enable to retry':!s.face_enabled&&!s.quadrant_enabled?'Off':s.face_pending?'Detecting faces…':(s.face_enabled?(s.face_count||0)+' face(s) detected':'Quadrant mode')+(s.face_index>=0?' — face '+(s.face_index+1)+' / '+s.face_count:s.quadrant_index>=0?' — quadrant '+(s.quadrant_index+1)+' / 4':' — full cover')+(s.zoom_animating?' — transitioning':'')+' — '+(s.face_source_px||64)+'px source';
 $('summary').innerHTML='<div class="card">Playback<b>'+esc(s.playback||'unknown')+'</b></div><div class="card">Service<b>'+esc(s.service_status||'unknown')+'</b></div><div class="card">Fallback pack<b>'+esc(s.pack_count||0)+' covers</b></div><div class="card">Network<b>'+esc(s.last_network_latency_ms||0)+' ms</b></div>';
 $('nextBtn').disabled=playing||submitting||!s.pack_count;$('nextBtn').title=playing?'Next is available while playback is idle':(!s.pack_count?'Fallback pack is still syncing':'');
 $('refreshBtn').disabled=submitting;
 if(s.display_generation!==lastGeneration||s.zoom_animating){lastGeneration=s.display_generation;drawFrame();}
 if(document.activeElement.id!=='ssid'&&!$('ssid').value)$('ssid').value=s.ssid||'';
 if(document.activeElement.id!=='url'&&!$('url').value)$('url').value=s.frame_url||'';
 if(document.activeElement.id!=='bright'){$('bright').value=s.brightness;$('bval').textContent=s.brightness;}
 if(document.activeElement.id!=='order'&&s.color_order!=null)$('order').value=s.color_order;
 var af=document.activeElement.id;
 if(!imageBusy)imageFields.forEach(function(f){var id='im_'+f[0];if(af!==id&&s[id]!=null){$(id).value=s[id];$('iv_'+f[0]).textContent=s[id];}});
 if(af!=='mode'&&s.display_mode!=null)$('mode').value=s.display_mode;
 if(s.fade_enabled!=null)$('fade').checked=s.fade_enabled;
 if(af!=='method'&&s.fade_method!=null)$('method').value=s.fade_method;
 if(af!=='patout'&&s.pat_out!=null)$('patout').value=s.pat_out;
 if(af!=='patin'&&s.pat_in!=null)$('patin').value=s.pat_in;
 if(af!=='easeout'&&s.ease_out!=null)$('easeout').value=s.ease_out;
 if(af!=='easein'&&s.ease_in!=null)$('easein').value=s.ease_in;
 if(af!=='slidedir'&&s.slide_dir!=null)$('slidedir').value=s.slide_dir;
 if(af!=='gam'&&s.gamma!=null)$('gam').checked=s.gamma;
 function sr(id,lbl,val){if(af!==id&&val!=null){$(id).value=val;if(lbl)$(lbl).textContent=val;}}
 sr('fdur','fdurl',s.fade_ms);sr('fhold','fholdl',s.fade_hold_ms);sr('bal','balv',s.balance);
 sr('spr','sprv',s.spread);sr('sft','sftv',s.soft);sr('jit','jitv',s.jitter);sr('des','desv',s.desat);
 statusTimer=setTimeout(loadStatus,busy?2000:5000);
}).catch(function(e){$('st').textContent='Status unavailable: '+e.message;statusTimer=setTimeout(loadStatus,5000);}).finally(function(){statusBusy=false;});}
function setOrder(){var f=new URLSearchParams();f.set('value',$('order').value);fetch('/colororder',{method:'POST',body:f}).then(function(r){return r.text()}).then(function(t){$('st').textContent=t;setTimeout(drawFrame,600);});}
function setMode(){var f=new URLSearchParams();f.set('value',$('mode').value);fetch('/mode',{method:'POST',body:f}).then(function(r){return r.text()}).then(function(t){$('st').textContent=t;});}
function setFade(){var f=new URLSearchParams();
 f.set('value',$('fade').checked?1:0);f.set('ms',$('fdur').value);f.set('hold',$('fhold').value);
 f.set('method',$('method').value);f.set('patout',$('patout').value);f.set('patin',$('patin').value);
 f.set('easeout',$('easeout').value);f.set('easein',$('easein').value);f.set('slidedir',$('slidedir').value);
 f.set('balance',$('bal').value);f.set('spread',$('spr').value);f.set('soft',$('sft').value);
 f.set('jitter',$('jit').value);f.set('desat',$('des').value);f.set('gamma',$('gam').checked?1:0);
 fetch('/fade',{method:'POST',body:f}).then(function(r){return r.text()}).then(function(t){$('st').textContent=t;});}
$('bright').oninput=function(){$('bval').textContent=$('bright').value};
$('bright').onchange=function(){var f=new URLSearchParams();f.set('value',$('bright').value);fetch('/brightness',{method:'POST',body:f});};
function save(){var f=new URLSearchParams();f.set('ssid',$('ssid').value);f.set('pass',$('pass').value);f.set('url',$('url').value);f.set('brightness',$('bright').value);
 timedFetch('/save',{method:'POST',body:f},3000).then(function(r){return r.text()}).then(function(t){$('st').textContent=t;loadStatus();});}
function act(p,button){if(submitting)return;submitting=true;if(button)button.disabled=true;timedFetch(p,{method:'POST'},1500).then(function(r){return r.text().then(function(t){if(!r.ok)throw Error(t);return t})}).then(function(t){$('st').textContent=t;}).catch(function(e){$('st').textContent='Action failed: '+e.message;}).finally(function(){submitting=false;loadStatus();});}
document.addEventListener('visibilitychange',function(){if(!document.hidden){loadStatus();drawFrame();}});
loadStatus();drawFrame();
</script></body></html>)HTML";
