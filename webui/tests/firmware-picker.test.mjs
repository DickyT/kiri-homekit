// Headless browser tests; use existing Playwright via PLAYWRIGHT_MODULE if needed.
import {build} from 'esbuild';
import {readFile,readdir} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const {chromium}=await import(process.env.PLAYWRIGHT_MODULE||'playwright');
const root=fileURLToPath(new URL('../',import.meta.url));
const bundle=await build({stdin:{contents:`import{render}from'preact';import{useState}from'preact/hooks';import{FirmwarePicker}from'./FirmwarePicker';window.selected=[];function Test(){const[disabled,setDisabled]=useState(false);window.disablePicker=setDisabled;return <FirmwarePicker disabled={disabled} onSelect={file=>{window.selected.push(file.name);return new Promise(resolve=>window.finishUpload=resolve)}}/>}render(<Test/>,document.getElementById('root'));`,resolveDir:root+'src',loader:'tsx'},bundle:true,write:false,jsx:'automatic',jsxImportSource:'preact',format:'esm'});
const css=await readFile(root+'src/styles.css','utf8');
const browser=await chromium.launch({headless:true,channel:process.env.PLAYWRIGHT_CHANNEL||'chrome'});
async function drop(page,selector,names,type='drop') {
  return page.evaluate(({selector,names,type})=>{
    const dt=new DataTransfer();for(const name of names)dt.items.add(new File(['fixture'],name));
    const event=new DragEvent(type,{bubbles:true,cancelable:true,dataTransfer:dt});
    document.querySelector(selector).dispatchEvent(event);return event.defaultPrevented;
  },{selector,names,type});
}
try {
  const page=await browser.newPage();page.setDefaultTimeout(10000);
  await page.route('http://localhost:9876/**',r=>r.fulfill({contentType:'text/html',body:`<!doctype html><style>${css}</style><div id="root"></div><script type="module">${bundle.outputFiles[0].text}</script>`}));
  await page.goto('http://localhost:9876/');
  assert.equal(await drop(page,'.firmware-drop-zone',['bad.bin']),true);
  await page.getByRole('alert').filter({hasText:'Only .kiri'}).waitFor();
  await drop(page,'.firmware-drop-zone',['one.kiri','two.kiri']);
  await page.getByRole('alert').filter({hasText:'one .kiri'}).waitFor();
  assert.deepEqual(await page.evaluate(()=>window.selected),[]);
  await drop(page,'.firmware-drop-zone',['update.kiri'],'dragover');
  assert.ok(await page.locator('.firmware-drop-zone').evaluate(el=>el.classList.contains('dragging')));
  await drop(page,'.firmware-drop-zone',['update.kiri']);
  await page.getByRole('button',{name:'Uploading…'}).waitFor();
  await drop(page,'.firmware-drop-zone',['duplicate.kiri']);
  assert.deepEqual(await page.evaluate(()=>window.selected),['update.kiri']);
  await page.evaluate(()=>window.finishUpload());
  await page.getByRole('button',{name:'Choose Firmware'}).waitFor();
  const chooserPromise=page.waitForEvent('filechooser');
  await page.getByRole('button',{name:'Choose Firmware'}).click();
  await (await chooserPromise).setFiles({name:'update.kiri',mimeType:'application/octet-stream',buffer:Buffer.from('fixture')});
  await page.getByRole('button',{name:'Uploading…'}).waitFor();
  assert.deepEqual(await page.evaluate(()=>window.selected),['update.kiri','update.kiri']);
  await page.evaluate(()=>window.finishUpload());
  await page.evaluate(()=>window.disablePicker(true));
  await page.waitForFunction(()=>document.querySelector('button').disabled);
  await drop(page,'.firmware-drop-zone',['disabled.kiri']);
  assert.equal(await page.evaluate(()=>window.selected.length),2);
  assert.equal(await drop(page,'body',['outside.kiri']),true);
  console.log('Shared firmware picker: click/drop, extension, single file, drag highlight, duplicate/busy/disabled guards, outside-drop navigation PASS');
  await page.close();

  const site=fileURLToPath(new URL('../../site/',import.meta.url));
  const exports=fileURLToPath(new URL('../../firmware_exports/',import.meta.url));
  const versions=(await readdir(exports)).filter(v=>/^\d{4}\.\d{4}\.\d{6}$/.test(v)).sort();
  const v=versions.at(-1), fixture=await readFile(`${exports}${v}/kiri_bridge_atom-lite_${v}.kiri`);
  const flash=await browser.newPage();flash.setDefaultTimeout(10000);
  const errors=[];flash.on('pageerror',e=>errors.push(e.message));
  await flash.addInitScript(()=>Object.defineProperty(navigator,'serial',{value:{requestPort:async()=>({getInfo:()=>({usbVendorId:0x403,usbProductId:0x6001}),open:async()=>{},close:async()=>{}})}}));
  await flash.route('https://cdn.jsdelivr.net/**',r=>r.fulfill({contentType:'application/javascript',body:`export class Transport{constructor(){}async disconnect(){}}export class ESPLoader{constructor(){this.chip={getChipDescription:async()=>"ESP32-PICO-D4",readMac:async()=>"90:15:06:00:00:01"}}async detectChip(){}async after(){}}`}));
  await flash.route('http://localhost:9877/**',async r=>{
    const path=new URL(r.request().url()).pathname;
    if(path==='/fixture.kiri')return r.fulfill({contentType:'application/octet-stream',body:fixture});
    const file=path==='/flash.html'?'flash.html':path.slice(1);
    try {return r.fulfill({contentType:file.endsWith('.js')?'application/javascript':file.endsWith('.css')?'text/css':'text/html',body:await readFile(site+file)});}
    catch{return r.fulfill({status:404});}
  });
  await flash.goto('http://localhost:9877/flash.html');
  await drop(flash,'#package-drop-zone',['early.kiri']);
  assert.equal(await flash.locator('#step-serial').isVisible(),true);
  await flash.getByRole('button',{name:'Select & verify',exact:true}).click();
  await flash.locator('#step-package').waitFor({state:'visible'});
  await drop(flash,'#package-drop-zone',['wrong.bin']);
  await flash.getByText('Choose a .kiri firmware / OTA file.',{exact:true}).waitFor();
  await flash.evaluate(async()=>{
    const bytes=await(await fetch('/fixture.kiri')).arrayBuffer();const dt=new DataTransfer();dt.items.add(new File([bytes],'update.kiri'));
    document.getElementById('package-drop-zone').dispatchEvent(new DragEvent('drop',{bubbles:true,cancelable:true,dataTransfer:dt}));
  });
  await flash.locator('#step-flash').waitFor({state:'visible'});
  assert.equal(await flash.locator('#flash-btn').isDisabled(),true,'Dropping a file never bypasses hardware confirmation');
  assert.ok((await flash.locator('#package-details').textContent()).includes(v));
  await drop(flash,'#package-drop-zone',['out-of-step.kiri']);
  assert.equal(await flash.locator('#step-flash').isVisible(),true);
  assert.deepEqual(errors,[]);
  console.log('Public USB flasher: pre-verification/out-of-step guards, invalid extension, real .kiri drag/drop parsing and hardware confirmation PASS');
  await flash.close();
} finally {await browser.close();}
