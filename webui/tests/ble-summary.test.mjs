// Uses an existing Playwright installation; same environment as qingping.test.mjs.
import {build} from 'esbuild';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const {chromium}=await import(process.env.PLAYWRIGHT_MODULE||'playwright');
const root=fileURLToPath(new URL('../',import.meta.url));
const bundle=await build({stdin:{contents:`import {render} from 'preact';import {BleSensorSummary} from './BleSensorSummary';import {status,lastFetchAt} from './store';window.setTestSensor=(sensor)=>{status.value={server_time_unix_ms:1794427200000,ble_sensor:sensor};lastFetchAt.value=Date.now();};render(<><section id="control"><div class="stats"><BleSensorSummary /></div></section><section id="admin"><BleSensorSummary variant="spec-row" /></section></>,document.getElementById('root'));`,resolveDir:root+'src',loader:'tsx'},bundle:true,write:false,format:'esm',jsx:'automatic',jsxImportSource:'preact'});
const css=await readFile(root+'src/styles.css','utf8');
const browser=await chromium.launch({headless:true,channel:process.env.PLAYWRIGHT_CHANNEL||'chrome'});
try {
  for(const width of [1280,390]) {
    const context=await browser.newContext({viewport:{width,height:900},locale:'en-US',timezoneId:'America/Los_Angeles'});
    const page=await context.newPage();page.setDefaultTimeout(10000);
    const errors=[];page.on('pageerror',e=>errors.push(e.message));
    let extraRequests=0;
    await page.route('http://ble-summary.test/**',route=>{
      if(new URL(route.request().url()).pathname!=='/'){extraRequests++;return route.fulfill({status:404});}
      return route.fulfill({contentType:'text/html',body:`<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><style>${css}</style><main id="root"></main><script type="module">${bundle.outputFiles[0].text}</script>`});
    });
    await page.goto('http://ble-summary.test/');
    for(const id of ['control','admin']){
      await page.locator(`#${id} .val`).filter({hasText:'Not connected'}).first().waitFor();
      assert.equal(await page.locator(`#${id} .val`).allTextContents().then(x=>x.join('|')),'Not connected|Not connected');
    }
    const sample={bound:true,received:true,online:true,paused:false,temperature_c:22.7,age_ms:5000,timeout_ms:300000};
    await page.evaluate(s=>window.setTestSensor(s),sample);
    await page.locator('#control .val').filter({hasText:'22.7°C / 72.9°F'}).waitFor();
    await page.locator('#admin .val').filter({hasText:'22.7°C / 72.9°F'}).waitFor();
    const expectedTime=await page.evaluate(()=>new Date(1794427200000-5000).toLocaleString());
    for(const id of ['control','admin'])assert.ok((await page.locator(`#${id} .val`).nth(1).innerText()).startsWith(expectedTime));
    await page.waitForTimeout(1200);
    for(const id of ['control','admin'])assert.ok((await page.locator(`#${id} .val`).nth(1).innerText()).startsWith(expectedTime));
    await page.evaluate(s=>window.setTestSensor(s),{...sample,online:false,age_ms:300000});
    for(const id of ['control','admin']){
      await page.locator(`#${id} .val`).first().filter({hasText:'Not connected'}).waitFor();
      assert.ok((await page.locator(`#${id} .val`).nth(1).innerText()).includes('not connected'));
    }
    await page.evaluate(s=>window.setTestSensor(s),{...sample,received:false});
    await page.locator('#admin .val').nth(1).filter({hasText:'Not connected'}).waitFor();
    assert.equal(extraRequests,0,'Summary must not introduce its own API polling');
    assert.deepEqual(errors,[]);
    assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    console.log(`BLE summary ${width}px: both page layouts, temperature, stable timestamp, offline/unreceived, no extra requests PASS`);
    await context.close();
  }
}finally{await browser.close();}
