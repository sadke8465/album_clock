// Optional local browser smoke test (Playwright + Chromium). The remote is
// exercised against fake device responses; no hardware is changed.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium } = require('playwright');

(async () => {
  const raw = fs.readFileSync(path.join(__dirname, '../firmware/include/PortalPage.h'), 'utf8');
  const html = raw.split('R"HTML(')[1].split(')HTML"')[0];
  const browser = await chromium.launch({executablePath: process.env.POKEMON_CHROMIUM_EXECUTABLE || undefined, headless: true, args: ['--no-sandbox']});
  try {
    const page = await browser.newPage({viewport: {width:390,height:844},deviceScaleFactor:1});
    let mode = 2, ambient={am_mode:0}; const errors = [], calls = [];
    const cells = [
      {id:25,name:'Pikachu',game:12,frames:58,variant:0,pinned:false,pending:false,favorite:true},
      {id:6,name:'Charizard',game:5,frames:32,variant:1,pinned:true,pending:false,favorite:false},
      {id:94,name:'Gengar',game:11,frames:20,variant:0,pinned:false,pending:false,favorite:false},
      {id:133,name:'Eevee',game:7,frames:15,variant:0,pinned:false,pending:false,favorite:false}
    ];
    const pk={layout:2,game:0,animatedOnly:1,generation:0,type:0,stage:0,legend:0,selection:0,specific:25,shiny:0,shinyChance:8,facing:0,female:0,interval:45,speed:100,loop:0,fps:25,scale:90,x:0,y:0,spacing:1,background:0,cellBackgrounds:0,palette:0,shadow:0,outline:0,glow:0,sparkle:0,staticMotion:0,transition:1,transitionMs:500,together:0,syncAnimations:0,labels:0,brightness:45,colorA:0,colorB:4226,paused:false,message:'Original game sprites',cache_hits:5,cells,favorites:[25]};
    page.on('pageerror', e => errors.push(e.message));
    await page.route('http://albumclock.local/**', async route => {
      const u = new URL(route.request().url());
      if(u.pathname==='/')return route.fulfill({body:html,contentType:'text/html'});
      if(u.pathname==='/status')return route.fulfill({json:{display_mode:mode,pokemon:mode===2?pk:undefined,wifi_connected:true,fw:'4.1.0-wiim',ip:'192.168.1.8',brightness:40,pack_count:279,playback:'idle',service_status:'Online',display_generation:1,...ambient}});
      if(u.pathname==='/frame.bin')return route.fulfill({body:Buffer.alloc(8192),contentType:'application/octet-stream'});
      if(u.pathname==='/pokemon/catalog')return route.fulfill({json:[{id:25,name:'Pikachu'},{id:133,name:'Eevee'},{id:6,name:'Charizard'}]});
      if(u.pathname==='/mode'){mode=Number(new URLSearchParams(route.request().postData()).get('value'));return route.fulfill({body:'mode updated'});}
      if(u.pathname==='/ambient'){for(const [key,value] of new URLSearchParams(route.request().postData()))ambient['am_'+key]=Number(value);return route.fulfill({body:'Ambient settings saved'});}
      if(u.pathname==='/album-audio-level')return route.fulfill({json:{level:178,ready:true,failed:false}});
      calls.push([u.pathname,route.request().postData()]);return route.fulfill({body:'ok'});
    });
    await page.goto('http://albumclock.local/');
    await page.waitForSelector('#appTitle:text("Pokémon Clock")');
    await page.waitForSelector('.pkcell');
    assert.deepEqual(await page.locator('.tabs button:visible').allTextContents(),['Display','Collection','Motion','Scene','Device']);
    assert.equal(await page.locator('.pkcell').count(),4);
    if(process.env.POKEMON_REMOTE_SCREENSHOT)await page.screenshot({path:process.env.POKEMON_REMOTE_SCREENSHOT,fullPage:true});
    await page.locator('.pkcell').nth(1).click();await page.locator('#pkPin').click();await page.waitForTimeout(300);
    assert.ok(calls.some(c=>c[0]==='/pokemon/action'&&c[1].includes('action=pin')&&c[1].includes('cell=1')));
    await page.getByRole('button',{name:'Collection',exact:true}).click();await page.locator('#pkSearch').fill('Eevee');await page.waitForSelector('#pkResults:text("Eevee")');
    assert.match(await page.locator('#pkResults').innerText(),/Eevee/);
    await page.selectOption('#mode','0');await page.waitForSelector('#appTitle:text("Album Clock")');
    assert.deepEqual(await page.locator('.tabs button:visible').allTextContents(),['Display','Ambient','Zoom','Image','Transitions','Device']);
    await page.getByRole('button',{name:'Ambient',exact:true}).click();
    await page.getByRole('button',{name:'♫ Audio pulse',exact:true}).click();
    await page.waitForSelector('#am_micgain:visible');
    assert.equal(await page.locator('#am_mode').inputValue(),'7');
    assert.equal(await page.locator('#am_target').inputValue(),'5');
    assert.equal(await page.locator('#am_density').isVisible(),false);
    await page.waitForFunction(()=>document.getElementById('micLevel').value===178);
    await page.selectOption('#am_mode','0');
    await page.waitForSelector('#micMeter',{state:'hidden'});
    assert.deepEqual(errors,[]);console.log('Pokémon and Audio Pulse mobile remote: mode isolation, grid actions, search, audio preset and mic meter passed');
  } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
