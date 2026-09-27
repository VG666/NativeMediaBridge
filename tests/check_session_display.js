'use strict';
// Run the real injection closure with a small DOM/bridge harness; no dependencies.
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'hook/js/injection.js'), 'utf8');
function harness() {
  let now = 1000, next = 0;
  const timers = new Map(), requests = [], observers = [], events = {};
  const document = {documentElement:null, head:null, cookie:'', baseURI:'https://example.test/',
    addEventListener(type,fn){(events[type]||(events[type]=[])).push(fn)},
    dispatchEvent(e){for(const fn of events[e.type]||[])fn(e)}, querySelectorAll(){return []}};
  const context = {document, navigator:{userAgent:'test'}, location:{href:document.baseURI, protocol:'https:',origin:'https://example.test'},
    crypto:require('crypto').webcrypto, Uint32Array, URL, Promise, Set,
    Date:{now:()=>now}, Element:function(){}, HTMLMediaElement:function(){}, Event:function(type){this.type=type},
    getComputedStyle:n=>n.css || {overflow:'visible',overflowX:'visible',overflowY:'visible'},
    setTimeout(fn,ms){timers.set(++next,{fn,ms});return next}, clearTimeout(id){timers.delete(id)},
    MutationObserver:function(fn){this.observe=(target,options)=>observers.push({fn,target,options})},
    innerWidth:1000,innerHeight:1000, addEventListener(){},requestAnimationFrame(){},
    mbQuery(n,req,callback){requests.push({fields:req.split('\t'),callback})}};
  context.window=context; context.top=context;
  context.HTMLMediaElement.prototype={play(){},pause(){},load(){}};
  const exposed = source.split('})();(function(){')[0].replace('    reportTitle();document.addEventListener',
    '    window.test={hook:hook,send:send,unhook:unhook,clipBox:clipBox,syncAll:syncAll,fsEnter:fsEnter,fsExit:fsExit,installMediaObserver:installMediaObserver};\n    reportTitle();document.addEventListener')+'})();';
  vm.runInNewContext(exposed,context);
  assert(context.test, 'injection must finish even without documentElement');
  function el() {return {tagName:'VIDEO',nodeType:1,currentSrc:'https://example.test/a.mp4',src:'',isConnected:true,
    style:{},paused:true,autoplay:false,events:[],listeners:{},parentElement:null,
    getAttribute(){return ''},hasAttribute(){return false},setAttribute(){},removeAttribute(){},
    getBoundingClientRect(){return {left:0,top:0,right:100,bottom:100,width:100,height:100}},
    addEventListener(name,fn){(this.listeners[name]||(this.listeners[name]=[])).push(fn)},
    dispatchEvent(e){this.events.push(e.type);for(const fn of this.listeners[e.type]||[])fn(e)}}}
  return {context,el,requests,timers,observers,api:context.test,advance(ms){now+=ms},
    reply(r,data){r.callback(0,JSON.stringify(data))},last(){return requests[requests.length-1]}};
}
const a=harness(),b=harness(),x=a.el(),y=b.el();
a.api.hook(x);b.api.hook(y);
assert.notStrictEqual(x.__nmbId,y.__nmbId,'different frame/document IDs');
const old=a.last();x.currentSrc='https://example.test/b.mp4';a.api.hook(x);
const fresh=a.last();a.reply(old,{ok:true,duration:999});assert(!x.__nmbHooked);
a.reply(fresh,{ok:true,duration:20,video:true,paused:true});assert(x.__nmbHooked);assert.equal(x.duration,20);
a.api.send(x,'state');const stale=a.last();
x.currentSrc='https://example.test/c.mp4';a.api.hook(x);
a.reply(stale,{ok:true,duration:999,missing:1});assert(!x.__nmbHooked);assert(!x.__nmbState);
a.reply(a.last(),{ok:true,duration:30,video:true,paused:true});assert(x.__nmbHooked);
assert.equal(x.listeners.play.length,1,'re-adoption must not duplicate listeners');
x.isConnected=false;a.api.unhook(x);assert(!x.__nmbSealed);assert(!x.__nmbHooked);
x.isConnected=true;a.api.hook(x);a.reply(a.last(),{ok:true,duration:30,video:true,paused:true});assert(x.__nmbHooked);
x.currentSrc='blob:test';a.api.hook(x);assert(!x.__nmbHooked);assert(x.__nmbSealed);
x.currentSrc='https://example.test/d.mp4';a.api.hook(x);assert(x.__nmbOpening);assert(!x.__nmbSealed);
// A timed-out attempt cannot adopt after the next attempt begins.
const c=harness(),z=c.el();c.api.hook(z);const late=c.last();
c.timers.get(z.__nmbOpenGuard).fn();c.advance(10000);c.api.hook(z);
c.reply(late,{ok:true,duration:888});assert(!z.__nmbHooked);
// Failed/busy attempts are bounded and permanently skipped for this source; a new source resets it.
for(const busy of [false,true]) {
  const h=harness(),e=h.el();
  for(let i=0;i<12;i++){h.api.hook(e);h.reply(h.last(),{ok:false,busy});h.advance(8000)}
  assert(e.__nmbSkip);const n=h.requests.length;h.advance(30000);h.api.hook(e);assert.equal(h.requests.length,n);
  e.currentSrc='https://example.test/retry.mp4';h.api.hook(e);assert(!e.__nmbSkip);assert(e.__nmbOpening);
  assert(h.requests.some(r=>r.fields[0]==='close'));
}
// Explicit play/load revives the same source, including repeated missing exhaustion.
for(const method of ['play','load']) {
  const h=harness(),e=h.el();h.api.hook(e);
  e.__nmbSkip=true;e.__nmbLost=4;e.__nmbTries=12;
  h.context.HTMLMediaElement.prototype[method].call(e);
  assert(!e.__nmbSkip);assert.equal(e.__nmbLost,0);assert(e.__nmbOpening);
  assert.equal(h.last().fields[0],'open');assert.equal(e.__nmbTries,1);
  if(method==='play')assert(e.__nmbWantPlay);
}
// No fake MSE, and native implementations must remain untouched.
const compat=fs.readFileSync(path.join(root,'hook/js/compat_shim.js'),'utf8');
for(const native of [false,true]) {
  const c={__nmbCompatMode:'desktop-mse',addEventListener(){},URL:{createObjectURL(){return 'real'}}};
  if(native){c.MediaSource=function(){};c.SourceBuffer=function(){};c.ManagedMediaSource=function(){}}
  const before=[c.MediaSource,c.SourceBuffer,c.ManagedMediaSource,c.URL.createObjectURL];
  c.window=c;vm.runInNewContext(compat,c);
  assert.deepStrictEqual([c.MediaSource,c.SourceBuffer,c.ManagedMediaSource,c.URL.createObjectURL],before);
}
// Early injection eventually installs exactly one observer, including style invalidation.
const h=harness();h.context.document.documentElement={};h.api.installMediaObserver();h.api.installMediaObserver();
assert.equal(h.observers.length,1);assert(h.observers[0].options.attributes);
const e=h.el(),p={nodeType:1,parentElement:null,css:{overflow:'visible',overflowX:'visible',overflowY:'visible'},
 getBoundingClientRect(){return {left:10,top:10,right:50,bottom:50}}};e.parentElement=p;
