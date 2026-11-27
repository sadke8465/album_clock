#pragma once
#include <Arduino.h>

// Minimal, self-contained config portal. No external assets so it works while
// the device hosts its own access point with no Internet. Renders a live
// preview of the current 64x64 frame by decoding /frame.bin (RGB565 LE) in the
// browser.
static const char PORTAL_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#151413">
<title>Album Clock</title>
<style>
:root{color-scheme:dark;--bg:#151413;--surface:#201e1c;--line:#36322e;--text:#f4f0e9;--muted:#a9a39a;--accent:#e1af76;--sub:#282522}
*{box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0 auto;background:var(--bg);color:var(--text);padding:18px 16px calc(28px + env(safe-area-inset-bottom));max-width:680px;font-size:15px}
h1{font-size:26px;font-weight:630;letter-spacing:-1.1px;margin:3px 0 0}h3{margin:0 0 5px;font-size:18px;font-weight:600;letter-spacing:-.3px}h4{font-size:13px;font-weight:600;color:var(--accent);text-transform:uppercase;letter-spacing:1px;margin:22px 0 8px}
.eyebrow{font:600 10px/1.4 system-ui;letter-spacing:2px;color:var(--accent);text-transform:uppercase}.header{display:flex;justify-content:space-between;align-items:center;margin-bottom:18px}.badge{font-size:11px;border:1px solid var(--line);border-radius:20px;padding:7px 10px;color:var(--muted);white-space:nowrap}
.hero{display:grid;grid-template-columns:minmax(110px,145px) 1fr;gap:16px;align-items:center;background:var(--surface);border:1px solid var(--line);padding:13px;border-radius:20px;margin-bottom:14px}
canvas{width:100%;aspect-ratio:1;height:auto;image-rendering:pixelated;display:block;background:#000;border-radius:9px;cursor:pointer}
.hero-copy{min-width:0}.now{font-size:19px;font-weight:620;letter-spacing:-.5px;line-height:1.15;overflow-wrap:anywhere;margin:5px 0 7px}.meta{font-size:12px;color:var(--muted);line-height:1.5}.summary{display:grid;grid-template-columns:1fr 1fr;gap:5px;margin:9px 0 0}.card{font-size:10px;color:var(--muted);min-width:0}.card b{display:block;color:var(--text);font-size:12px;font-weight:550;margin-top:2px;overflow-wrap:anywhere}
.tabs{position:sticky;top:0;z-index:5;display:flex;gap:5px;overflow-x:auto;scrollbar-width:none;background:var(--bg);padding:8px 0 10px;margin:0 0 8px}.tabs::-webkit-scrollbar{display:none}.tabs button{white-space:nowrap;flex:1;min-width:max-content;padding:11px 13px;font-size:12px;border-radius:12px;background:var(--surface);border-color:var(--line);color:var(--muted)}.tabs button[aria-selected=true]{background:var(--accent);border-color:var(--accent);color:#201910;font-weight:650}
.panel{background:var(--surface);border:1px solid var(--line);border-radius:18px;padding:17px;margin-bottom:12px}.panel[hidden]{display:none}.caption{font-size:12px;color:var(--muted);line-height:1.5;margin:4px 0 18px}
label{display:block;margin:17px 0 7px;font-size:13px;color:var(--text);font-weight:450;line-height:1.4}label span{color:var(--accent);font-variant-numeric:tabular-nums;float:right}label:has(input[type=checkbox]){display:flex;align-items:center;gap:11px;padding:11px 0;border-bottom:1px solid var(--line);margin:4px 0}
input,select{width:100%;padding:12px;background:var(--sub);border:1px solid var(--line);color:var(--text);border-radius:10px;font:inherit;min-height:44px;outline-color:var(--accent)}
input[type=range]{appearance:none;width:100%;padding:0;border:0;min-height:30px;background:transparent;accent-color:var(--accent)}input[type=range]::-webkit-slider-runnable-track{height:5px;background:#595149;border-radius:8px}input[type=range]::-webkit-slider-thumb{appearance:none;width:23px;height:23px;border-radius:50%;background:var(--accent);margin-top:-9px;border:2px solid var(--surface)}input[type=range]::-moz-range-thumb{border:0;background:var(--accent);width:20px;height:20px;border-radius:50%}
input[type=checkbox]{appearance:none;width:42px;min-width:42px;height:25px;min-height:25px;border-radius:18px;background:#4b4540;border:0;padding:0;position:relative;margin:0}input[type=checkbox]::after{content:"";position:absolute;left:3px;top:3px;width:19px;height:19px;background:#eee;border-radius:50%;transition:transform .15s}input[type=checkbox]:checked{background:var(--accent)}input[type=checkbox]:checked::after{transform:translateX(17px);background:#201910}
button{font:550 13px system-ui;color:var(--text);padding:12px 14px;background:var(--sub);border:1px solid var(--line);border-radius:10px;cursor:pointer;min-height:42px}button.primary{background:var(--accent);border-color:var(--accent);color:#201910}button:disabled{opacity:.34;cursor:not-allowed}.row{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0}.row button{flex:1;min-width:125px}.presets{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;margin:12px 0}.presets button{min-height:50px;text-align:left}
pre{background:var(--sub);padding:12px;border-radius:9px;font-size:11px;overflow:auto;white-space:pre-wrap;overflow-wrap:anywhere}small{color:var(--muted);line-height:1.5;font-size:12px}details{margin-top:14px}summary{cursor:pointer;color:var(--muted);font-size:13px;padding:10px 0}.note{display:block;padding:10px 12px;background:var(--sub);border-radius:9px;margin:12px 0;color:var(--muted);font-size:12px}
@media(max-width:380px){body{padding:12px}.hero{grid-template-columns:108px 1fr;gap:10px}.now{font-size:16px}.panel{padding:14px}.tabs button{padding:10px 11px}.row button{min-width:105px}}
</style></head><body>
<header class="header"><div><div class="eyebrow">Listening room / 64 × 64</div><h1>Album Clock</h1></div><span id="connectionBadge" class="badge">Connecting</span></header>
<div class="hero"><canvas id="c" width="64" height="64" aria-label="Current artwork preview"></canvas><div class="hero-copy"><div class="eyebrow">Now showing</div><div id="nowTitle" class="now">Album artwork</div><div id="nowMeta" class="meta">Connecting to display…</div><div id="summary" class="summary"></div></div></div>
<nav class="tabs" aria-label="Settings"><button data-tab="display" aria-selected="true" onclick="openPanel('display')">Display</button><button data-tab="ambient" aria-selected="false" onclick="openPanel('ambient')">Ambient</button><button data-tab="zoom" aria-selected="false" onclick="openPanel('zoom')">Zoom</button><button data-tab="image" aria-selected="false" onclick="openPanel('image')">Image</button><button data-tab="transitions" aria-selected="false" onclick="openPanel('transitions')">Transitions</button><button data-tab="device" aria-selected="false" onclick="openPanel('device')">Device</button></nav>
<section class="panel" data-panel="zoom" hidden>
<h3>Smart zoom</h3>
<p class="caption">Explore the artwork with faces, typography and visual details.</p>
<label><input type="checkbox" id="facezoom" onchange="setFaceZoom()"> Enable automatic face zoom (Apple Music only)</label>
<label><input type="checkbox" id="textzoom" onchange="zoomSetting('text',this.checked?1:0)"> Enable text-region zoom</label>
<label><input type="checkbox" id="quadzoom" onchange="setQuadrantZoom()"> Enable quadrant zoom (fallback if no faces or text)</label>
<label><input type="checkbox" id="saliency" onchange="zoomSetting('saliency',this.checked?1:0)"> Prioritize visually detailed quadrants</label>
<label>Face sensitivity: <span id="facesensv">60</span>%</label><input id="facesens" type="range" min="0" max="100" value="60" oninput="$('facesensv').textContent=this.value" onchange="zoomSetting('sensitivity',this.value)">
<label>Zoom depth: <span id="zoomdepthv">100</span>%</label><input id="zoomdepth" type="range" min="50" max="150" value="100" oninput="$('zoomdepthv').textContent=this.value" onchange="zoomSetting('depth',this.value)">
<label>Automatic delay: <span id="zoomdelayv">30</span> seconds</label><input id="zoomdelay" type="range" min="1" max="120" value="30" oninput="$('zoomdelayv').textContent=this.value" onchange="zoomSetting('delay',this.value)">
<label>Hold duration: <span id="zoomholdv">18</span> seconds</label><input id="zoomhold" type="range" min="3" max="60" value="18" oninput="$('zoomholdv').textContent=this.value" onchange="zoomSetting('hold',this.value)">
<label>Zoom transition: <span id="zoomdurv">3000</span> ms</label><input id="zoomdur" type="range" min="500" max="8000" step="500" value="3000" oninput="$('zoomdurv').textContent=this.value" onchange="setZoomDuration()">
<label>Zoom smoothness: <span id="zoomsmoothv">100</span>%</label><input id="zoomsmooth" type="range" min="0" max="100" value="100" oninput="$('zoomsmoothv').textContent=this.value" onchange="zoomSetting('smoothness',this.value)">
<label>Zoom frame rate target: <span id="zoomfpsv">30</span> fps</label><input id="zoomfps" type="range" min="12" max="30" value="30" oninput="$('zoomfpsv').textContent=this.value" onchange="zoomSetting('fps',this.value)">
<small id="faceStatus">Off</small>
<div class="row"><button id="nextFace" onclick="faceAction('next')">Test / next face</button><button id="nextText" onclick="faceAction('nexttext')">Test / next text</button><button id="nextQuad" onclick="faceAction('nextquad')">Test / next quadrant</button><button id="rescan" onclick="faceAction('rescan')">Rescan cover</button><button id="fullFace" onclick="faceAction('full')">Full cover</button></div>
<small>Tap the preview to advance faces, text, or detail regions. Text detection locates likely horizontal text bands; it does not read words.</small>
</section>
<section class="panel" data-panel="display">
<h3>Display</h3><p class="caption">Quick controls for your listening room.</p>
<label>Brightness <span id="bval">30</span></label><input id="bright" type="range" min="1" max="255">
<label>Display mode</label>
<select id="mode" onchange="setMode()"><option value="0">Album art</option><option value="1">Audio reactive (mic spectrum)</option></select>
<div class="row">
<button id="refreshBtn" onclick="act('/refresh',this)">Refresh now</button>
<button id="nextBtn" onclick="act('/next',this)">Next fallback</button>
</div>
<div class="note">Tap the cover to advance smart zoom. Artwork preview refreshes periodically.</div>
<button class="primary" onclick="openPanel('ambient')">Explore ambient effects</button>
</section>
<section class="panel" data-panel="ambient" hidden>
<h3>Ambient effects</h3><p class="caption">Responsive light over the album artwork. Pixel shimmer animates the artwork’s own pixels in place. No white sparkle overlay; use Pixel color variation to adjust the native colors. Pure black remains dark in highlights mode. Effects pause during cover and zoom transitions.</p>
<div class="presets"><button class="primary" onclick="ambientPreset('pixels')">✴ &nbsp; Pixel shimmer</button><button onclick="ambientPreset('sparkle')">✦ &nbsp; Luminous sparkle</button><button onclick="ambientPreset('bold')">✧ &nbsp; Bold sparkle</button><button onclick="ambientPreset('shimmer')">≈ &nbsp; Light sweep</button><button onclick="ambientPreset('breathe')">○ &nbsp; Breathing light</button><button onclick="ambientPreset('drift')">⋯ &nbsp; Drifting embers</button><button onclick="ambientPreset('ripple')">◎ &nbsp; Luminous ripple</button><button onclick="ambientPreset('subtle')">· &nbsp; Subtle sparkle</button><button onclick="ambientSetting('mode',0)">Off</button></div>
<div id="ambientControls"></div>
<button onclick="ambientPreset('reset')">Restore recommended settings</button>
</section>
<section class="panel" data-panel="device" hidden>
<h3>Device &amp; network</h3><p class="caption">Wi-Fi and source settings. Blank password keeps the saved password.</p>
<label>Wi-Fi network (SSID)</label><input id="ssid" autocomplete="off">
<label>Wi-Fi password <small>(blank = keep current)</small></label><input id="pass" type="password" autocomplete="off">
<label>Frame URL</label><input id="url" autocomplete="off">
<div class="row"><button class="primary" onclick="save()">Save &amp; apply</button><button onclick="act('/restart')">Restart</button></div>
</section>
<section class="panel" data-panel="device" hidden>
<h3>Firmware updates</h3><p class="caption">Download verified updates directly to the display over Wi-Fi. Keep the display powered during installation.</p>
<div id="otaCurrent" class="meta">Current version —</div><div id="otaLatest" class="now">Check for updates</div><div id="otaNotes" class="meta"></div>
<progress id="otaBar" value="0" max="100" style="width:100%;accent-color:var(--accent);display:none"></progress>
<div id="otaMessage" class="note">Updates are installed only when you confirm.</div>
<div class="row"><button id="otaCheck" onclick="otaAction('check')">Check for updates</button><button id="otaInstall" class="primary" disabled onclick="otaAction('install')">Update now</button></div>
</section>
<section class="panel" data-panel="image" hidden>
<h3>Artwork appearance</h3>
<small>Applies to WiiM, Kan 88 and random artwork. Changes are saved automatically.</small>
<div id="imageControls"></div>
<button onclick="recommendedImage()">Recommended clean preset</button>
<button onclick="resetImage()">Reset to original colors</button>
<h4>Color calibration</h4>
<label>Panel color order</label>
<select id="order" onchange="setOrder()">
<option value="0">0 - RGB</option><option value="1">1 - RBG</option>
<option value="2">2 - GRB</option><option value="3">3 - GBR</option>
<option value="4">4 - BRG</option><option value="5">5 - BGR</option>
</select>
<div class="row"><button onclick="act('/testcard')">Toggle test card</button></div>
<small>Test card quadrants: top-left RED, top-right GREEN, bottom-left BLUE, bottom-right WHITE. Change the order until they match, then turn the test card off.</small>
</section>
<section class="panel" data-panel="transitions" hidden>
<h3>Transitions</h3>
<p class="caption">Fine-tune cover changes, pacing and the individual pixel fade patterns.</p>
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
</section>
<section class="panel" data-panel="device" hidden><h3>Diagnostics</h3><div id="deviceInfo" class="meta"></div><details><summary>Raw status and logs</summary><pre id="st">loading&hellip;</pre></details></section>
<small id="notice" role="status" aria-live="polite"></small>
<script>
var $=function(i){return document.getElementById(i)},lastGeneration=-1,statusTimer=0,submitting=false,statusBusy=false,previewBusy=false;
var otaToken='',otaState=0;
function otaAction(action){
 if(!otaToken){notice('Wait for the device to connect');return;}
 if(action==='install'&&!confirm('Install the new firmware? The display will restart. Keep it powered until the update finishes.'))return;
 var data=new URLSearchParams();data.set('action',action);
 $('otaCheck').disabled=true;$('otaInstall').disabled=true;
 timedFetch('/ota',{method:'POST',body:data,headers:{'X-OTA-Token':otaToken}},5000).then(function(r){return r.text().then(function(t){if(!r.ok)throw Error(t);return t;});})
 .then(function(t){$('otaMessage').textContent=t;loadStatus();}).catch(function(e){$('otaMessage').textContent=e.message;loadStatus();});
}
function openPanel(id){
 document.querySelectorAll('[data-panel]').forEach(function(p){p.hidden=p.dataset.panel!==id;});
 document.querySelectorAll('[data-tab]').forEach(function(b){b.setAttribute('aria-selected',String(b.dataset.tab===id));});
 window.scrollTo(0,0);
}
var ambientFields=[
 ['mode','Effect',0,6,1,['Off','Sparkle','Silk shimmer','Breathing','Drifting embers','Soft ripple','Pixel shimmer']],
 ['target','Light follows',0,4,0,['Luminance / highlights','Shadows','Edges / contrast','Entire image','Saturated colors']],
 ['intensity','Intensity',0,100,68],['density','Density / wave width',1,48,34],['speed','Speed',10,200,80],
 ['size','Particle size',1,3,2],['threshold','Target threshold',0,245,40],['softness','Softness',0,100,35],
 ['palette','Light color',0,4,1,['Artwork color','White','Warm amber','Cool blue','Multicolor']],
 ['direction','Direction',0,3,0,['Right','Left','Down','Up']],
 ['blend','Blend',0,3,0,['Mix','Screen','Additive','Light & shade (waves)']],['chroma','Pixel color variation',0,100,25],['fps','Animation frame rate',5,25,15]
],ambientBusy=false;
ambientFields.forEach(function(f){
 var label=document.createElement('label'),v=document.createElement('span'),control=document.createElement(f[5]?'select':'input'),id='am_'+f[0];
 label.htmlFor=id;label.textContent=f[1];control.id=id;
 if(f[5])f[5].forEach(function(text,index){var option=document.createElement('option');option.value=index;option.textContent=text;control.appendChild(option);});
 else{control.type='range';control.min=f[2];control.max=f[3];control.step=1;v.id='av_'+f[0];v.textContent=f[4];label.appendChild(v);control.oninput=function(){v.textContent=control.value;};}
 control.value=f[4];control.onchange=function(){ambientSetting(f[0],control.value);};
 $('ambientControls').appendChild(label);$('ambientControls').appendChild(control);
});
function notice(t){$('notice').textContent=t;clearTimeout(notice.timer);notice.timer=setTimeout(function(){$('notice').textContent='';},4500);}
function sendAmbient(data){ambientBusy=true;timedFetch('/ambient',{method:'POST',body:data},5000)
 .then(function(r){if(!r.ok)throw Error('Ambient '+r.status);return r.text();})
 .then(function(t){notice(t);drawFrame();})
 .catch(function(e){notice(e.message);})
 .finally(function(){ambientBusy=false;loadStatus();});}
function ambientSetting(name,value){var data=new URLSearchParams();data.set(name,value);sendAmbient(data);}
function ambientPreset(name){
 var presets={
 pixels:{mode:6,target:0,intensity:72,density:28,speed:100,size:1,threshold:22,softness:65,chroma:30,fps:15},
 sparkle:{mode:1,target:0,intensity:68,density:34,speed:80,size:2,threshold:40,softness:35,palette:1,direction:0,blend:0,fps:15},
 bold:{mode:1,target:0,intensity:95,density:48,speed:105,size:3,threshold:25,softness:30,palette:1,direction:0,blend:2,fps:15},
 subtle:{mode:1,target:0,intensity:25,density:18,speed:65,size:1,threshold:88,softness:70,palette:0,direction:0,blend:0,fps:15},
 shimmer:{mode:2,target:0,intensity:55,density:24,speed:55,size:1,threshold:40,softness:40,palette:1,direction:0,blend:3,fps:15},
 breathe:{mode:3,target:0,intensity:50,density:18,speed:40,size:1,threshold:30,softness:45,palette:0,direction:0,blend:3,fps:12},
 drift:{mode:4,target:0,intensity:75,density:30,speed:85,size:2,threshold:38,softness:40,palette:2,direction:3,blend:2,fps:15},
 ripple:{mode:5,target:0,intensity:55,density:18,speed:65,size:1,threshold:35,softness:40,palette:0,direction:0,blend:3,fps:15},
 reset:{reset:1}
 };
 var data=new URLSearchParams();Object.keys(presets[name]).forEach(function(k){data.set(k,presets[name][k]);});sendAmbient(data);
}
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
function zoomSetting(name,value){var data=new URLSearchParams();data.set(name,value);sendFace(data);}
function sendFace(data){timedFetch('/facezoom',{method:'POST',body:data},3000).then(function(r){return r.text().then(function(t){if(!r.ok)throw Error(t);return t})}).then(function(t){$('st').textContent=t;loadStatus();}).catch(function(e){$('faceStatus').textContent=e.message;});}
$('c').onclick=function(){if(!$('nextFace').disabled)faceAction('next');else if(!$('nextText').disabled)faceAction('nexttext');else if(!$('nextQuad').disabled)faceAction('nextquad');};
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
 $('connectionBadge').textContent=s.wifi_connected?'● Online':s.ap_mode?'Setup mode':'Offline';
 $('connectionBadge').style.color=s.wifi_connected?'#a7d6a2':'';
 $('nowTitle').textContent=s.display_mode===1?'Audio reactive':s.pending_title||(playing?'Live artwork':'Random artwork');
 $('nowMeta').textContent=(s.service_status||'Connecting')+' · '+(s.face_source_px||64)+'px source';
 $('deviceInfo').textContent='Firmware '+(s.fw||'—')+' · '+(s.ip||'—')+' · '+(s.rssi||0)+' dBm';
 otaToken=s.ota_token||'';otaState=s.ota_state||0;
 $('otaCurrent').textContent='Current: '+(s.fw||'—');
 $('otaLatest').textContent=s.ota_version?'Latest: '+s.ota_version:'Check for updates';
 $('otaNotes').textContent=s.ota_notes||'';
 $('otaMessage').textContent=s.ota_message||'Updates are installed only when you confirm.';
 $('otaBar').style.display=otaState===4||otaState===6?'block':'none';$('otaBar').value=s.ota_progress||0;
 $('otaCheck').disabled=!s.wifi_connected||!otaToken||[1,4,6].includes(otaState);
 $('otaInstall').disabled=otaState!==2||!s.wifi_connected;
 if(!ambientBusy)ambientFields.forEach(function(f){var id='am_'+f[0];if(document.activeElement.id!==id&&s[id]!=null){$(id).value=s[id];if(!f[5])$('av_'+f[0]).textContent=s[id];}});
 $('facezoom').checked=!!s.face_enabled;$('nextFace').disabled=!s.face_enabled||!s.face_apple_music||!s.face_count||s.transition_active;
 $('quadzoom').checked=!!s.quadrant_enabled;$('nextQuad').disabled=!s.quadrant_enabled||!s.has_frame||s.transition_active;
 $('textzoom').checked=!!s.text_enabled;$('saliency').checked=!!s.saliency_enabled;$('nextText').disabled=!s.text_enabled||!s.text_count||s.transition_active;
 $('rescan').disabled=!!s.face_pending||!s.has_frame;
 $('fullFace').disabled=(s.face_index==null||s.face_index<0)&&(s.quadrant_index==null||s.quadrant_index<0)&&(s.text_index==null||s.text_index<0)&&!s.zoom_animating;
 [['facesens','face_sensitivity'],['zoomdepth','zoom_depth'],['zoomdelay','zoom_delay'],['zoomhold','zoom_hold'],['zoomfps','zoom_fps'],['zoomsmooth','zoom_smoothness']].forEach(function(p){if(document.activeElement.id!==p[0]&&s[p[1]]!=null){$(p[0]).value=s[p[1]];$(p[0]+'v').textContent=s[p[1]];}});
 if(document.activeElement.id!=='zoomdur'&&s.zoom_duration!=null){$('zoomdur').value=s.zoom_duration;$('zoomdurv').textContent=s.zoom_duration;}
 $('faceStatus').textContent=s.face_recovery?'Face detection disabled after interruption — enable to retry':!s.face_enabled&&!s.quadrant_enabled&&!s.text_enabled?'Off':s.face_pending?'Scanning face tiles…':(s.face_enabled&&s.face_apple_music?(s.face_count||0)+' face(s), ':'')+(s.text_enabled?(s.text_count||0)+' text region(s)':'Detail mode')+(s.face_index>=0?' — face '+(s.face_index+1)+' / '+s.face_count:s.text_index>=0?' — text '+(s.text_index+1)+' / '+s.text_count:s.quadrant_index>=0?' — quadrant '+(s.quadrant_index+1)+' / 4':' — full cover')+(s.zoom_animating?' — transitioning':'')+' — '+(s.face_source_px||64)+'px source';
 $('summary').innerHTML='<div class="card">Playback<b>'+esc(s.playback||'unknown')+'</b></div><div class="card">Ambient<b>'+esc(ambientFields[0][5][s.am_mode]||'Off')+'</b></div><div class="card">Library<b>'+esc(s.pack_count||0)+' covers</b></div><div class="card">Network<b>'+esc(s.last_network_latency_ms||0)+' ms</b></div>';
 $('nextBtn').disabled=playing||submitting||!s.pack_count;$('nextBtn').title=playing?'Next is available while playback is idle':(!s.pack_count?'Fallback pack is still syncing':'');
 $('refreshBtn').disabled=submitting;
 if(s.display_generation!==lastGeneration||s.zoom_animating||s.transition_active||s.ambient_rendered){lastGeneration=s.display_generation;drawFrame();}
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
 statusTimer=setTimeout(loadStatus,busy||otaState===1||otaState===4?2000:5000);
}).catch(function(e){$('st').textContent='Status unavailable: '+e.message;$('connectionBadge').textContent='No response';statusTimer=setTimeout(loadStatus,5000);}).finally(function(){statusBusy=false;});}
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
