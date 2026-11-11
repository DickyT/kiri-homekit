// Run with PLAYWRIGHT_MODULE pointing to an installed Playwright package if it
// is not available through normal module resolution. No dependency installation.
import { build } from 'esbuild';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = fileURLToPath(new URL('../', import.meta.url));
const bundle = await build({ stdin: { contents: 'import {render} from "preact"; import {QingpingSettings} from "./QingpingSettings"; render(<QingpingSettings />,document.getElementById("root"));', resolveDir: root+'src', loader:'tsx' }, bundle:true, write:false, format:'esm', jsx:'automatic', jsxImportSource:'preact' });
const css = await readFile(root+'src/styles.css','utf8');
const browser = await chromium.launch({headless:true, channel:process.env.PLAYWRIGHT_CHANNEL || 'chrome'});
try {
  for (const width of [1280, 390]) {
    const context = await browser.newContext({ viewport:{width,height:900} });
    const page = await context.newPage();
    page.setDefaultTimeout(10000);
    const errors=[]; page.on('pageerror', e=>errors.push(e.message));
    const device = { address:'58:2D:34:80:17:3F', service_uuid:'0000fdcd-0000-1000-8000-00805f9b34fb', model:16, temperature_c:22.7, humidity:40, battery:85, rssi:-67, age_ms:0 };
    let state = {ok:true,bound:false,address:'',online:false,received:false,age_ms:0,temperature_c:0,humidity:0,battery:-1,scanning:false,paused:false,control_usable:false,command_pending:false,message:'Not bound',devices:[]};
    let failScan=false, bindingWrites=0, deletes=0;
    await page.route('http://qingping.test/**', async route=>{
      const req=route.request(), path=new URL(req.url()).pathname;
      if(path==='/') return route.fulfill({contentType:'text/html',body:`<!doctype html><meta name="viewport" content="width=device-width, initial-scale=1"><style>${css}</style><main id="root"></main><script type="module">${bundle.outputFiles[0].text}</script>`});
      if(path==='/api/qingping/scan') {
        if(failScan) return route.fulfill({status:400,contentType:'application/json',body:JSON.stringify({ok:false,error:'Bluetooth scan unavailable during provisioning'})});
        state={...state,scanning:true,devices:[device]};
      }
      if(path==='/api/qingping/bind' && req.method()==='POST') {
        assert.equal(new URLSearchParams(req.postData()).get('address'),device.address);
        bindingWrites++; state={...state,bound:true,address:device.address,received:false};
      }
      if(path==='/api/qingping/bind' && req.method()==='DELETE') {
        deletes++; state={...state,bound:false,address:'',received:false,online:false};
      }
      return route.fulfill({contentType:'application/json',body:JSON.stringify(state)});
    });
    await page.goto('http://qingping.test/');
    await page.waitForTimeout(300);
    assert.deepEqual(errors,[], 'No browser errors when mounting the modal component');
    const section = page.locator('section').filter({has:page.getByRole('heading',{name:'External Room Temperature',exact:true})});
    assert.equal(await section.count(),1);
    await section.getByText('Binding and unbinding take effect immediately', {exact:false}).waitFor();
    assert.ok(!(await section.innerText()).includes('Qingping'),'Settings entry must be sensor-brand neutral');
    assert.equal(await section.locator('.spec-row').count(),3);
    assert.equal(await section.getByRole('button').innerText(),'CONFIGURE ROOM SENSOR');
    await page.getByRole('button',{name:'Configure Room Sensor',exact:true}).click();
    await page.getByRole('heading',{name:'BLE Room Sensor',exact:true}).waitFor();
    await page.getByRole('heading',{name:'01 — Find your room sensor'}).waitFor();
    await section.getByText('Not bound',{exact:true}).waitFor();
    await page.getByRole('button',{name:'Refresh Nearby Sensors'}).click();
    await page.getByRole('heading',{name:'02 — Select one sensor'}).waitFor();
    assert.equal(bindingWrites,0);
    await page.getByRole('cell').filter({hasText:'22.7°C / 72.9°F'}).waitFor();
    assert.equal(await page.getByRole('button',{name:'Bind Selected Sensor'}).isEnabled(),false);
    await page.getByRole('radio').check();
    await page.getByRole('button',{name:'Bind Selected Sensor'}).click();
    await page.getByRole('heading',{name:'03 — Sensor bound successfully'}).waitFor();
    assert.equal(bindingWrites,1);
    state={...state,received:true,online:true,temperature_c:22.7,humidity:40,battery:85,control_usable:true};
    await page.getByRole('cell',{name:'Online',exact:true}).waitFor();
    await section.getByText('Bound · Active',{exact:true}).waitFor();
    assert.ok((await section.innerText()).includes('22.7°C / 72.9°F'));
    await page.getByRole('cell',{name:'22.7°C / 72.9°F',exact:true}).waitFor();
    state={...state,online:false,age_ms:300000,control_usable:false};
    await page.getByRole('cell',{name:'Offline',exact:true}).waitFor();
    await section.getByText('Bound · Offline',{exact:true}).waitFor();
    assert.ok((await section.locator('.spec-row').last().innerText()).includes('5m'));
    assert.equal(await section.getByRole('button').textContent(),'Configure Room Sensor');
    assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    await page.getByRole('button',{name:'Done',exact:true}).click();
    await page.getByRole('button',{name:'Configure Room Sensor',exact:true}).click();
    await page.getByRole('heading',{name:'Bound Sensor',exact:true}).waitFor();
    await page.getByRole('button',{name:'Unbind Sensor',exact:true}).click();
    await page.getByRole('button',{name:'Keep Sensor',exact:true}).click();
    assert.equal(deletes,0);
    await page.getByRole('button',{name:'Unbind Sensor',exact:true}).click();
    await page.getByRole('button',{name:'Unbind Sensor',exact:true}).click();
    await page.getByRole('heading',{name:'01 — Find your room sensor'}).waitFor();
    assert.equal(deletes,1);
    await section.getByText('Not bound',{exact:true}).waitFor();
    failScan=true;
    await page.getByRole('button',{name:'Refresh Nearby Sensors'}).click();
    await page.getByRole('alert').filter({hasText:'Bluetooth scan unavailable'}).waitFor();
    assert.deepEqual(errors,[]);
    console.log(`Qingping modal ${width}px: discover/select/bind/live/offline/reopen/unbind/error PASS`);
    await context.close();
  }
} finally { await browser.close(); }