assert.equal(h.api.clipBox(e,e.getBoundingClientRect()).r,100);
p.css.overflow='hidden';h.observers[0].fn([]);assert.equal(h.api.clipBox(e,e.getBoundingClientRect()).r,50);
p.css.borderRightWidth='5';h.advance(251);assert.equal(h.api.clipBox(e,e.getBoundingClientRect()).r,45);
e.parentElement=null;assert.equal(h.api.clipBox(e,e.getBoundingClientRect()).r,100);
// Axis-specific clipping includes the scrollable client area.
p.css={overflow:'visible',overflowX:'hidden',overflowY:'visible'};
p.clientLeft=2;p.clientTop=2;p.clientWidth=25;p.clientHeight=25;p.offsetWidth=40;p.offsetHeight=40;
e.parentElement=p;h.advance(251);
let clipped=h.api.clipBox(e,e.getBoundingClientRect());
assert.equal(clipped.l,12);assert.equal(clipped.r,37);assert.equal(clipped.t,0);assert.equal(clipped.b,100);
// Zero-sized media must stop drawing immediately, without waiting for release.
const hidden=harness(),hv=hidden.el();hidden.api.hook(hv);
hidden.reply(hidden.last(),{ok:true,duration:20,video:true,paused:true});
hidden.context.document.querySelectorAll=s=>s==='audio,video'?[hv]:[];
hv.getBoundingClientRect=()=>({left:0,top:0,right:0,bottom:0,width:0,height:0});
hidden.api.syncAll();assert.equal(hidden.last().fields[0],'rect');
assert.deepStrictEqual(hidden.last().fields.slice(17,21),['0','0','0','0']);assert(hv.__nmbHooked);
// Geometry supplied by the caller must be the geometry sent over the bridge.
hidden.api.send(hv,'rect',null,null,false,{left:13,top:27,width:120,height:80});
assert.deepStrictEqual(hidden.last().fields.slice(9,13),['13','27','120','80']);
// Native drawing contract: no-frame / failed blit must never reach drawn or soft-hole blending.
// Exercise the actual collector and event-driven refresh, without a browser dependency.
const overlay=harness(),v=overlay.el();overlay.api.hook(v);
overlay.reply(overlay.last(),{ok:true,duration:20,video:true,paused:true});
overlay.context.document.querySelectorAll=s=>s==='audio,video'?[v]:[];
const panel={nodeType:1,parentElement:null,css:{display:'block',visibility:'visible',opacity:'1',backgroundColor:'rgba(24,24,24,0.94)'},
 getBoundingClientRect:()=>({left:0,top:0,right:100,bottom:100,width:100,height:100})};
