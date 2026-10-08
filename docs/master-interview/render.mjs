// Export the offline document with a local Chromium-compatible browser.
// Override the executable with INTERVIEW_BROWSER_PATH when needed.
import {spawn,execFileSync} from 'node:child_process';
import {writeFile,readFile} from 'node:fs/promises';
import {dirname,join} from 'node:path';
import {fileURLToPath,pathToFileURL} from 'node:url';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';

const root=dirname(fileURLToPath(import.meta.url));
const expected=JSON.parse(await readFile(join(root,'build-report.json'),'utf8'));
const executable=process.env.INTERVIEW_BROWSER_PATH||'/home/jinjie/.cache/puppeteer/chrome-headless-shell/linux-148.0.7778.97/chrome-headless-shell-linux64/chrome-headless-shell';
const browser=spawn(executable,[
  '--headless','--no-sandbox','--disable-gpu','--disable-dev-shm-usage',
  '--disable-background-networking','--disable-component-update',
  '--no-first-run','--no-default-browser-check','--allow-file-access-from-files',
  '--remote-debugging-pipe','--user-data-dir=/tmp/master-interview-browser-profile'
],{stdio:['ignore','ignore','pipe','pipe','pipe']});
let nextId=0,buffer='',errors='';
const pending=new Map(),exceptions=[];
browser.stderr.on('data',chunk=>errors+=chunk);
browser.on('error',error=>{errors+=error.message;for(const p of pending.values())p.reject(error);pending.clear()});
for(const stream of [browser.stdio[3],browser.stdio[4]])stream.on('error',error=>{errors+='\n'+error.message});
browser.stdio[4].on('data',chunk=>{
  buffer+=chunk.toString();let at;
  while((at=buffer.indexOf('\0'))>=0){
    const raw=buffer.slice(0,at);buffer=buffer.slice(at+1);if(!raw)continue;
    const msg=JSON.parse(raw);
    if(msg.id){const p=pending.get(msg.id);if(p){pending.delete(msg.id);msg.error?p.reject(new Error(JSON.stringify(msg.error))):p.resolve(msg.result)}}
    if(msg.method==='Runtime.exceptionThrown')exceptions.push(msg.params.exceptionDetails);
  }
});
browser.on('exit',()=>{for(const p of pending.values())p.reject(new Error('Browser stopped: '+errors.slice(-1800)));pending.clear()});
function call(method,params={},sessionId){return new Promise((resolve,reject)=>{
  const id=++nextId;pending.set(id,{resolve,reject});
  browser.stdio[3].write(JSON.stringify({id,method,params,...(sessionId?{sessionId}:{})})+'\0');
})}
async function evaluate(sid,expression){
  const r=await call('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true},sid);
  if(r.exceptionDetails)throw new Error(JSON.stringify(r.exceptionDetails));return r.result.value;
}
async function open(width=1440,height=1050,stem="full"){
  const {targetId}=await call('Target.createTarget',{url:'about:blank'});
  const {sessionId:sid}=await call('Target.attachToTarget',{targetId,flatten:true});
  await call('Runtime.enable',{},sid);await call('Page.enable',{},sid);
  await call('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:1,mobile:width<760},sid);
  await call('Page.navigate',{url:pathToFileURL(join(root,stem+'.html')).href},sid);
  await evaluate(sid,'new Promise(resolve=>{if(document.readyState==="complete")resolve(true);else window.addEventListener("load",()=>resolve(true),{once:true})})');
  await evaluate(sid,'document.fonts.ready.then(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))))');
  return sid;
}
async function screenshot(sid,name,selector){
  const clip=selector?await evaluate(sid,`(()=>{const r=document.querySelector(${JSON.stringify(selector)}).getBoundingClientRect();return {x:r.x+scrollX,y:r.y+scrollY,width:r.width,height:r.height,scale:1}})()`):null;
  const r=await call('Page.captureScreenshot',{format:'png',...(clip?{clip,captureBeyondViewport:true}:{})},sid);
  await writeFile('/tmp/master-interview-'+name+'.png',Buffer.from(r.data,'base64'));
}
async function navigate(sid,file){
  await call('Page.navigate',{url:pathToFileURL(join(root,file)).href},sid);
  await evaluate(sid,'new Promise(r=>document.readyState==="complete"?r():window.addEventListener("load",r,{once:true}))');
  await evaluate(sid,'document.fonts.ready');
}
const timer=setTimeout(()=>browser.kill('SIGTERM'),150000);
const normalize=s=>s.normalize('NFKC').replace(/[\s\u00ad\u200b]+/g,'');
try{
  const desktop=await open(1440,1000,'index');
  await screenshot(desktop,'directory');
  const report={parts:[],chapterPages:[],pdfs:{},artifacts:{}};
  const mobile=await open(390,844,'index');
  assert.equal(await evaluate(mobile,'document.documentElement.scrollWidth>innerWidth+1'),false);
  await screenshot(mobile,'directory-mobile');
  const definitions=[['theory','第一部分_纯八股.pdf',expected.theory_questions,expected.theory_chapters],['project','第二部分_项目面经.pdf',expected.project_questions,expected.project_chapters],['full','网关与节点总面经.pdf',expected.theory_questions+expected.project_questions,expected.theory_chapters+expected.project_chapters]];
  const texts={};
  for(const [stem,pdfname,total,chapters] of definitions){
    await navigate(desktop,stem+'.html');
    const result=await evaluate(desktop,`(()=>{const ids=[...document.querySelectorAll('[id]')].map(e=>e.id);return {questions:document.querySelectorAll('.qa').length,chapters:document.querySelectorAll('.chapter').length,theory:document.querySelectorAll('.qa.theory').length,project:document.querySelectorAll('.qa.project').length,duplicateIds:ids.filter((x,i)=>ids.indexOf(x)!==i),overflow:document.documentElement.scrollWidth>innerWidth+1,externalAssets:[...document.querySelectorAll('script[src],link[rel="stylesheet"],img[src]')].map(x=>x.src||x.href),brokenAnchors:[...document.querySelectorAll('a[href^="#"]')].filter(a=>!document.getElementById(decodeURIComponent(a.getAttribute('href').slice(1)))).map(a=>a.getAttribute('href'))}})()`);
    assert.equal(result.questions,total);assert.equal(result.chapters,chapters);assert.deepEqual(result.duplicateIds,[]);assert.deepEqual(result.externalAssets,[]);assert.deepEqual(result.brokenAnchors,[]);assert.equal(result.overflow,false);
    if(stem==='theory')assert.equal(result.project,0);
    if(stem==='project')assert.equal(result.theory,0);
    const target=stem==='project'?'P037':'T075';
    await evaluate(desktop,`document.getElementById('search').value='${target}';document.getElementById('search').dispatchEvent(new Event('input'));`);
    assert.equal(await evaluate(desktop,'document.querySelectorAll(".qa:not([hidden])").length'),1);
    await screenshot(desktop,stem+'-question','#'+target);
    await evaluate(desktop,`document.getElementById('search').value='DMA';document.getElementById('search').dispatchEvent(new Event('input'));`);
    const matches=await evaluate(desktop,'document.querySelectorAll(".qa:not([hidden])").length');assert.ok(matches>1&&matches<total);
    await evaluate(desktop,`document.getElementById('search').value='none不存在987654321';document.getElementById('search').dispatchEvent(new Event('input'));`);
    assert.equal(await evaluate(desktop,"document.getElementById('empty').hidden"),false);
    await evaluate(desktop,"document.getElementById('reset').click();document.getElementById('quiz').click()");
    assert.equal(await evaluate(desktop,"getComputedStyle(document.querySelector('.answer-content')).display"),'none');
    await evaluate(desktop,"document.querySelector('.reveal').click()");
    assert.notEqual(await evaluate(desktop,"getComputedStyle(document.querySelector('.answer-content')).display"),'none');
    await evaluate(desktop,"document.getElementById('reset').click();document.getElementById('chapter-filter').selectedIndex=1;document.getElementById('chapter-filter').dispatchEvent(new Event('change'))");
    assert.ok(await evaluate(desktop,"document.querySelectorAll('.qa:not([hidden])').length < document.querySelectorAll('.qa').length"));
    await evaluate(desktop,"document.getElementById('reset').click();window.scrollTo(0,0)");
    texts[pdfname]=await evaluate(desktop,"[...document.querySelectorAll('.bodytext')].map(e=>({question:e.closest('.qa')?.id,text:e.textContent}))");
    const pdf=await call('Page.printToPDF',{printBackground:true,preferCSSPageSize:true,displayHeaderFooter:true,generateDocumentOutline:true,headerTemplate:'<span></span>',footerTemplate:'<div style="font-family:Arial;font-size:8px;width:100%;text-align:center;color:#607783">Gateway + STM32 / Interview 2026 &nbsp; · &nbsp; <span class="pageNumber"></span> / <span class="totalPages"></span></div>'},desktop);
    await writeFile(join(root,pdfname),Buffer.from(pdf.data,'base64'));
    report.parts.push({file:stem+'.html',...result});
    await navigate(mobile,stem+'.html');
    assert.equal(await evaluate(mobile,'document.documentElement.scrollWidth>innerWidth+1'),false);
    await evaluate(mobile,`location.hash='${target}'`);
    await evaluate(mobile,'new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r)))');
    await screenshot(mobile,stem+'-mobile');
    console.log('Rendered '+pdfname);
  }
  report.pdfLinks=JSON.parse(execFileSync('python3',[join(root,'pdf_links.py')],{encoding:'utf8'}));
  for(const [stem,pdfname,total] of definitions){
    const content=execFileSync('pdftotext',['-layout',join(root,pdfname),'-'],{encoding:'utf8',maxBuffer:20*1024*1024});
    const flat=normalize(content.replace(/^.*Gateway \+ STM32 \/ Interview 2026.*$/gm,'')),pages=content.split('\f');if(!pages.at(-1).trim())pages.pop();
    const missing=texts[pdfname].filter(v=>!flat.includes(normalize(v.text)));
    report.pdfs[pdfname]={pages:pages.length,bodyParagraphs:texts[pdfname].length,searchableParagraphs:texts[pdfname].length-missing.length,missing,sparsePages:pages.map((p,i)=>({page:i+1,characters:normalize(p).length})).filter(p=>p.characters<60)};
    assert.deepEqual(missing,[],pdfname+' contains missing paragraphs');
    assert.deepEqual(report.pdfs[pdfname].sparsePages,[],pdfname+' contains nearly empty pages');
  }
  for(const c of expected.chapters){
    await navigate(mobile,c.file);
    const result=await evaluate(mobile,`({questions:document.querySelectorAll('.qa').length,overflow:document.documentElement.scrollWidth>innerWidth+1})`);
    assert.equal(result.questions,c.questions);assert.equal(result.overflow,false);report.chapterPages.push({file:c.file,...result});
  }
  // Review the new FreeRTOS porting questions at both desktop and phone widths.
  for(const [stem,key] of [['theory','T119'],['project','P053']]){
    await navigate(desktop,stem+'.html');await screenshot(desktop,'freertos-port-'+stem,'#'+key);
    await navigate(mobile,stem+'.html');
    assert.equal(await evaluate(mobile,'document.documentElement.scrollWidth>innerWidth+1'),false);
    await screenshot(mobile,'freertos-port-'+stem+'-mobile','#'+key);
  }
  // Review the current multi-loop and third-party HTTP architecture on both widths.
  for(const key of ['P004','P012','P014']){
    await navigate(desktop,'project.html');await screenshot(desktop,'architecture-'+key,'#'+key);
    await navigate(mobile,'project.html');
    assert.equal(await evaluate(mobile,'document.documentElement.scrollWidth>innerWidth+1'),false);
    await screenshot(mobile,'architecture-'+key+'-mobile','#'+key);
  }
  // The repository architecture diagram is a code-native SVG, also reviewed offline.
  await call('Emulation.setDeviceMetricsOverride',{width:1480,height:1100,deviceScaleFactor:1,mobile:false},desktop);
  await navigate(desktop,'../architecture.svg');
  const svgTextOverflow=await evaluate(desktop,`[...document.querySelectorAll('text')].filter(e=>{const b=e.getBBox();return b.x<0||b.y<0||b.x+b.width>1480||b.y+b.height>1100}).map(e=>e.textContent)`);
  assert.deepEqual(svgTextOverflow,[]);await screenshot(desktop,'architecture-overview');
  report.architectureSvg='Text fits viewBox; offline screenshot captured for visual review.';
  await call('Emulation.setDeviceMetricsOverride',{width:1440,height:1000,deviceScaleFactor:1,mobile:false},desktop);
  // Follow a real project-to-theory link, including the target question anchor.
  await navigate(desktop,'project.html');
  await evaluate(desktop,"document.querySelector('#P037 .theory-backlinks a').click()");
  await evaluate(desktop,'new Promise(r=>setTimeout(r,150))');
  assert.equal(await evaluate(desktop,'location.hash'),'#T072');
  assert.equal(await evaluate(desktop,"document.querySelector('.qa.project')"),null);
  assert.equal(await evaluate(desktop,"document.getElementById('T072').hidden"),false);
  report.crossPartNavigation='P037 → theory.html#T072 passed';
  report.browserExceptions=exceptions;assert.deepEqual(exceptions,[]);
  for(const file of ['index.html','theory.html','project.html','full.html','第一部分_纯八股.pdf','第二部分_项目面经.pdf','网关与节点总面经.pdf'])report.artifacts[file]=createHash('sha256').update(await readFile(join(root,file))).digest('hex');
  report.interactions='Question/text search, no results, chapter filter, reset, self-test/reveal, exact theory backlinks and mobile layout passed.';
  report.scope='Document only; no new application, hardware or performance tests.';
  await writeFile(join(root,'validation.json'),JSON.stringify(report,null,2)+'\n');
  console.log(JSON.stringify({parts:report.parts,pdfs:report.pdfs,chapterPages:report.chapterPages.length,pdfLinks:report.pdfLinks.files},null,2));
}catch(error){console.error(error.stack||String(error));console.error(errors.slice(-1800));process.exitCode=1;}
finally{clearTimeout(timer);if(browser.exitCode===null&&!browser.killed)browser.kill('SIGTERM');}
