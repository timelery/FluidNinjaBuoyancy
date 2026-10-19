const fs = require('fs');
const path = require('path');
const {pathToFileURL}=require('url');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE || 'playwright');
(async()=>{
  const browser=await chromium.launch({...(process.env.CHROME_PATH ? {executablePath:process.env.CHROME_PATH} : {channel:'chrome'}),headless:true});
  const page=await browser.newPage({viewport:{width:1440,height:1100},deviceScaleFactor:1});
  const qa=path.resolve(process.env.GUIDE_QA_DIR || path.join(__dirname,'qa'));
  fs.mkdirSync(qa,{recursive:true});
  const errors=[]; page.on('pageerror',e=>errors.push(e.message));
  await page.goto(pathToFileURL(path.join(__dirname,'index.html')).href);
  await page.screenshot({path:path.join(qa,'qa-desktop.png')});
  const calc=[];
  for(const key of ['ball','log','boat','steel']){
    await page.evaluate(k=>presetCalc(k),key);
    calc.push({key,text:await page.locator('#calcresult').innerText()});
  }
  const edgeCases=await page.evaluate(()=>{
    const set=(values)=>{Object.entries(values).forEach(([id,value])=>document.getElementById(id).value=value);calculate();return document.getElementById('calcresult').innerText};
    return {
      halfBox:set({shape:'box',length:100,width:100,height:100,mass:500,rho:1000}),
      neutralBox:set({mass:1000}),
      invalidMass:set({mass:0})
    };
  });
  if(!edgeCases.halfBox.includes('50.00 cm draft')||!edgeCases.neutralBox.includes('Neutral when submerged')||!edgeCases.invalidMass.includes('Enter positive'))throw new Error('Calculator edge case failed');
  await page.locator('#propertySearch').fill('WaveImpulseScale');
  const filtered=await page.locator('.propgroup:not([hidden]) tbody tr:not([hidden])').count();
  await page.locator('#propertySearch').fill('');
  await page.evaluate(()=>presetCalc('ball'));
  const broken=await page.evaluate(()=>[...document.querySelectorAll('a[href^="#"]')].filter(a=>!document.getElementById(a.getAttribute('href').slice(1))).map(a=>a.getAttribute('href')));
  const horizontal=await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth);
  await page.pdf({path:path.join(__dirname,'FluidNinja_Architecture_and_Buoyancy_Guide.pdf'),format:'A4',printBackground:true,preferCSSPageSize:true,displayHeaderFooter:true,tagged:true,outline:true,
    headerTemplate:'<div style="font:8px Arial;color:#607887;width:100%;margin:0 60px;">FLUIDNINJA ARCHITECTURE AND BUOYANCY</div>',
    footerTemplate:'<div style="font:8px Arial;color:#607887;width:100%;margin:0 60px;display:flex;justify-content:space-between;"><span>Source audit · 18 October 2026</span><span><span class="pageNumber"></span> / <span class="totalPages"></span></span></div>'});
  await page.setViewportSize({width:390,height:844});
  await page.goto(pathToFileURL(path.join(__dirname,'index.html')).href);
  await page.addStyleTag({content:'html{scroll-behavior:auto!important}'});
  await page.locator('#recipes').scrollIntoViewIfNeeded();
  await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
  await page.screenshot({path:path.join(qa,'qa-mobile.png')});
  const mobileOverflow=await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth);
  fs.writeFileSync(path.join(qa,'qa-browser.json'),JSON.stringify({errors,calc,edgeCases,filtered,broken,horizontal,mobileOverflow},null,2));
  if(errors.length||broken.length||horizontal||mobileOverflow||filtered!==1)throw new Error('Browser verification failed');
  await browser.close();
  console.log(JSON.stringify({errors,filtered,broken,horizontal,mobileOverflow,calc,edgeCases}));
})().catch(e=>{console.error(e);process.exit(1)});