overlay.context.document.elementsFromPoint=()=>[panel,v];
function lastHoles(){return overlay.requests.filter(r=>r.fields[0]==='holes').slice(-1)[0].fields[4]}
overlay.api.syncAll();assert(lastHoles().startsWith('h'),'dark near-opaque menus must survive compositing');
overlay.context.document.documentElement={};overlay.api.installMediaObserver();
panel.css.display='none';overlay.observers[0].fn([]);overlay.api.syncAll();
assert.equal(lastHoles(),'','hidden overlays must clear on the next sync, without 400ms delay');
panel.css.display='block';panel.css.backgroundColor='rgba(0,0,0,0)';
panel.css.backgroundImage='url(transparent.png)';overlay.observers[0].fn([]);overlay.api.syncAll();
assert(lastHoles().startsWith('s'),'transparent images must not cut a black rectangle');
panel.css.backgroundColor='rgb(24,24,24)';overlay.observers[0].fn([]);overlay.api.syncAll();
assert(lastHoles().startsWith('h'),'opaque dark popups must survive compositing');
v.getBoundingClientRect=()=>({left:0,top:10,right:100,bottom:110,width:100,height:100});
overlay.api.syncAll();assert(lastHoles().includes('0,10,100,90'),'layout changes refresh overlay geometry immediately');
for(const type of ['mouseover','mouseout','click','focusin','focusout','transitionend','animationend','fullscreenchange']){
  panel.css.display='none';overlay.context.document.dispatchEvent({type});overlay.api.syncAll();
  assert.equal(lastHoles(),'',type+' clears overlays');
  panel.css.display='block';overlay.context.document.dispatchEvent({type});overlay.api.syncAll();
  assert(lastHoles().startsWith('h'),type+' restores overlays');
}
overlay.context.document.dispatchEvent({type:'mouseover'});overlay.api.syncAll();
panel.css.display='none';overlay.advance(60);overlay.api.syncAll();
assert.equal(lastHoles(),'','hover fade changes refresh while the pointer is stationary');
console.log('PASS: overlay classification, mutation, hover enter/leave/fade, focus, click, animation, fullscreen events, moving clip');
const cpp=fs.readFileSync(path.join(root,'render/nmb_composite.cpp'),'utf8');
assert(cpp.includes('bool paintVideo('));assert(cpp.includes('return lines != 0 && lines != GDI_ERROR;'));
assert(cpp.indexOf('if (!drawn)')<cpp.indexOf('if (browser->pageBits && browser->paintBits)'));
assert(cpp.includes('diagNote("noframe-or-blit-failed"'));
console.log('PASS: frame IDs, source generations, late callbacks, retry bounds/recovery, reinsertion, blob handoff, listeners, early observer, clip invalidation, native draw contract');
