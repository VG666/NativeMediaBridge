(function(){
  if(window.__nmbInstalled)return;window.__nmbInstalled=true;
  // 这段会在脚本上下文刚建立时就执行（见 onScriptContext），那时文档里可能一个节点都还没有。
  // 整体兜住异常并在出错时复位标记，保证后面还有机会重新注入。
  try{
    var seq=0,session='';
    // 文档/子 frame 独立的随机会话，不能只用各自从 1 开始的元素序号。
    try{var random=new Uint32Array(4);window.crypto.getRandomValues(random);session=Array.prototype.join.call(random,'-')}catch(e){}
    if(!session)session=Date.now().toString(36)+'-'+Math.random().toString(36).slice(2)+'-'+Math.random().toString(36).slice(2);
    var clipVersion=0;

    // ── Fullscreen API polyfill ──────────────────────────────────────────────
    // 离屏内核没有真实顶层窗口：实测 el.requestFullscreen() 返回的 Promise 必 reject(TypeError)，
    // document.fullscreenElement 恒为 null、fullscreenchange 从不触发。站点播放器(B站 bpx-player /
    // 腾讯 thumbplayer)和原生控件的全屏按钮因此点了毫无反应。
    // 用"页面内全屏"模拟：给目标元素的包装盒（组件壳）fixed 铺满视口、元素留在壳内，再按标准派发 fullscreenchange —— 播放器靠这个
    // 事件给自己加全屏 class（bpx-player-state-fullscreen 等）、重排控件；元素 rect 变大后桥的
    // syncAll 会按新 rect 上报，ffmpeg 帧自动铺满（与窗口缩放同一条已验证链路）。宿主窗口本身仍可
    // 由用户用系统最大化按钮放大，视觉上与真全屏一致。
    var fsElement=null,fsHost=null,fsPlaceholder=null;
    function fsDispatch(){
      try{document.dispatchEvent(new Event('fullscreenchange',{bubbles:true}))}catch(e){}
      try{document.dispatchEvent(new Event('webkitfullscreenchange',{bubbles:true}))}catch(e){}
    }
    function fsEnter(el){
      if(!el||fsElement===el)return;
      if(fsElement)fsExit();
      if(!attached(el))return;
      ensureBarStyle(document);
      var w=el.__nmbWrap||el;
      fsElement=el;fsHost=w;
      if(w!==document.documentElement&&w!==document.body){
        var r=w.getBoundingClientRect(),cs=getComputedStyle(w);
        fsPlaceholder=document.createElement('div');
        fsPlaceholder.setAttribute('data-nmb-fs-placeholder','1');
        fsPlaceholder.style.cssText='visibility:hidden!important;pointer-events:none!important;box-sizing:border-box!important;';
        var props=['display','margin-top','margin-right','margin-bottom','margin-left','vertical-align','flex','align-self','order','float'];
        for(var i=0;i<props.length;i++)fsPlaceholder.style.setProperty(props[i],cs.getPropertyValue(props[i]));
        fsPlaceholder.style.width=r.width+'px';fsPlaceholder.style.height=r.height+'px';
        w.parentNode.insertBefore(fsPlaceholder,w);
        document.documentElement.appendChild(w);
      }
      w.setAttribute('data-nmb-fs','1');
      w.setAttribute('data-nmb-fs-host','1');
      clipVersion++;
      if(document.body)document.documentElement.setAttribute('data-nmb-fs-lock','1');
      fsDispatch();
      fsSyncBars();
      // fixed 改尺寸不触发 resize，而播放器全屏后普遍监听 window resize 重算控件/弹幕区尺寸，手动补发。
      try{window.dispatchEvent(new Event('resize'))}catch(e){}
    }
    function fsExit(){
      var el=fsElement;
      if(!el)return;
      fsElement=null;
      var host=fsHost,placeholder=fsPlaceholder;
      fsHost=null;fsPlaceholder=null;
      if(host){
        host.removeAttribute('data-nmb-fs');host.removeAttribute('data-nmb-fs-host');
        if(placeholder&&placeholder.parentNode){
          if(attached(host))placeholder.parentNode.insertBefore(host,placeholder);
          placeholder.parentNode.removeChild(placeholder);
        }
      }
      clipVersion++;
      try{(el.__nmbWrap||el).removeAttribute('data-nmb-fs')}catch(e){}
      try{el.removeAttribute('data-nmb-fs')}catch(e){} // 旧版把属性打在元素上：清理兜底
      try{document.documentElement.removeAttribute('data-nmb-fs-lock')}catch(e){}
      fsDispatch();
      fsSyncBars();
      try{window.dispatchEvent(new Event('resize'))}catch(e){}
    }
    // 全屏时把其他媒体的桥控件条藏掉：它们的 z-index 比全屏层高（否则会被视频帧盖死），
    // 不藏就会漂在全屏画面上（如页面下方的音频条）。全屏元素自身/其内部媒体的条保留。
    // 这里同样只打属性，显隐在 CSS（[data-nmb-controls][data-nmb-off="1"]）。
    function fsSyncBars(){
      var el=fsElement;
      var bars=document.querySelectorAll('[data-nmb-controls]');
      for(var i=0;i<bars.length;i++){
        var bar=bars[i],owner=bar.__nmbOwner,keep=1,fsh;
        if(el&&owner)keep=(el===owner||(el.contains&&el.contains(owner)))?1:0;
        fsh=keep?'0':'1';
        if(bar.getAttribute('data-nmb-fshide')!==fsh)bar.setAttribute('data-nmb-fshide',fsh);
      }
    }
    try{
      Object.defineProperty(document,'fullscreenElement',{configurable:true,get:function(){return fsElement}});
      Object.defineProperty(document,'webkitFullscreenElement',{configurable:true,get:function(){return fsElement}});
      Object.defineProperty(document,'webkitCurrentFullScreenElement',{configurable:true,get:function(){return fsElement}});
      Object.defineProperty(document,'fullscreenEnabled',{configurable:true,get:function(){return true}});
      Object.defineProperty(document,'webkitFullscreenEnabled',{configurable:true,get:function(){return true}});
      Element.prototype.requestFullscreen=function(){fsEnter(this);return Promise.resolve()};
      Element.prototype.webkitRequestFullscreen=Element.prototype.requestFullscreen;
      Element.prototype.webkitRequestFullScreen=Element.prototype.requestFullscreen;
      // 退出必须自己补一个（defineProperty），原因不是"原生访问器没有 setter"——探针实测
      // （2026-09-18，_nmb_fs_rt.html 的 exitApi）内核**压根没有 document.exitFullscreen**：
      // 沿原型链 HTMLDocument/Document/Node/EventTarget 查全是 none，而同一层的
      // createElement/getElementById 都看得见，所以不是探针看不见、是真没实现。
      // 症状（最大化后属性一个都摘不掉、元素留在 fixed 满视口、条留在文档根上，像"退出按钮点了没反应"）
      // 的根因就是这个方法缺失：doc.exitFullscreen() 直接抛 TypeError。站点播放器的退出按钮同样调它。
      // 内核没有影子访问器，实例直接赋值其实也能生效；用 defineProperty 只是顺带覆盖
      // webkit 两个别名、并写成不可枚举的实例属性（下面 try/catch 里保留赋值兜底）。
      var fsExitFn=function(){fsExit();return Promise.resolve()};
      try{
        Object.defineProperty(document,'exitFullscreen',{configurable:true,writable:true,value:fsExitFn});
        Object.defineProperty(document,'webkitExitFullscreen',{configurable:true,writable:true,value:fsExitFn});
        Object.defineProperty(document,'webkitCancelFullScreen',{configurable:true,writable:true,value:fsExitFn});
      }catch(e){
        document.exitFullscreen=fsExitFn;
        document.webkitExitFullscreen=fsExitFn;
        document.webkitCancelFullScreen=fsExitFn;
      }
      // ESC：标准浏览器由系统拦截 ESC 退出全屏，页面随后才收到键；这里在捕获阶段自己退一次（幂等），
      // 不 stopPropagation —— 播放器自身的 ESC 监听照常运行，fsExit 的 fullscreenchange 会同步它的状态。
      document.addEventListener('keydown',function(e){
        if(fsElement&&(e.key==='Escape'||e.keyCode===27))fsExit();
      },true);
    }catch(e){}
    window.__nmbFsExit=fsExit; // 供诊断/原生侧在需要时强制退出
    // ── Fullscreen polyfill 结束 ──────────────────────────────────────────────

    // 每个媒体元素对应一份由原生解码器驱动的虚拟播放状态。
    function st(el){return el.__nmbState||(el.__nmbState={duration:0,position:0,paused:true,ended:false,ready:false,error:false,endedFired:false})}
    function fire(el,names){for(var i=0;i<names.length;i++){try{el.dispatchEvent(new Event(names[i]))}catch(e){}}}
    // 把真实播放状态写回元素的虚拟状态；fireEvents 只在位置/状态的例行回读时打开，
    // 否则每次播放暂停都会往页面里灌一串 timeupdate。
    function applyState(el,data,fireEvents){
      if(!data||typeof data.duration!=='number')return;
      var s=st(el);
      // 原样留一份应答：套件要读 audio/apos/aerr 这类字段，而 st(el) 只挑走它自己用得到的几个。
      // 页面侧脚本拿不到注入闭包里的 st()，但能通过 el.__nmbState 读到这份原样应答。
      s.raw=data;
      var before=s.duration;
      if(data.duration>0)s.duration=data.duration;
      if(typeof data.position==='number'&&!el.__nmbSeeking)s.position=data.position;
      s.paused=!!data.paused;s.ended=!!data.ended;s.error=!!data.error;
      // 读前缓冲水位（秒，领先播放多少）：测试用它判断源站够不够快。
      if(typeof data.buffered==='number')s.buffered=data.buffered;
      // 已缓冲到的绝对时间点（秒）：控件条画"已缓冲"那一段用它，见 syncBar。
      if(typeof data.bufferedUntil==='number')s.bufferedUntil=data.bufferedUntil;
      if(data.video||data.audio)s.ready=true;
      if(fireEvents&&s.ready){
        if(s.duration!==before&&s.duration>0)fire(el,['durationchange']);
        fire(el,['timeupdate']);
        if(s.ended&&!s.endedFired){s.endedFired=true;fire(el,['ended'])}
      }
      // 其余边沿事件（loadedmetadata/loadeddata/canplay/canplaythrough/progress/error）统一走 advance，
      // 它定义在下面那段字面量的拼接处——函数声明会提升，这里调用时它已经就位。
      // error 不看 fireEvents：解码中途失败时页面可能一次例行回读都没赶上，这条不能漏。
      if(fireEvents||s.error)advance(el,s,data);
    }
    // 一次桥请求。内核换成 mb 接口后，页面→native 只能走 window.mbQuery，
    // 它的回调是异步的，所以凡是"拿到结果之后才能做的事"都必须放进 done 回调里。
    // 字段用制表符拼接，顺序与 native 侧 splitFields 的取值一一对应。
    var querySeq=0;
    function send(el,op,value,done,fireEvents,geometry){
      var id=el.__nmbId||(el.__nmbId='nmb-'+session+'-'+(++seq));
      var generation=el.__nmbGeneration||0,source=sourceOf(el);
      var r=geometry||el.getBoundingClientRect();
      var src=String(el.__nmbServeUrl||el.currentSrc||el.src||el.getAttribute('src')||'').replace(/[\t\r\n]/g,'');
      // mb108 在 file:// 页面上可能把相对 currentSrc 返回成裁掉协议的路径；
      // 交给 FFmpeg 前统一解析成绝对 URL，否则本地视频会一直 open/busy。
      try{
        if(!/^[a-z][a-z0-9+.-]*:/i.test(src) || /^\/\/[A-Za-z]:\//.test(src)){
          var base=String(document.baseURI||location.href||'');
          // mb108 的 file 页面可能返回形如 //F:/... 的伪 URL；URL 构造器会把它
          // 当成网络协议相对地址，结果丢掉本地盘符。先补回 file:。
          if(/^\/\/[A-Za-z]:\//.test(base))base='file:'+base;
          if(base)src=new URL(src,base).href;
        }
      }catch(e){}
      var ua=String(navigator.userAgent||'').replace(/[\t\r\n]/g,'');
      // Referer 必须是**当前文档**的地址，而不是 document.referrer —— 后者是"上一页"，
      // 直接打开链接（测试脚本、粘贴地址）时恒为空串，等于一个 Referer 都不发。
      // 站点的防盗链正是按它判的：B站 CDN 同一条地址实测"无 Referer → 403 / 带 Referer → 200"，
      // 这也是"抓到了地址却打不开（403/404）"的直接原因。iframe 里的媒体报的是 iframe 自己的地址，
      // 与真实浏览器一致。fragment 要去掉，Referer 里不允许带 '#'。file:// 这类不上报，免得递一个 CDN 认不得的值。
      var ref='';try{if(/^https?:/i.test(location.protocol))ref=String(location.href||'').split('#')[0].replace(/[\t\r\n]/g,'')}catch(e){}
      var origin=String(location.origin||'').replace(/[\t\r\n]/g,'');
      var cookie='';try{cookie=String(document.cookie||'').replace(/[\t\r\n]/g,'')}catch(e){}
      var b=el.__nmbBarRect||[0,0,0,0];
      // clipRect＝元素在页面里真正可见的区域（placeBar 存的 clipBox 结果；null/缺省按全零
      // 上报，native 按"完全不可见、不画"处理）。字段 17-20，与 nmb_protocol.cpp 的
      // kFieldClipX..H 对齐；无元素通道 __nmbMedia 那条路径补的是同样的 4 个 '0'。
      var c=el.__nmbClipRect||[0,0,0,0];
      var req=[op,id,el.tagName.toLowerCase(),src,String(value==null?'':value),ua,ref,cookie,origin,
        String(r.left),String(r.top),String(r.width),String(r.height),
        String(b[0]),String(b[1]),String(b[2]),String(b[3]),
        String(c[0]),String(c[1]),String(c[2]),String(c[3])].join('\t');
      try{
        window.mbQuery(++querySeq,req,function(customMsg,response){
          if(op==='close'||id!==el.__nmbId||generation!==(el.__nmbGeneration||0)||source!==sourceOf(el))return;
          if(!attached(el)){unhook(el);return}
          var data;try{data=JSON.parse(String(response==null?'':response)||'{}')}catch(e){data={ok:false}}
          // native 那边已经不认这个元素了（条目被清掉或被释放，应答 missing:1）：退回"未接管"，
          // 让 tick 下一帧重新 open。否则元素会永久停在"接管了但什么都不动"——控件条画着、点了没反应，
          // 比不接管更难查。open 自己回的 missing 不走这里：那条路径另有 skip/退避处理。
          if(data.missing&&op!=='open'){forget(el);return}
          applyState(el,data,fireEvents);
          if(done)done(el,data);
        });
      }catch(e){
        // mbQuery 不可用（内核没提供、或注入过早）：当成打不开，把元素原样交还给内核和站点。
        if(done)done(el,{ok:false});
      }
    }
    // 页面侧不绑定具体媒体元素的独立通知通道（media_suite 自检用它把结果经 title op 回写宿主
    // 窗口标题，测试脚本再用 GetWindowText 读回）。send() 必须挂在一个元素上取 id/rect/src，
    // 这类通知没有元素；这里按与 send() 完全一致的字段顺序拼请求，保证 native splitFields 各
    // 字段索引不错位（value 仍在第 5 列），rect/barRect 全填 0。历史上页面就在调 __nmbMedia，
    // 桥侧一直缺这个全局名，调用被 try/catch 静默吞掉、标题永远停在初始页名。
    // 通用外部接管入口：页面/兼容层可声明"某元素用这个可服务 URL 走桥解码"，
    // 用于内核解不了的源（MSE/blob:）但有直链的场景（如站点的 playurl 合流）。
    // 不带任何站点语义；具体站点的直链解析由 compat_shim 等兼容层负责并调用本入口。
    window.__nmbServeVideo=function(el,url){
      try{
        if(!el||!url)return;
        el.__nmbServeUrl=String(url);
        hook(el);
      }catch(e){}
    };
    // 通用 MSE 接管：任意用 MediaSource 的站点（B站/YouTube/…），拦截 SourceBuffer.appendBuffer
    // 把音视频字节经桥喂 FFmpeg 解码。不写任何站点特定代码；音频分离待 P2，先传视频字节出画面。
    function base64FromUint8(u8){
      var s='';var CH=0x8000;
      for(var i=0;i<u8.length;i+=CH)s+=String.fromCharCode.apply(null,u8.subarray(i,i+CH));
      return btoa(s);
    }
    (function(){
      try{
        if(window.__nmbMseHooked)return;
        var RealMS=window.MediaSource;
        if(typeof RealMS!=='function')return;
        window.__nmbMseHooked=true;
        var blobToMs={};
        var realCreate=URL.createObjectURL;
        URL.createObjectURL=function(obj){
          var url=realCreate.apply(this,arguments);
          try{if(obj instanceof RealMS)blobToMs[url]=obj}catch(e){}
          return url;
        };
        function elForMs(ms){
          var vs=document.querySelectorAll('video');
          for(var i=0;i<vs.length;i++){if(blobToMs[String(vs[i].src)]===ms)return vs[i];}
          return vs[0]||null;
        }
        var realAdd=RealMS.prototype.addSourceBuffer;
        RealMS.prototype.addSourceBuffer=function(type){
          var sb=realAdd.apply(this,arguments);
          try{
            var ms=this;
            var realAppend=sb.appendBuffer;
            sb.appendBuffer=function(data){
              try{
                var el=elForMs(ms)||document.querySelector('video');
                if(el&&window.__nmbServeMse&&data){
                  var u8=(data instanceof Uint8Array)?data:(data&&data.buffer?new Uint8Array(data.buffer,data.byteOffset,data.byteLength):new Uint8Array(data));
                  window.__nmbServeMse(el,type,u8);
                }
              }catch(e){}
              return realAppend.apply(this,arguments);
            };
          }catch(e){}
          return sb;
        };
        window.__nmbLog.push('mse:hooked');
      }catch(e){}
    })();
    // 通用 MSE 入口：标记元素由桥接管绘制，并把音视频字节（base64 分块）经 op="mse" 喂桥。
    window.__nmbServeMse=function(el,mime,u8){
      try{
        if(!el||!u8)return;
        if(!el.__nmbMse){el.__nmbMse=true;el.__nmbServeUrl='__nmb_mse__';hook(el);}
        var id=el.__nmbId; if(!id)return;
        var CH=262144;
        for(var off=0;off<u8.length;off+=CH){
          var chunk=u8.subarray(off,Math.min(off+CH,u8.length));
          var b64=base64FromUint8(chunk);
          var req=['mse',id,'video','','',mime,'','','','','0','0','0','0','0','0','0','0','0','0','0','0',b64].join('\t');
          window.mbQuery(++querySeq,req,function(){});
        }
      }catch(e){}
    };
    window.__nmbMedia=function(op,id,kind,source,value,rx,ry,rw,rh){
      var ua='',ref='',cookie='',org='';
      try{ua=String(navigator.userAgent||'')}catch(e){}
      try{if(/^https?:/i.test(location.protocol))ref=String(location.href||'').split('#')[0]}catch(e){}
      try{cookie=String(document.cookie||'')}catch(e){}
      try{org=String(location.origin||'')}catch(e){}
      var req=[String(op),String(id==null?'':id),String(kind==null?'':kind),
        String(source==null?'':source),String(value==null?'':value),
        ua,ref,cookie,org,
        String(rx||0),String(ry||0),String(rw||0),String(rh||0),'0','0','0','0','0','0','0','0'].join('\t');
      try{window.mbQuery(++querySeq,req,function(){})}catch(e){}
    };
    // 内核自带控件读取的是内核内部的媒体状态，无法反映原生解码进度，
    // 所以对声明了 controls 的元素隐藏原生控件，改为绘制一套由本桥驱动的控件条。
    // 控件条的外观**不在这个文件里**：真源是 hook/css/*.css（按生成器 CSS_ORDER 表顺序拼成
    // 一份样式表），构建时由 _nmb_gen_hooks_inc.py 把拼接结果替换到下面那行 NMB_CSS。
    // 那行是生成器的占位行，会被整行重写，手改无效 —— 要改样式请改 hook/css 目录，
    // 然后 `python _nmb_gen_hooks_inc.py` 重新生成 inc 再编译（详见 hook/css/README.md）。
    var NMB_CSS='@@NMB_CSS@@';
    // 进度条要同时画"已播"和"已缓冲"两段，而 range 的轨道颜色只能靠伪元素给：
    // 一次性注入样式表，之后只改元素上的 --nmb-track（伪元素能继承自定义属性）。
    function ensureBarStyle(doc){
      if(doc.__nmbBarStyle)return;
      if(!doc.head&&!doc.documentElement)return;
      try{
        var style=doc.createElement('style');
        style.textContent=NMB_CSS;
        (doc.head||doc.documentElement).appendChild(style);
        doc.__nmbBarStyle=true;
      }catch(e){}
    }
    // ── 进度条三段色带（蓝=已播／浅=已缓冲／深=未读到）的喂料口 ──
    // 轨道样式仍是 base.css 的事（高度/圆角/还没喂数据时的回退色），这里只把每帧变的渐变串送上去，
    // 而且刻意**不用自定义属性**：miniblink 2023（Chromium 60）丢弃一切经 setProperty 写的自定义属性
    // —— 内联的丢，CSSOM 规则上的也丢（同页实测：算出来是 var() 的回退值）。于是原来的
    // seek.style.setProperty('--nmb-track',…) 从来没送到轨道上，整条只剩回退灰、两段色带一起消失
    //（用户报的"进度条没色带"就是这个）。写在样式表规则里的自定义属性它认，但"能否继承进伪元素"在
    // 本内核读不回来、无从证明；而"直接改这条伪元素规则的 background"能写、能读回、特异度也压得住
    // 基 CSS，所以改走这条。
    var trackSeq=0;
    // base 是滑块在样式表里的"身份选择器"：进度条 '.nmb-seek'、音量条 '.nmb-vol'。
    // 拼出来的选择器（[data-nmb-controls] + 身份 + [data-nmb-track-id]）必须比基 CSS 里那条
    // 同类伪元素规则特异度高，这样不靠"谁后进文档"定胜负。同一个函数服务两个滑块，
    // 免得音量条再抄一份插规则的逻辑。
    function trackRule(el,base){
      if(el.__nmbRule)return el.__nmbRule;
      var doc=el.ownerDocument,id='nmb-t'+(++trackSeq);
      el.__nmbTrackSel='[data-nmb-controls] '+(base||'.nmb-seek')+'[data-nmb-track-id="'+id+'"]';
      el.setAttribute('data-nmb-track-id',id);
      var sh=doc.createElement('style');
      (doc.head||doc.documentElement).appendChild(sh);
      el.__nmbTrackSheet=sh;
      try{
        sh.sheet.insertRule(el.__nmbTrackSel+'::-webkit-slider-runnable-track{background:none}',0);
        el.__nmbRule=sh.sheet.cssRules[0];
      }catch(e){}
      return el.__nmbRule;
    }
    function paintTrack(el,track,base){
      var rule=trackRule(el,base);
      if(!rule){   // 规则都插不进（没有 head / 没有 sheet）：退回整段样式表文本，总比整条没色强
        try{el.__nmbTrackSheet.textContent=el.__nmbTrackSel+'::-webkit-slider-runnable-track{background:'+track+'}'}catch(e){}
        return;
      }
      try{rule.style.background=track}catch(e){}
    }
    // 规则挂在 head 上，条被删不会把它带走 —— unhook 时必须自己收。
    // 一个条里现在有两个滑块（进度条 + 音量条）各挂一条规则，所以是"全收"不是"收第一条"。
    function removeTrackSheet(bar){
      if(!bar||!bar.querySelectorAll)return;
      var sks=bar.querySelectorAll('[data-nmb-track-id]');  // 静态 NodeList，边删边用安全
      for(var i=0;i<sks.length;i++){
        var sk=sks[i],sh=sk.__nmbTrackSheet;
        if(sh&&sh.parentNode)sh.parentNode.removeChild(sh);
        sk.__nmbTrackSheet=null;sk.__nmbRule=null;sk.__nmbTrackSel=null;sk.removeAttribute('data-nmb-track-id');
      }
    }
    function fmtTime(value){
      var t=Math.floor(value||0);
      if(!(t>=0))return '--:--';
      var m=Math.floor(t/60),s=t%60;
      return m+':'+(s<10?'0':'')+s;
    }
    // 元素真正看得见的那块矩形：元素盒子 ∩ 所有会裁剪它的祖先 ∩ 视口。
    // 为什么必须自己算：控件条挂在 documentElement 下，**根本不在元素的父容器里**，
    // 于是父容器的 overflow:hidden/auto 裁不到它。卡片里的视频只要有一部分露在卡片外，
    // 条就会整条画到卡片外面去——用户报的"超出界面控件显示"就是它。
    // 会裁剪的祖先只在第一次算（要 getComputedStyle，每帧做太贵），之后每帧只取它们的矩形。
    function clipBox(el,r){
      if(fsElement&&fsElement!==el&&!(fsElement.contains&&fsElement.contains(el)))return null;
      var visible;try{visible=getComputedStyle(el);if(visible.visibility==='hidden'||visible.visibility==='collapse'||visible.display==='none')return null}catch(e){}
      var box={l:r.left,t:r.top,r:r.right,b:r.bottom};
      var list=el.__nmbClippers;
      // DOM/style/class/resize 立即失效；CSSOM、伪类和动画无 mutation，最多缓存 250ms。
      var now=Date.now();
      if(!list||el.__nmbClipVersion!==clipVersion||el.__nmbClipParent!==el.parentElement||now-el.__nmbClipAt>=250){
        el.__nmbClipVersion=clipVersion;el.__nmbClipParent=el.parentElement;el.__nmbClipAt=now;
        list=[];
        for(var n=el.parentElement;n&&n.nodeType===1;n=n.parentElement){
          var st;try{st=getComputedStyle(n)}catch(e){continue}
          var ox=st.overflowX||st.overflow||'visible',oy=st.overflowY||st.overflow||'visible';
          if(st.visibility==='hidden'||st.display==='none'||st.opacity==='0')return null;
          if(ox!=='visible'||oy!=='visible'||st.overflow==='hidden')
            list.push({n:n,x:ox!=='visible'||st.overflow==='hidden',y:oy!=='visible'||st.overflow==='hidden',
                       bt:parseFloat(st.borderTopWidth)||0,br:parseFloat(st.borderRightWidth)||0,
                       bb:parseFloat(st.borderBottomWidth)||0,bl:parseFloat(st.borderLeftWidth)||0});
        }
        el.__nmbClippers=list;
      }
      for(var i=0;i<list.length;i++){
        var it=list[i],c=it.n.getBoundingClientRect();
        var sx=it.n.offsetWidth?(c.right-c.left)/it.n.offsetWidth:1;
        var sy=it.n.offsetHeight?(c.bottom-c.top)/it.n.offsetHeight:1;
        var l=c.left+(typeof it.n.clientLeft==='number'?it.n.clientLeft:it.bl)*sx;
        var t=c.top+(typeof it.n.clientTop==='number'?it.n.clientTop:it.bt)*sy;
        var right=typeof it.n.clientWidth==='number'?l+it.n.clientWidth*sx:c.right-it.br;
        var bottom=typeof it.n.clientHeight==='number'?t+it.n.clientHeight*sy:c.bottom-it.bb;
        if(it.x){box.l=Math.max(box.l,l);box.r=Math.min(box.r,right)}
        if(it.y){box.t=Math.max(box.t,t);box.b=Math.min(box.b,bottom)}
      }
      // 视口也是一道裁剪：滚出窗口的部分本来就看不见，条不必画到窗口外面去。
      if(box.l<0)box.l=0;
      if(box.t<0)box.t=0;
      if(box.r>window.innerWidth)box.r=window.innerWidth;
      if(box.b>window.innerHeight)box.b=window.innerHeight;
      if(box.r-box.l<=0||box.b-box.t<=0)return null;
      return box;
    }
    // 条的宿主＝包装盒（buildBar 的 wrapEl 建）。全屏铺满视口的是包装盒自己（fsEnter 把
    // data-nmb-fs 打在壳上，见 base.css），元素仍是壳的流内子元素 → inWrap 恒真，
    // 条整场留在壳里、absolute 贴壳底=贴视口底。fsbar 分支只剩一种来路：站点把元素挪出
    // 包装盒（absolute/fixed 等）且全屏还在——条改挂文档根兜底；非全屏的挪走没法用 CSS
    // 贴一个不包着它的元素——整条藏。
    function boxEq(node,r){
      var b=node.getBoundingClientRect();
      return Math.abs(b.left-r.left)<1&&Math.abs(b.top-r.top)<1&&Math.abs(b.width-r.width)<1&&Math.abs(b.height-r.height)<1;
    }
    function placeBar(el){
      var bar=el.__nmbBar;
      if(!bar)return;
      var r=el.getBoundingClientRect();
      // 控件条自身的内容高度。音频那条会被撑到元素盒子的高度（见下面的 fill），实测高度就不可靠了，
      // 所以只量第一次（buildBar 末尾调用时条已经进 DOM、是可见的）。
      if(!bar.__nmbNatH)bar.__nmbNatH=bar.offsetHeight||38;
      var nat=bar.__nmbNatH;
      var wrap=el.__nmbWrap;
      var fsEl=document.fullscreenElement;
      var inWrap=!!wrap&&boxEq(wrap,r);
      var selfFs=!!fsEl&&(fsEl===el||(fsEl.contains&&fsEl.contains(el)))||!!(el.hasAttribute&&el.hasAttribute('data-nmb-fs'));
      var fsbar=!inWrap&&selfFs;
      // 条在包装盒与文档根之间搬（仅全屏进出会触发）：位置两侧都是纯 CSS，搬的只是父子关系。
      if(bar.__nmbFsbar!==fsbar){
        bar.__nmbFsbar=fsbar;
        if(fsbar){
          bar.setAttribute('data-nmb-fsbar','1');
          if(bar.parentNode!==document.documentElement){try{document.documentElement.appendChild(bar)}catch(e){}}
        }else{
          bar.removeAttribute('data-nmb-fsbar');
          if(wrap&&bar.parentNode!==wrap){try{wrap.appendChild(bar)}catch(e){}}
        }
      }
      // 音频没有画面，控件条就是这个组件本身。原生 <audio controls> 的占位是 54px，而控件条内容是 38px，
      // 按视频那套"压在画面底部"贴着盒子底边画，盒子上方就空出一条 16px 的死区——看起来就是
      // "一条控件条浮在一个空盒子里、样式不对"。所以盒子完整露在视口里时让条撑满盒子：
      // 撑满关系在样式表的 [data-nmb-fill="1"]（top:0+bottom:0），这里只打属性。
      var fill=el.tagName==='AUDIO'&&inWrap&&r.height>nat+1&&r.top>=0&&r.bottom<=window.innerHeight;
      if(bar.__nmbFill!==fill){bar.__nmbFill=fill;if(fill)bar.setAttribute('data-nmb-fill','1');else bar.removeAttribute('data-nmb-fill')}
      // 全屏中的元素是另一个（或另一个容器）：本元素的条必须藏掉，否则会漂在全屏画面上。
      var fsHidden=!!fsEl&&fsEl!==el&&!(fsEl.contains&&fsEl.contains(el));
      // 只有"元素整体离开了视口"（或盒子没尺寸，或元素跳出了包装盒又不在全屏）才藏起来。
      // 过去按"视口里露出来的那一段"定位（贴可见部分的底边），条就会随滚动在元素内部滑来滑去，
      // 元素下半截滚出窗口时还会变成一条粘在窗口底边的幽灵进度条——用户报的就是这个"到处动"。
      // 现在条由 CSS 锁在包装盒（=元素盒子）里，剩下多少、露不露得出来都交给浏览器自己裁。
      var off=0;
      if(fsHidden||(!inWrap&&!fsbar)||r.width<=0||r.bottom<=0||r.top>=window.innerHeight||r.right<=0||r.left>=window.innerWidth)off=1;
      // 显隐口径（2026-09-18 ui10 修）：条锁在包装盒里、跟着元素一起被视口/容器**真实裁剪**
      //（包装盒方案前条贴"可见部分"的底边，部分越界时会孤零零悬在画面上——那时"一裁就藏"
      // 是对的）。现在部分越界时条与画面同位置同裁剪，等于浏览器原生控件的行为：
      // 只有**整个元素都看不见**（完全出界，或被 overflow 祖先完全吃掉 = clipBox 返回 null）
      // 才收掉整条。部分裁剪还收条，就是用户报的"视频超过顶部条就没了"。
      var box=off?null:clipBox(el,r);
      if(!box)off=1;
      // 元素在页面里真正可见的区域**不由这里存**：placeBar 在拆包/无条时根本不进来
      //（236 行 if(!bar)return），把 clip 绑在条上会让拆包元素的 clip 永远缺省、
      // native 按"完全不可见"把画面也砍掉。权威写入点在 syncAll（算 ckey 的地方），
      // 每次几何变化都会覆盖，有条无条都跑。
      // 16px 以下连一个播放键都塞不下，画出来只会是一坨挤坏的残条，不如不画。
      if(r.height<16)off=1;
      // 显隐只打属性，怎么显示在样式表里（[data-nmb-controls][data-nmb-off="1"]）。
      if(off){
        if(el.__nmbOff!==1){el.__nmbOff=1;bar.setAttribute('data-nmb-off','1')}
        el.__nmbBarRect=null;
        return;
      }
      if(el.__nmbOff!==0){el.__nmbOff=0;bar.removeAttribute('data-nmb-off')}
      // 条的几何由 CSS 从包装盒算出，这里只**读**不写：分档看条实测宽，挖洞矩形照实量条自己——
      // 页面侧条画在哪、上报给 native 的就是哪，条与洞天然是同一份数字。
      var bb=bar.getBoundingClientRect();
      if(bb.width<=0||bb.height<=0){el.__nmbBarRect=null;return}
      // 条比控件排得下的宽度还窄时，按宽度分档收掉非必要控件（只保留播放 + 进度 + 时间）。
      // 分档只收内部控件，不动条自身的几何（条宽由包装盒决定），所以这里怎么打属性都不会反回来改坐标。
      var tier=bb.width<240?'tiny':(bb.width<300?'narrow':'');
      if(bar.__nmbTier!==tier){
        bar.__nmbTier=tier;
        if(tier){bar.setAttribute('data-nmb-'+tier,'1')}
        else{bar.removeAttribute('data-nmb-narrow');bar.removeAttribute('data-nmb-tiny')}
      }
      el.__nmbBarRect=[Math.round(bb.left),Math.round(bb.top),Math.round(bb.width),Math.round(bb.height)];
    }
    // 条要"控制在元素内部"，但 video/audio 是替换元素（子元素不渲染），条塞不进元素自己。
    // 由桥建一个包装盒放在元素原布局位上（insertBefore 到元素原位置）、把元素搬进去：
    // 条成为包装盒的子元素，位置交给 CSS 相对布局（包装盒 relative + 条 absolute inset 贴底），
    // 滚动/缩放/布局变化全由浏览器处理，脚本不写任何坐标。
    // 包装会改写 CSS 匹配上下文（站点的 >video 子选择器、flex 拉伸、百分比高度循环都可能失配）：
    // 包装前后各量一次元素盒子，布局被包装动作改写就当场拆包回退——元素原样交还、条不建
    // （native 画面照常，只是没有控件条）。布局正确性优先于有控件。
    function wrapEl(el){
      var doc=el.ownerDocument;
      var r0=el.getBoundingClientRect();
      var wrap=doc.createElement('div');
      wrap.setAttribute('data-nmb-wrap','1');
      // 包装盒要"顶替"元素在站点语境里的盒外表现，否则就是样式失控：行内媒体
      // （video 默认 inline）被块级包装盒挤成独占一行；margin 不搬走，站点给元素的
      // margin 变成包装盒内的双重间距（条贴盒底就不再贴元素底）。
      // 搬完后元素自身的 margin/vertical-align 由样式表清零（margin:0!important），
      // 清不掉（站点 !important）就会被下面的逐像素保险抓到、当场拆包。
      // display 不照抄：包装盒的行内/块级身份由 data-nmb-inline 属性交给样式表定
      // （见 base.css），脚本不写 display —— 布局值归样式表，这是用户明确口径。
      // 样式表那边只能用 position:relative + 条 absolute/inset 贴盒底那套：
      // 本内核没有 CSS Grid，display:grid 会被静默丢弃、条直接掉到元素下面去。
      try{
        var cs=doc.defaultView?doc.defaultView.getComputedStyle(el):null;
        if(cs){
          var d=cs.display;
          if(d&&d.indexOf('inline')===0)wrap.setAttribute('data-nmb-inline','1');
          var mv=['margin-top','margin-right','margin-bottom','margin-left','vertical-align'];
          for(var i=0;i<mv.length;i++){var v=cs.getPropertyValue(mv[i]);if(v)wrap.style.setProperty(mv[i],v)}
        }
      }catch(e){}
      try{el.parentNode.insertBefore(wrap,el);wrap.appendChild(el)}catch(e){return null}
      var r1=el.getBoundingClientRect();
      if(Math.abs(r1.left-r0.left)>=1||Math.abs(r1.top-r0.top)>=1||
         Math.abs(r1.width-r0.width)>=1||Math.abs(r1.height-r0.height)>=1){
        try{if(wrap.parentNode){wrap.parentNode.insertBefore(el,wrap);wrap.parentNode.removeChild(wrap)}}catch(e){}
        return null;
      }
      return wrap;
    }
    // 拆包装还原（unhook 用）：元素放回原布局位、包装盒删除，站点 DOM 恢复接管前的样子。
    function unwrapEl(el){
      var wrap=el.__nmbWrap;
      if(!wrap)return;
      el.__nmbWrap=null;
      try{
        if(wrap.parentNode){if(el.parentNode===wrap)wrap.parentNode.insertBefore(el,wrap);wrap.parentNode.removeChild(wrap)}
      }catch(e){}
    }
    function buildBar(el){
      if(el.__nmbBar)return el.__nmbBar;
      var doc=el.ownerDocument;
      var bar=doc.createElement('div');
      bar.setAttribute('data-nmb-controls','1');
      // 条默认就建在桥的包装盒里（见末尾 wrap.appendChild(bar)），由 base.css 的
      // absolute+inset 相对布局贴盒底——普通态一个坐标都不写、不脱离文档流。
      // 全屏时铺满视口的是包装盒（[data-nmb-wrap][data-nmb-fs="1"]），条整场留在壳里自动贴视口底；
      // 只有站点把元素挪出壳的异常才走 placeBar 的 fsbar 兜底（见 base.css 与 placeBar）。
      // 反向指回所属媒体：全屏 polyfill 隐藏"全屏元素之外"的控件条时靠它认亲
      // （全屏的是容器时，保留容器内媒体的条；其他 video/audio 的条藏到全屏层下，不漂在画面上）。
      bar.__nmbOwner=el;
      // 不插到元素旁边：原生帧画在页面之上，插在旁边会被画面盖住，位置靠合成时的挖洞露出来。
      // 视频条不要圆角：它压在画面底边上，圆角会在两侧露出画面的尖角，看着像"贴上去的一块"。
      // 音频条相反——它本身就是那个组件，四角都圆（见下面的 data-nmb-audio 分支）。
      // 外观全部在 base.css（选择器带 [data-nmb-controls] 前缀）。这里不写样式字符串，
      // 只打 data-* 属性（控件身份、宽度分档、音频条），样式表靠这些属性选中对应控件。
      // overflow:hidden 那类保险也在样式表里 —— 进度条的 min-width 撑着不让收缩，条一窄，
      // 后面的控件就整块溢到条外面压在页面背景上（音频元素普遍窄，所以这个毛病只显在音频上）。
      ensureBarStyle(doc);
      var audio=el.tagName==='AUDIO';
      if(audio)bar.setAttribute('data-nmb-audio','1');
      var btn=doc.createElement('button');btn.type='button';btn.setAttribute('aria-label','播放或暂停');btn.setAttribute('data-nmb-play','1');
      // 记号是 btn 里唯一那个 <i>：形状由 .nmb-mark-play/.nmb-mark-pause 决定，syncBar 只切 class。
      var mark=doc.createElement('i');mark.className='nmb-mark nmb-mark-play';btn.appendChild(mark);btn.__nmbMark=mark;
      var cur=doc.createElement('span');
      var seek=doc.createElement('input');seek.type='range';seek.min='0';seek.max='1000';seek.step='1';seek.value='0';seek.setAttribute('aria-label','进度');seek.className='nmb-seek';
      var total=doc.createElement('span');
      // className 是"身份选择器"：样式靠它选中（.nmb-vol），paintTrack 也靠它拼出那条
      // 特异度更高的伪元素规则来喂"拖过/没拖过"的分界线，别去掉。
      var vol=doc.createElement('input');vol.type='range';vol.min='0';vol.max='100';vol.step='1';vol.value='100';vol.setAttribute('aria-label','音量');vol.className='nmb-vol';
      // 宽度分档（placeBar 的 tier 写 data-nmb-narrow/data-nmb-tiny）与各控件尺寸都靠这些标记认控件。
      cur.setAttribute('data-nmb-cur','1');seek.setAttribute('data-nmb-seek','1');total.setAttribute('data-nmb-total','1');vol.setAttribute('data-nmb-vol','1');
      var fs=null;
      if(!audio){fs=doc.createElement('button');fs.type='button';fs.textContent='⛶';fs.setAttribute('aria-label','全屏');fs.setAttribute('data-nmb-fs','1')}
      btn.onclick=function(){if(st(el).paused){el.play()}else{el.pause()}};
      // 音频不建全屏按钮：没有画面，全屏没有意义。注意按钮上的 data-nmb-fs 只是"这是全屏按钮"的
      // 记号，与全屏**状态**属性同名——状态选择器必须写成 [data-nmb-wrap][data-nmb-fs="1"]（见
      // base.css），裸 [data-nmb-fs="1"] 会把这个按钮拉成 100vw×100vh。
      if(fs)fs.onclick=function(){try{
        // 在不在全屏看 fsElement 本尊，别看元素身上的属性——状态属性打在包装盒上（fsEnter），
        // 元素自己永远不带。退出统一走自家 fsExit：内核没有 document.exitFullscreen（见顶部注释），
        // 调它等于什么都没做。
        if(fsElement===el){fsExit()}
        else if(el.requestFullscreen){el.requestFullscreen()}
        else fsEnter(el);
      }catch(e){}};
      // 拖动进度条：本地值先落地（滑块立刻跟手），最终位置交给原生解码器。
      // 过去有两个坑，合起来就是用户报的"拖不动"：
      //  ① __busy 靠 600ms 定时器自动解除：拖动稍慢一点就在中途被解除，syncBar 立刻拿 native 的旧
      //     position 回写 value，滑块被硬拽回原处——看起来就是拖不动、一松手就弹回去；
      //  ② 只有 s.duration>0 才设 currentTime：时长还没报回来（见 openAudio 的时长回落）时，
      //     整个拖动都是空转——滑块跟着鼠标走，松手什么也不发生。
      // 现在：拖动期间一律不回写；松手后落点先写给本地，等 native 把位置接上（或超时）再交还控制权。
      function dragTo(v){
        seek.__nmbLocal=v;
        var s=st(el),d=s.duration;
        if(d>0){var t=d*v/1000;if(t<0)t=0;if(t>d)t=d;el.currentTime=t}
      }
      function dragEnd(){
        if(seek.__timer)clearTimeout(seek.__timer);
        // 250ms 给 native 的 seek 落点回到网页侧；这段时间内同步逻辑不许碰滑块。
        seek.__timer=setTimeout(function(){seek.__busy=false},250);
      }
      seek.addEventListener('mousedown',function(){seek.__busy=true});
      // 内核的滑块不一定派发 mouse 系事件（部分版本只走 pointer）：两套都挂，谁先到算谁。
      seek.addEventListener('pointerdown',function(){seek.__busy=true});
      seek.addEventListener('input',function(){seek.__busy=true;dragTo(Number(seek.value))});
      seek.addEventListener('change',function(){dragTo(Number(seek.value))});
      seek.addEventListener('mouseup',dragEnd);
      seek.addEventListener('pointerup',dragEnd);
      // 鼠标在条外松开（拖出控件条再松手）时元素收不到 mouseup，靠这条兜底解除，否则滑块会永久锁死。
      seek.addEventListener('mouseleave',function(){if(seek.__busy)dragEnd()});
      vol.addEventListener('input',function(){el.volume=Number(vol.value)/100});
      bar.appendChild(btn);bar.appendChild(cur);bar.appendChild(seek);bar.appendChild(total);bar.appendChild(vol);if(fs)bar.appendChild(fs);
      // 条与元素一起进桥的包装盒（wrapEl）：条由 CSS 相对布局贴盒底，一个坐标都不写。
      // 包装被布局保险退回（wrapEl 返回 null）就不要条——native 画面照常，宁缺毋滥。
      var wrap=el.__nmbWrap||wrapEl(el);
      if(!wrap){el.__nmbWantsBar=false;return null}
      el.__nmbWrap=wrap;
      wrap.appendChild(bar);
      el.__nmbBar=bar;
      el.__nmbWantsBar=true;
      placeBar(el);
      return bar;
    }
    function syncBar(el){
      var bar=el.__nmbBar;
      if(!bar)return;
      // 这里原来每帧先调一次 placeBar(el)：syncAll 明明只在 rect 变了（__nmbRect）才摆条，
      // 这一句又把"每帧无条件重写 left/top/width/height/display"补回来了，等于白算一次样式、白失效一次布局。
      // 摆条只该发生在两处：buildBar（建条时一次）和 syncAll（rect 或视口变了才一次）。这里不再碰位置。
      var s=st(el);
      var btn=bar.firstChild;
      var cur=btn.nextSibling,seek=cur.nextSibling,total=seek.nextSibling,vol=total.nextSibling;
      var paused=!!s.paused;
      // 只在状态真的翻转时才改记号：syncAll 是每帧跑的，每帧写一次样式纯属白烧 CPU。
      if(btn.__nmbPaused!==paused){
        btn.__nmbPaused=paused;
        btn.__nmbMark.className=paused?'nmb-mark nmb-mark-play':'nmb-mark nmb-mark-pause';
        var label=paused?'播放':'暂停';btn.setAttribute('aria-label',label);btn.title=label;
      }
      // 拖动中（__busy）或 native 还在定位（__nmbSeeking）都绝不回写：回写就是用旧位置把滑块拽回去。
      if(!seek.__busy&&!el.__nmbSeeking&&s.duration>0){
        var v=Math.round(s.position/s.duration*1000);
        if(v<0)v=0;if(v>1000)v=1000;
        v=String(v);
        // 和下面 track/tip 同一个口径：值没变就不写。position 是 200ms 上报一次的，
        // 每帧回写相同的值只会让浏览器多算一次 range 的内部布局。
        if(seek.value!==v)seek.value=v;
      }
      // 进度条上同时画出"已播"和"已缓冲"两段：蓝=已播，浅色=已经读到但还没播，深色=还没读到。
      // 已缓冲段用原生上报的"已读到的最远时间点"（绝对秒 bufferedUntil），而不是"播放位置+相对水位"：
      // 后者在往回拖进度条时会跟着播放位置一起往回缩——那段数据本来早就读过了，色带必须停在原地。
      // 已播段同理：拖动期间要用滑块自身的值，否则蓝带停在旧位置，看着仍然是拖不动。
      var shown=(seek.__busy||el.__nmbSeeking)?Number(seek.value)/10:s.position;
      var pct=s.duration>0?Math.max(0,Math.min(100,shown/s.duration*100)):0;
      var until=typeof s.bufferedUntil==='number'?s.bufferedUntil:0;
      var buf=s.duration>0?Math.max(pct,Math.min(100,until/s.duration*100)):pct;
      // 取两位小数：不round的话每帧浮点尾数都不同，样式字符串一直在变，下面那句"没变就不写"就形同虚设。
      pct=Math.round(pct*100)/100;buf=Math.round(buf*100)/100;
      // 断点一律"每段两端各写一个百分比"，绝不用 "#1a73e8 0 30%" 这种"颜色 起 止"的双位置写法
      //（CSS Images 4，要 Chrome 71+）。内核切到 miniblink 2023 后 UA 是 Chromium 60，双位置整条
      // 渐变会被判非法值 → background 计算值变成 none → 轨道没有任何颜色，已播/已缓冲两段一起消失。
      //（这是"进度条没色带"的原因之一，另一个是自定义属性送不到轨道上，见 paintTrack；两个都补上
      //  才画得出来。）老写法 Chrome 10+ 都认，新旧内核通吃。
      var track='linear-gradient(90deg,#1a73e8 0%,#1a73e8 '+pct+'%,rgba(255,255,255,.5) '+pct+'%,rgba(255,255,255,.5) '+buf+'%,rgba(255,255,255,.16) '+buf+'%,rgba(255,255,255,.16) 100%)';
      // 值没变就不写样式，避免每帧都触发一次重算。
      if(seek.__nmbTrack!==track){seek.__nmbTrack=track;paintTrack(seek,track)}
      var tip=until>0?('已缓冲到 '+fmtTime(s.duration>0?Math.min(until,s.duration):until)):'进度';
      if(seek.__nmbTip!==tip){seek.__nmbTip=tip;seek.title=tip}
      // 已播时间也跟手：拖动时显示落点时间，和滑块、蓝带三者一致。
      // 和上面同一条规矩：秒数一秒才动一次、总时长与音量基本不变，所以先比再写。
      var curTxt=fmtTime(shown);
      if(cur.__nmbTxt!==curTxt){cur.__nmbTxt=curTxt;cur.textContent=curTxt}
      var totTxt=s.duration>0?fmtTime(s.duration):'--:--';
      if(total.__nmbTxt!==totTxt){total.__nmbTxt=totTxt;total.textContent=totTxt}
      var volN=el.muted?0:Math.round((el.volume==null?1:el.volume)*100);
      var volV=String(volN);
      if(vol.value!==volV)vol.value=volV;
      // 音量条也画两段：**拖过的一段实白、没拖过的一段灰**（用户口径）。分界只能来自每帧的值，
      // 所以和进度条一样喂进那条伪元素规则——写元素自己的 background 会被伪元素的轨道色盖住。
      // 断点写法照旧：每段两端各写一个百分比，绝不用"颜色 起 止"的双位置语法（CSS Images 4，
      // 本内核 Chromium 60 判非法，整条渐变会失效变成 none）。
      var vtrack='linear-gradient(90deg,#fff 0%,#fff '+volN+'%,rgba(255,255,255,.28) '+volN+'%,rgba(255,255,255,.28) 100%)';
      if(vol.__nmbTrack!==vtrack){vol.__nmbTrack=vtrack;paintTrack(vol,vtrack,'.nmb-vol')}
    }
    // 元素被从页面里移除后，挂在 body 上的控件条和原生视频画面都得一起清掉，否则会残留在页面上。
    var hookedEls=[];
    function attached(el){try{return el.isConnected===undefined?document.documentElement.contains(el):el.isConnected}catch(e){return true}}
    // 站点用 MSE（blob:）或 data: 自己喂数据的元素我们解不了，也绝不能碰：
    // 一旦改写它的属性，站点自己的播放链路读到的就是假状态。
    function sourceOf(el){return el.currentSrc||el.src||el.getAttribute('src')||''}
    function sealed(el){return /^(blob:|mediasource:|data:)/i.test(sourceOf(el))}
    function servable(el){var s=sourceOf(el);return !!s&&!/^(blob:|mediasource:|data:)/i.test(s)}
    function dropDetached(){
      for(var k=hookedEls.length-1;k>=0;k--){
        var el=hookedEls[k];
        if(attached(el))continue;
        unhook(el);
      }
    }
    // <audio controls> 一关掉原生控件就没有内在尺寸了，而 <audio> 默认是行内元素：
    // height/min-height 对行内盒子不生效，元素会当场塌成 0×0。控件条挂在元素上，
    // 高度为 0 就被当成"露不出来"整条隐藏，页面上只剩一块空白——用户报的"音频组件一片空白"就是它。
    // 视频有画面撑着不会塌，所以只需要给音频补：把隐藏原生控件之前的占位尺寸钉成内联样式（外观不变），
    // 行内盒子顺便转成能设尺寸的行内块。站点自己设了 display:none 的元素不碰——钉住尺寸会把它从隐藏变可见。
    function pinBox(el){
      var r=el.getBoundingClientRect();
      var d='';try{d=getComputedStyle(el).display}catch(e){}
      if(d==='none')return false;
      el.__nmbPinned={display:el.style.display,width:el.style.width,minHeight:el.style.minHeight};
      if(d==='inline'||d==='inline-block'){
        if(r.width>0)el.style.width=Math.round(r.width)+'px';
        el.style.display='inline-block';
      }
      if(r.height>0)el.style.minHeight=Math.round(r.height)+'px';
      return r.width>0;
    }
    // 原生确认能打开这个地址：从这里开始才改写元素属性、隐藏原生控件并接管事件。
    function adopt(el){
      if(el.__nmbHooked)return;
      el.__nmbHooked=true;hookedEls.push(el);el.setAttribute('data-native-media','1');
      el.__nmbHadControls=!!(el.hasAttribute&&el.hasAttribute('controls'));
      var boxed=el.tagName==='AUDIO'?pinBox(el):false;
      if(el.__nmbHadControls){
        // mb108 的 UA 样式 `audio:not([controls]){display:none!important}` 连行内 !important 都赢不了
        // （实测 F:\ffbuild\p_truth2.log：setProperty('display','inline-block','important') 后 computed 仍 none）。
        // audio 一摘 controls 属性就被内核强制隐藏、盒子塌成 0x0，控件条跟着消失——"音频组件样式完全没显示"
        // 的根因。所以 audio 保留 controls 属性：原生小部件留在盒子底下，由本模块的不透明控件条盖住
        // （placeBar 的 fill 模式本来就是按"撑满 54px 原生盒"设计的）。video 没有这条隐藏规则，照旧摘掉。
        if(el.tagName!=='AUDIO'){try{el.controls=false}catch(e){}}
        // audio 摘不了 controls，就把它连同里面那套原生小部件一起藏掉：
        // 盒子尺寸已由 pinBox 钉住、display 也转成了 inline-block，改 opacity 只影响可见性、
        // 不动布局也不动命中判定（elementsFromPoint 照样认它），所以这条比 visibility 稳。
        // audio 元素除影子控件外本身没有画面，藏掉它没有任何信息损失——它长什么样由控件条决定。
        // video 暂不整体隐藏：它的黑底在未接管/暂无画面时还顶着位置，"video 默认样式"先靠
        // 样式表里那条 ::-webkit-media-controls 去（对 video 一样生效，且没有副作用）。
        if(el.tagName==='AUDIO'){
          el.__nmbHidden=el.style.opacity;
          el.style.setProperty('opacity','0','important');
        }
        el.__nmbWantsBar=true}
      installOverrides(el);
      if(!el.__nmbListeners){
        el.__nmbListeners=true;
        el.addEventListener('play',function(){if(el.__nmbHooked){st(el).paused=false;send(el,'play')}},true);
        el.addEventListener('pause',function(){if(el.__nmbHooked){st(el).paused=true;send(el,'pause')}},true);
        el.addEventListener('volumechange',function(){if(el.__nmbHooked)send(el,'volume',el.muted?0:el.volume)},true);
        el.addEventListener('loadedmetadata',function(){hook(el)},true);
        el.addEventListener('canplay',function(){hook(el)},true);
      }
      // 音频没有画面：只要站点给了可见的盒子（pinBox 认过宽度），即便它本来不带原生控件，
      // 那块盒子在页面上也只是个空洞，照样给它一条控件条。
      if(el.__nmbWantsBar||boxed)buildBar(el);
      // 兑现接管之前页面就表达过的播放意图（采样见 hook 与 sniff）：走接管后的 el.play()，
      // 它会 fire play → 上面那个捕获监听发 play op。少了这一步，页面在接管前调过的 play() 永远
      // 到不了原生解码器：条目建好了、矩形也对，只是 native 侧 paused 恒 true —— 视频区只剩页面
      // 底色、也听不到声音，而页面侧 paused 已是 false、play() 早已 resolve，站点以为一直在播。
      if(el.__nmbWantPlay){el.__nmbWantPlay=false;try{el.play()}catch(e){}}
    }
    // 接管一个元素：先问原生解码器能不能打开，能打开才改写它的属性、隐藏原生控件并接管事件；
    // 打不开的元素一个属性都不改，原样交还给内核和站点。
    // 应答是异步回来的，所以这里必须用 __nmbOpening 挡住同一元素上并发飞出去的多次 open。
    function retryOpen(el){
      el.__nmbOpening=false;
      if(el.__nmbTries>=12){
        if(el.__nmbId)send(el,'close');
        el.__nmbGeneration=(el.__nmbGeneration||0)+1;el.__nmbId=null;
        el.__nmbSkip=true;el.__nmbRetryAt=0;
        fire(el,['error']);return;
      }
      el.__nmbRetryAt=Date.now()+Math.min(200*Math.pow(2,el.__nmbTries-1),8000);
    }
    // Explicit calls reset exhaustion; ticks and native events remain bounded.
    function recoverSkipped(el){
      if(!el.__nmbSkip||!servable(el)||sealed(el))return;
      unhook(el);el.__nmbLost=0;
    }
    try{
      ['play','load'].forEach(function(name){
        var original=HTMLMediaElement.prototype[name];
        if(typeof original!=='function')return;
        HTMLMediaElement.prototype[name]=function(){
          if(this.__nmbSkip){
            recoverSkipped(this);
            if(name==='play'){this.__nmbWantPlay=true;this.__nmbEverWanted=true}
            hook(this);
          }
          return original.apply(this,arguments);
        };
      });
    }catch(e){}
    function hook(el){
      var source=sourceOf(el);
      if(el.__nmbSrc!==source){
        if(el.__nmbSrc!==undefined)unhook(el);
        el.__nmbSrc=source;el.__nmbLost=0;
      }
      if(!attached(el))return;
      if(el.__nmbHooked||el.__nmbSkip||el.__nmbSealed)return;
      if(sealed(el)){
        // 内核解不了的源（MSE/blob:）：外部经 __nmbServeVideo 提供可服务直链，或通用 MSE 接管
        // （__nmbServeMse 标记）已就位，则接管画面覆盖原元素；否则 sealed 跳过（不碰原生 MSE 链路）。
        if(!(el.__nmbServeUrl&&/^blob:/i.test(sourceOf(el))) && !el.__nmbMse){el.__nmbSealed=true;return}
      }
      if(!servable(el)&&!el.__nmbServeUrl)return;
      var now=Date.now();
      if(el.__nmbRetryAt&&now<el.__nmbRetryAt)return;
      if(el.__nmbOpening)return;
      el.__nmbOpening=true;
      // open 的应答偶尔会丢（native 忙、内核吞回调）：__nmbOpening 不复位的话元素永远停在
      // "接管中"，控件条永远建不出来。超时兜底把标记放开，下一轮 tick 重新 open；
      // 正常应答会先一步把计时器撤掉（见回调第一行）。
      if(el.__nmbOpenGuard)clearTimeout(el.__nmbOpenGuard);
      el.__nmbTries=(el.__nmbTries||0)+1;
      var generation=el.__nmbGeneration=(el.__nmbGeneration||0)+1;
      el.__nmbOpenGuard=setTimeout(function(){
        if(generation!==el.__nmbGeneration)return;
        el.__nmbOpenGuard=null;el.__nmbGeneration++;retryOpen(el);
      },6000);
      // 接管前采样页面意图：此刻元素还是内核原生实现，读到的 el.paused/el.autoplay 是页面真正
      // 表达过的状态；adopt 之后 paused 变成桥的替身，就再也问不出来源意图了。
      // 只采一次（元素上后续还会反复进这里）。事件证据（见 sniff）比这次采样更晚、更准，别覆盖它。
      if(el.__nmbWantPlay===undefined)
      el.__nmbWantPlay=(el.paused===false)||(el.autoplay===true)||!!(el.hasAttribute&&el.hasAttribute('autoplay'));
      // 与 error 看门狗的判据保持一致：采样认定"本来就要播"的元素（autoplay/已在播）锁存意图，
      // 不能被 1.5 秒的探测元素看门狗误放手。
      if(el.__nmbWantPlay)el.__nmbEverWanted=true;
      send(el,'open',null,function(el,data){
        if(el.__nmbOpenGuard){clearTimeout(el.__nmbOpenGuard);el.__nmbOpenGuard=null}
        el.__nmbOpening=false;
        if(el.__nmbHooked||el.__nmbSkip||el.__nmbSealed)return;
        if(data&&data.ok){el.__nmbTries=0;el.__nmbRetryAt=0;adopt(el);return}
        // 失败条目必须关闭，下一次才能真的重开；busy 仅轮询同一 native open。
        if(!data||!data.busy){send(el,'close');el.__nmbId=null;el.__nmbGeneration++}
        retryOpen(el);
      });
    }
    // 把元素还给内核和站点（元素被移出 DOM，或站点把 src 换成了 MSE 的 blob: 地址）。
    function unhook(el){
      if(el.__nmbId)send(el,'close');
      el.__nmbGeneration=(el.__nmbGeneration||0)+1;el.__nmbId=null;
      if(el.__nmbOpenGuard){clearTimeout(el.__nmbOpenGuard);el.__nmbOpenGuard=null}
      el.__nmbOpening=false;el.__nmbSeeking=false;el.__nmbState=null;
      el.__nmbWantPlay=undefined;el.__nmbEverWanted=false;
      if(el.__nmbBar){
        // 轨道规则挂在 head 上（不在条里），条删了它不会走，先收掉再删条。
        removeTrackSheet(el.__nmbBar);
        if(el.__nmbBar.parentNode)el.__nmbBar.parentNode.removeChild(el.__nmbBar);
      }
      el.__nmbBar=null;el.__nmbWantsBar=false;el.__nmbBarRect=null;el.__nmbClippers=null;el.__nmbClipRect=null;
      el.__nmbOff=undefined;el.__nmbRect=undefined;el.__nmbFsbar=undefined;el.__nmbFill=undefined;
      // 拆包装：元素放回原布局位、包装盒删除，站点 DOM 恢复接管前的样子。
      unwrapEl(el);
      // 钉住的占位尺寸要还回去：元素交还给内核后，它按自己的样式重新显示。
      if(el.__nmbPinned){el.style.display=el.__nmbPinned.display;el.style.width=el.__nmbPinned.width;el.style.minHeight=el.__nmbPinned.minHeight;el.__nmbPinned=null}
      // 藏过的元素要还原：交还给内核之后，它按自己的样式重新显示（含原生控件）。
      if(el.__nmbHidden!==undefined){el.style.opacity=el.__nmbHidden;el.__nmbHidden=undefined}
      el.__nmbHooked=false;el.__nmbSealed=sealed(el);el.__nmbSkip=false;el.__nmbTries=0;el.__nmbRetryAt=0;
      if(el.__nmbErrGuard){clearTimeout(el.__nmbErrGuard);el.__nmbErrGuard=null}
      el.__nmbHolesKey='';el.__nmbHoleAt=0;
      removeOverrides(el);
      if(el.__nmbHadControls){try{el.controls=true}catch(e){}}
      try{el.removeAttribute('data-native-media')}catch(e){}
      var k=hookedEls.indexOf(el);if(k>=0)hookedEls.splice(k,1);
    }
    // native 已经丢掉这个元素（应答 missing，见 send）：退回"未接管"但**不交还给站点**——
    // 下一帧 tick 会重新 open，成功就再 adopt。和 unhook 的区别是这里保留控件条、不置 sealed：
    // 换文档导致的清表是可自愈的，站点换成 blob: 那种才是真该放手。
    function forget(el){
      if(!el.__nmbHooked)return;
      el.__nmbGeneration=(el.__nmbGeneration||0)+1;
      var k=hookedEls.indexOf(el);if(k>=0)hookedEls.splice(k,1);
      el.__nmbHooked=false;el.__nmbSealed=false;el.__nmbSkip=false;
      el.__nmbOpening=false;el.__nmbRetryAt=0;el.__nmbTries=0;
      if(el.__nmbErrGuard){clearTimeout(el.__nmbErrGuard);el.__nmbErrGuard=null}
      el.__nmbHolesKey='';el.__nmbHoleAt=0;
      // 播放意图也要重采：条目被丢掉时页面通常不会**再**调一次 play（它以为一直在播），
      // 留着上一轮消费掉的标记会让重开后的条目永远停在 paused —— 表现就从"闪一下"变成"永久黑"。
      // 置回 undefined，下次 hook 会重新从元素当前状态采样（st.paused 没被这里复位）。
      el.__nmbWantPlay=undefined;
      try{el.removeAttribute('data-native-media')}catch(e){}
      // 旧会话的时长/位置不能再留着：控件条会拿它画进度，看起来像"还能播"。
      var s0=st(el);s0.duration=0;s0.position=0;s0.bufferedUntil=0;s0.ready=false;s0.ended=false;s0.endedFired=false;
      // 边沿标记一起复位：重新接管后 loadedmetadata/canplay 那一串要能再发一遍，否则站点等不到。
      s0.rs=0;s0.errFired=false;s0.progAt=undefined;
      // 反复丢（例如内核每次都在接管后清表）就彻底放手：无限重开解码线程比停住更糟。
      el.__nmbLost=(el.__nmbLost||0)+1;
      if(el.__nmbLost>3){el.__nmbSkip=true;el.__nmbRetryAt=0}
    }
    function scan(root){if(root.nodeType===1&&/^(AUDIO|VIDEO)$/.test(root.tagName))hook(root);var q=root.querySelectorAll?root.querySelectorAll('audio,video'):[];for(var i=0;i<q.length;i++)hook(q[i]);}
    // 站点的能力探测（例如腾讯视频判断"当前设备能不能播"）读的就是媒体元素上的
    // duration/error/readyState/play()/load()。以前在 HTMLMediaElement.prototype 上全局改写这些属性，
    // 等于把站点的探测结果伪造了，站点会据此得出"设备不适合播放"这种错误结论；
    // 现在只对真正被本桥接管的元素逐个改写，其他元素保持内核原生实现。
    var overridden=['duration','currentTime','paused','ended','readyState','networkState','error','seekable','buffered','played','volume','muted','play','pause','load','videoWidth','videoHeight'];
    function defOn(el,name,get,set){try{Object.defineProperty(el,name,{configurable:true,get:get,set:set||function(){}})}catch(e){}}
    // defOn 是"属性"的写法：getter 的返回值就是属性值。
    // 方法不能照这个套 —— play/pause/load 的方法体被当成 getter 后，读出来的属性值是
    // Promise.resolve()（play）或 undefined（pause/load），站点一调就报 "el.play is not a function"，
    // 腾讯视频的播放器正好断在这一句（superplayer.js 里 this._video.play()）。
    // 方法统一走这里：getter 返回一个真正的函数，调用时 this 与 arguments 照常落到方法体上。
    function defM(el,name,fn){
      defOn(el,name,function(){return function(){return fn.apply(this,arguments)}});
    }
    // 桥交给页面的 buffered/played/seekable 是普通对象，而**内核自己**给的那个 buffered 判
    // `instanceof TimeRanges` 是**真**（实测 mb108：TimeRanges 是原生 function，本桥没补过它）。
    // 同一句判断在没接管时为真、一被接管就变成假，等于把站点挤到它本来不会走的支路上
    // （有的播放器就是靠这个判断决定"能不能用 buffered 算缓冲进度"）。这里把替身的原型挂到接口上，
    // 让两边自洽。MediaError 不在内核里、由 kMediaApiShim 补，注入在同一帧内更早的位置完成，
    // 所以取原型要延到调用时（这里不缓存 window[name]）；名字不在就退回普通对象，不抛。
    function asIf(o,name){try{var F=window[name];if(F&&F.prototype)Object.setPrototypeOf(o,F.prototype)}catch(e){}return o}
    function fakeRanges(el){return asIf({length:1,start:function(){return 0},end:function(){return st(el).duration}},'TimeRanges')}
    // 站点普遍靠事件推进自己的播放流程：有的播放器等 canplay/loadeddata 才放行"点击播放"，
    // 有的用 loadedmetadata 初始化控件，有的靠 progress 画缓冲条。桥以前只发
    // durationchange/timeupdate/ended，这些站点一直等不到，就永远停在"正在加载"——
    // 画面其实在走、进度也在走，只有站点自己的 UI 不动，很难看出是事件没发。
    // 这里按虚拟 readyState 的跨越补发（0→4 一次补齐四个，顺序同规范），是边沿触发、不会刷屏。
    // waiting/stalled 不假发：解码器不把"饥饿"状态回给页面，编一个只会让站点误判卡顿。
    function advance(el,s,data){
      if(s.error&&!s.errFired){s.errFired=true;fire(el,['error'])}
      if(!s.ready)return;
      if(s.rs!==4){s.rs=4;fire(el,['loadedmetadata','loadeddata','canplay','canplaythrough'])}
      if(typeof s.bufferedUntil==='number'&&s.bufferedUntil!==s.progAt){s.progAt=s.bufferedUntil;fire(el,['progress'])}
    }
    // 用真实的时长/进度覆盖元素自身的属性，否则内核无法解复用时会话条无法拖动。
    function installOverrides(el){
      if(el.__nmbPatched)return;el.__nmbPatched=true;
      defOn(el,'duration',function(){return st(this).duration});
      defOn(el,'currentTime',function(){return st(this).position},function(v){
        var el=this;if(!el.__nmbHooked)return;var t=Number(v)||0;if(!isFinite(t)||t<0)t=0;
        el.__nmbSeeking=true;st(el).position=t;
        fire(el,['seeking']);send(el,'seek',t);
        var generation=el.__nmbGeneration;
        setTimeout(function(){if(!el.__nmbHooked||generation!==el.__nmbGeneration)return;el.__nmbSeeking=false;fire(el,['seeked']);send(el,'state')},350);
      });
      defOn(el,'paused',function(){return st(this).paused});
      defOn(el,'ended',function(){return st(this).ended});
      defOn(el,'readyState',function(){return st(this).ready?4:0});
      defOn(el,'networkState',function(){return 1});
      // 错误对象也要挂到 MediaError 的原型上：站点常写 if(err instanceof MediaError) 才去读 code，
      // 判假就整段错误处理都不走（内核原本没有 MediaError，这个原型由 kMediaApiShim 补出来）。
      defOn(el,'error',function(){return st(this).error?asIf({code:3,message:'native decode failed'},'MediaError'):null});
      defOn(el,'seekable',function(){return fakeRanges(this)});
      defOn(el,'buffered',function(){return fakeRanges(this)});
      defOn(el,'played',function(){return fakeRanges(this)});
      // 内核给不了解码尺寸，这两个值恒 0；站点播放器拿它们做画面适配/弹幕排版/全屏判定，
      // 0x0 会走错分支。真实尺寸由 native 应答的 vw/vh 带回（open 成功前为 0，符合"尚不可知"）。
      defOn(el,'videoWidth',function(){var d=st(this).raw;return d&&d.vw?d.vw:0});
      defOn(el,'videoHeight',function(){var d=st(this).raw;return d&&d.vh?d.vh:0});
      // volume/muted 的 setter 过去只把值转给原生，自己一声不响：站点的音量按钮点下去，
      // 它自己的图标（往往靠 volumechange 才重画）就永远不同步。这里补上事件。
      defOn(el,'volume',function(){return this.__nmbVolume==null?1:this.__nmbVolume},function(v){this.__nmbVolume=Number(v);send(this,'volume',this.__nmbVolume);fire(this,['volumechange'])});
      defOn(el,'muted',function(){return !!this.__nmbMuted},function(v){this.__nmbMuted=!!v;send(this,'volume',this.__nmbMuted?0:(this.__nmbVolume==null?1:this.__nmbVolume));fire(this,['volumechange'])});
      defM(el,'play',function(){
        var el=this;recoverSkipped(el);hook(el);
        if(!el.__nmbHooked)return HTMLMediaElement.prototype.play.apply(el,arguments);
        st(el).paused=false;st(el).ended=false;st(el).endedFired=false;fire(el,['play','playing']);
        return Promise.resolve();
      });
      defM(el,'pause',function(){
        var el=this;if(!el.__nmbHooked)return HTMLMediaElement.prototype.pause.apply(el,arguments);
        st(el).paused=true;fire(el,['pause']);
      });
      defM(el,'load',function(){
        var el=this;
        unhook(el);el.__nmbLost=0;el.__nmbSrc=sourceOf(el);
        if(!servable(el)||sealed(el))return HTMLMediaElement.prototype.load.apply(el,arguments);
        hook(el);
      });
    }
    function removeOverrides(el){
      if(!el.__nmbPatched)return;el.__nmbPatched=false;
      for(var i=0;i<overridden.length;i++){try{delete el[overridden[i]]}catch(e){}}
    }
    // 用事件捕获（而不是改写原型）来发现站点新建后主动播放的媒体元素：
    // 捕获阶段只读事件、不改任何属性，站点读到的仍是内核原生实现。
    // 顺手记下页面的"播放意图"：接管**完成之前**发生的 play/pause（autoplay、站点在 loadstart
    // 附近自己调 play、或"先播一下再暂停"的预热手法）落在内核原生实现上，而内核没有解码能力，
    // 这一次意图就此丢掉；桥是异步接管的，接管完再没人调过 play。adopt 时兑现这个标记。
    function sniff(e){
      var el=e.target;
      if(!el||!el.tagName||!/^(AUDIO|VIDEO)$/.test(el.tagName))return;
      if(e.type==='play'){el.__nmbWantPlay=true;el.__nmbEverWanted=true}
      else if(e.type==='pause')el.__nmbWantPlay=false;
      hook(el);
    }
    document.addEventListener('play',sniff,true);
    document.addEventListener('pause',sniff,true);
    document.addEventListener('loadstart',sniff,true);
    // 关键拦截：内核自己解不了的源（实测腾讯给的是 MPEG-TS 直链，Chromium 系内核不认 TS 容器）
    // 会在元素上派发原生 error。这一事件在桥异步接管完成之前就到了，站点播放器（腾讯
    // thumbplayer）收到后状态机立刻锁进错误页（70011103.1 "网络状况异常"），之后桥把流缓冲好、
    // 画面声音都在走也救不回来。媒体 error 不冒泡，但 capture 阶段必经 document：在这里拦截至 target
    // 监听（站点的错误处理都挂元素自己上）之前。已决定放手（skip/sealed）的元素不拦，让内核报错
    // 原样到达；桥最终打不开时会在 skip 分支补一个合成 error，站点不会永远傻等。
    // 但页面上还有一类"只加载、从不播放"的探测/预加载元素（实测腾讯的剧集悬停预览 video）：
    // 无桥时它们的原生 error 会让站点把预览容器藏掉；无脑吞错反而让黑窗常驻、遮住选集列表。
    // "有没有播放意图"在这里不可靠：正片的 play 可能晚数秒才来（实测 thumbplayer 先建双 video
    // 竞速，play 在重试成功之后）。用 DOM 结构判据：继续保护"非隐藏、面积最大的那一路视频"
    // （正片 530x298 vs 悬停预览 384x216，靠面积比 0.9 实测稳定可分；不看是否在当前视口内——
    // 折叠下方需滚动才可见的第二个正文视频与正片等大，必须一并保护），其余视频元素看门狗到点放手并补回 error。
    // audio 没有视觉、误接管无副作用，一律保护。
    function isPrimaryMedia(el){
      if(el.tagName==='AUDIO')return true;
      if(el.__nmbEverWanted||el.autoplay===true||(el.hasAttribute&&el.hasAttribute('autoplay')))return true;
      var r=el.getBoundingClientRect();
      if(r.width<16||r.height<16)return false;
      var cs=getComputedStyle(el);
      if(cs.display==='none'||cs.visibility!=='visible'||cs.opacity==='0')return false;
      // 不能用"是否在当前视口内"否决：多视频页里第二个视频常排在折叠下方（要滚动才可见），
      // 它是正文里真实存在、滚动即可播放的视频。仅因 top>innerHeight 就放弃，会误杀内核打不开、
      // 本应由桥 ffmpeg 接管的源（实测 media_suite 的 v2：内核 ready:0 err:4，ffmpeg 却能正常开）。
      // 探测/预览元素改用与视口无关的特征过滤：尺寸<16、display:none、面积占比、明显小于最大视频。
      var area=r.width*r.height;
      if(area<window.innerWidth*window.innerHeight*0.05)return false;
      var maxArea=0,list=document.querySelectorAll('video');
      for(var i=0;i<list.length;i++){var o=list[i];
        if(o===el)continue;var or2=o.getBoundingClientRect();
        if(or2.width<16||or2.height<16)continue;var oc=getComputedStyle(o);
        if(oc.display==='none'||oc.visibility!=='visible'||oc.opacity==='0')continue;
        if(or2.width*or2.height>maxArea)maxArea=or2.width*or2.height;
      }
      return area>=maxArea*0.9;
    }
    document.addEventListener('error',function(e){
      var el=e.target;
      if(!el||!el.tagName||!/^(AUDIO|VIDEO)$/.test(el.tagName))return;
      if(el.__nmbSkip||el.__nmbSealed)return;
      hook(el);
      if(!el.__nmbErrGuard){
        el.__nmbErrGuard=setTimeout(function(){
          el.__nmbErrGuard=null;
          if(el.__nmbSkip||el.__nmbSealed)return;
          if(!isPrimaryMedia(el)){
            unhook(el);
            el.__nmbSkip=true;el.__nmbRetryAt=Date.now()+30000;
            fire(el,['error']);
          }
        },1500);
      }
      e.stopImmediatePropagation();
      e.preventDefault();
    },true);
    // 采集"绘制在视频元素之上"的页面元素矩形，交给 native 在合成视频帧时整块挖空。
    // 背景：帧是 StretchDIBits(SRCCOPY) 直接盖在页面像素上的，站点自绘的播放器 UI（控制栏、弹幕、
    // 弹窗、遮罩、广告、loading 动画）全在视频矩形之内、视频帧之上，不挖就会被画面压没。
    // elementsFromPoint 由内核做命中测试，返回该点自顶向下的元素栈：栈里 video 之前的是覆盖物，
    // video 之后的是它的祖先容器（在视频之下，绝不能挖——整片黑底容器都在那一段）。
    // 规则网格 + 四边加密采样：控制栏通常只有 40px 高，纯网格可能一个点都落不进去。
    var HOLE_STEP=30, HOLE_EDGE=8, HOLE_MAX=80, holeMark=0;
    function collectHoles(el){
      if(window.__nmbNoHoles||typeof document.elementsFromPoint!=='function')return '';
      var r=el.getBoundingClientRect();
      if(r.width<16||r.height<16)return '';
      var token=++holeMark;
      var pts=[],x,y;
      for(x=r.left+HOLE_STEP/2;x<r.right;x+=HOLE_STEP)
      for(y=r.top+HOLE_STEP/2;y<r.bottom;y+=HOLE_STEP)pts.push([x,y]);
      // 上下两条长边、左右两条短边各加密一排点，细高/细宽的控制栏、进度条、弹幕都靠它们命中。
      for(x=r.left+HOLE_STEP/2;x<r.right;x+=HOLE_STEP){pts.push([x,r.top+HOLE_EDGE]);pts.push([x,r.bottom-HOLE_EDGE])}
      for(y=r.top+HOLE_STEP/2;y<r.bottom;y+=HOLE_STEP){pts.push([r.left+HOLE_EDGE,y]);pts.push([r.right-HOLE_EDGE,y])}
      var found=[];
      function take(o){
        if(!o||o===el||o===document||o===document.documentElement||o===document.body)return;
        if(o.__nmbHoleMark===token)return;
        o.__nmbHoleMark=token;
        if(found.length>=HOLE_MAX)return;
        try{
          var z=getComputedStyle(o);
          if(z.display==='none'||z.visibility!=='visible'||z.opacity==='0')return;
          var b=o.getBoundingClientRect();
          if(b.width<=0||b.height<=0)return;
          var x1=Math.max(r.left,b.left),y1=Math.max(r.top,b.top),
            x2=Math.min(r.right,b.right),y2=Math.min(r.bottom,b.bottom);
          if(x2-x1<1||y2-y1<1)return;
          // 高不透明度、无圆角的实色背景才能整块挖空；否则软洞只回贴亮色像素，
          // 会把深色控制栏/菜单背景误留在视频帧上。背景图/渐变仍不猜测其透明度。
          var kind='s';
          try{
            var bg=String(z.backgroundColor||''),alpha=0;
            var mm=bg.match(/^rgba?\(([^)]*)\)$/);
            if(mm){var pp=mm[1].trim().split(/\s*[,/]\s*|\s+/);
              alpha=pp.length===3?1:parseFloat(pp[3]);
              if(pp.length===4&&/%$/.test(pp[3]))alpha/=100;
            }
            var opaque=alpha>=0.85;
            for(var p=o;p&&p.nodeType===1;p=p.parentElement){
              var ps=p===o?z:getComputedStyle(p);
              if(parseFloat(ps.opacity)<1){opaque=false;break}
            }
            var rounded=[z.borderTopLeftRadius,z.borderTopRightRadius,z.borderBottomLeftRadius,z.borderBottomRightRadius]
              .some(function(v){return String(v||'0').split(/\s+/).some(function(n){return parseFloat(n)>0})});
            if(opaque&&!rounded)kind='h';
          }catch(e){}
          // 起点四舍五入、尺寸向上取整：避免相邻洞之间或洞与视频边缘露出 1px 的视频残边。
          found.push(kind+Math.round(x1)+','+Math.round(y1)+','
            +Math.ceil(x2-Math.round(x1))+','+Math.ceil(y2-Math.round(y1)));
        }catch(e){}
      }
      for(var i=0;i<pts.length;i++){
        var stack;
        try{stack=document.elementsFromPoint(pts[i][0],pts[i][1])}catch(e){continue}
        if(!stack||stack.indexOf(el)<0)continue;
        for(var j=0;j<stack.length;j++){
          if(stack[j]===el)break; // 到 video 为止；再往下全是祖先容器
          take(stack[j]);
        }
      }
      return found.join(';');
    }
    var lastState=0;
    var lastIdleScan=0;
    // 元素连续隐藏多久就释放原生那一路（毫秒）。给一点宽限是因为站点换皮肤/折叠动画期间
    // 会有一两帧尺寸归零，立刻放手会把正常播放的媒体也干掉。
    var kHiddenReleaseMs=1500;
    // 覆盖物会在没有布局变化时自己变动（控制栏靠鼠标移动淡入淡出、弹窗动画、弹幕滚动），
    // rect 上报捕获不到这些，所以单独节流轮询；滚动/缩放/鼠标移动时立即标脏、下一帧就采。
    var holeDirty=true,holeActiveUntil=0,lastHoleMove=0;
    function markHoles(event){
      if(!hookedEls.length)return;
      // 鼠标在同一媒体控件内移动时不需要每个事件都重新测量所有覆盖物；
      // 高频测量会和页面 hover 重绘、视频帧合成竞争，表现为悬浮闪烁。
      if(event&&event.type==='mousemove'){
        var now=Date.now();
        if(now-lastHoleMove<50)return;
        lastHoleMove=now;
      }
      holeDirty=true;holeActiveUntil=Date.now()+500
    }

    function syncAll(){
      if(fsElement&&(!attached(fsElement)||!attached(fsHost)||(fsPlaceholder&&!attached(fsPlaceholder))))fsExit();
      var now=Date.now();
      // 没有任何已接管媒体时大幅降频：rAF 每帧 + 每个 scroll 事件都全文档 querySelectorAll('audio,video')
      // 并强制 layout，在 B站这类重页面上实测把滚动首帧延迟抬高一倍（关桥 70ms → 开桥 160ms）。
      // 新元素由 MutationObserver 的 scan() 实时发现，处于 opening/重试退避的元素 500ms 兜底再试足够；
      // 一旦有元素 adopt（hookedEls 非空）立即恢复逐帧，保证播放中的视频跟随滚动不慢半拍。
      if(hookedEls.length===0){
        if(now-lastIdleScan<500)return;
        lastIdleScan=now;
      }
      dropDetached();
      // 位置每帧都对，播放状态按固定间隔回读，避免每帧都做一次桥调用。
      var wantState=now-lastState>=200;
      if(wantState)lastState=now;
      var q=document.querySelectorAll('audio,video');
      for(var i=0;i<q.length;i++){
        var el=q[i];
        var r=el.getBoundingClientRect();
        // 组件被隐藏（站点 display:none / 折叠面板 / 切走的 tab）时元素**还在 DOM 里**，
        // 所以 dropDetached 管不到它，可画面和控件条都已经没了——不管的话原生那一路会一直解码
        // 下去、声音继续放、缓冲继续涨，用户以为"关掉了"其实还在跑。
        // 隐藏期间根本不接管；已经接管的，连续隐藏 kHiddenReleaseMs 之后把原生那一路释放掉
        // （用 forget 而不是 unhook：不置 sealed，重新显示时下一帧 tick 会再 hook/open 接回来）。
        if(r.width<=0||r.height<=0){
          if(!el.__nmbHooked)continue;
          if(!el.__nmbHiddenAt){
            el.__nmbHiddenAt=now;el.__nmbClipRect=null;el.__nmbBarRect=null;el.__nmbRect=null;
            placeBar(el);send(el,'rect',null,null,false,r);continue;
          }
          if(now-el.__nmbHiddenAt<kHiddenReleaseMs)continue;
          send(el,'close');forget(el);el.__nmbHiddenAt=0;
          continue;
        }
        el.__nmbHiddenAt=0;
        hook(el);
        // 没被接管的元素（MSE/blob:、或原生打不开的地址）原样交给内核和站点，
        // 不产生任何桥调用：重页面上每帧几十次无效往返本身就是卡死的主因之一。
        if(!el.__nmbHooked)continue;
        // 站点换源（实测腾讯视频：先用 .f2.mp4，播几秒后换成一整条 .ts.m3u8）时，
        // 原生会重开一路解码，新会话报到时长之前页面若还留着上一份的时长，控件条就会拿旧值画
        // （实测位置已经走到 28s，右边仍显示 /15）。所以源一变就把这份状态清掉，等新值回来。
        var srcNow=sourceOf(el);
        if(el.__nmbSrc!==srcNow){el.__nmbSrc=srcNow;
          var s0=st(el);s0.duration=0;s0.position=0;s0.bufferedUntil=0;s0.ended=false;s0.endedFired=false;
          s0.rs=0;s0.errFired=false;s0.progAt=undefined;}
        // 视口尺寸与全屏状态也进 key：placeBar 的"是否被窗口裁到"、音频 fill 都看 innerWidth/innerHeight，
        // 而全屏中的元素要切条挂点/藏条。少了这两项，光缩窗口/进全屏而元素 rect 没动时条就不会重摆。
        // 条自己的状态也必须进：条的几何现在由 CSS 从包装盒算出，不等于脚本常量——显隐、fill 翻转、
        // 全屏搬移都要靠 key 变化把新几何过桥给 native（挖洞矩形就是条 DOM 的实测值）。
        // 元素可见区域（clipBox 结果）同样进 key：native 画面只画这块交集，站点容器滚动/伸缩
        // 改变裁剪而元素 rect 不变时，必须靠 key 变化把新交集过桥。注意 key 里要用**当场算的**
        // clipBox，不能读 __nmbClipRect 缓存——placeBar 只在 key 变化时才跑，读缓存就是死锁
        // （clip 变了 key 不变 → placeBar 不跑 → 缓存不更新 → 永远不发现）。
        placeBar(el);
        r=el.getBoundingClientRect();
        var cb=clipBox(el,r);
        // clip 与条的存亡无关：拆包降级（保险判定布局被包装改写、不建条）的元素照样可见，
        // 画面照样要按交集裁。placeBar 只在有条时才会走到存值那一步，无条元素的 clip
        // 必须在这里兜底——send 直接读这份缓存，权威写入点就放在当场算 ckey 的旁边。
        el.__nmbClipRect=cb?[Math.round(cb.l),Math.round(cb.t),Math.round(cb.r-cb.l),Math.round(cb.b-cb.t)]:null;
        var ckey=cb?Math.round(cb.l)+':'+Math.round(cb.t)+':'+Math.round(cb.r-cb.l)+':'+Math.round(cb.b-cb.t):'-';
        var key=Math.round(r.left)+','+Math.round(r.top)+','+Math.round(r.width)+','+Math.round(r.height)+
                ','+window.innerWidth+','+window.innerHeight+','+(document.fullscreenElement?1:0)+
                ','+(el.__nmbOff||0)+','+(el.__nmbBarRect?el.__nmbBarRect.join(':'):'-')+','+ckey;
        var changed=el.__nmbRect!==key;el.__nmbRect=key;
        // 滚动位置先发给 native；collectHoles 的命中测试很重，不能挡住最新 rect。
        if(changed)send(el,'rect',null,null,true,r);
        if(el.tagName==='VIDEO'&&(changed||holeDirty||el.__nmbHoleAt===undefined||
            now-el.__nmbHoleAt>(now<holeActiveUntil?50:400))){
          el.__nmbHoleAt=now;
          var hk=cb?collectHoles(el):'';
          if(hk!==el.__nmbHolesKey){
            el.__nmbHolesKey=hk;send(el,'holes',hk,null,true,r);
          }
        }
        if(!changed&&wantState)send(el,'state',null,null,true,r);
        if(el.__nmbWantsBar)syncBar(el);
      }
      holeDirty=false;
    }
    // 用动画帧逐帧同步元素位置：只用定时器的话，页面滚动时原生视频画面会慢半拍，
    // 看起来就像视频在跟着页面滚动。
    var raf=window.requestAnimationFrame||function(fn){return setTimeout(fn,16)};
    function tick(){syncAll();raf(tick)}
    raf(tick);
    function syncLayout(){markHoles();syncAll()}
    window.addEventListener('scroll',syncLayout,true);
    window.addEventListener('resize',syncLayout,true);
    // 覆盖物在这些时机变化：翻页/内滚（弹幕容器也会内滚）、缩放、鼠标进出视频区（控制栏淡入淡出）、
    // 点击（弹出菜单/设置面板/清晰度列表）。监听只置脏标记，采集节流仍由 syncAll 统一做。

    ['mousemove','mouseover','mouseout','pointerdown','click','focusin','focusout',
      'transitionrun','transitionend','animationstart','animationend','fullscreenchange']
      .forEach(function(type){document.addEventListener(type,markHoles,true)});
    scan(document);
    // 脚本上下文刚建立时 documentElement 可能还不存在；延迟安装且只装一次。
    function installMediaObserver(){
      var root=document.documentElement;
      if(!root){setTimeout(installMediaObserver,25);return}
      if(document.__nmbMediaObserver)return;
      document.__nmbMediaObserver=true;
      new MutationObserver(function(rs){
        clipVersion++;markHoles();
        for(var i=0;i<rs.length;i++)for(var j=0;j<rs[i].addedNodes.length;j++)scan(rs[i].addedNodes[j]);
      }).observe(root,{childList:true,subtree:true,attributes:true,attributeFilter:['class','style','hidden']});
    }
    installMediaObserver();
    // 把当前文档标题回写到宿主窗口标题，宿主据此可以确认页面跳转是否真的发生。
    // 只有主框架说了算：真实站点里满地都是 about:blank 的 iframe，它们也会报自己的
    // document.title||location.href；允许它们改窗口标题的话，窗口标题会当场变成 about:blank
    // （v.qq.com 实测如此），宿主就再也读不到当前页面了。
    function isTopFrame(){try{return window.top===window}catch(e){return false}}
    function reportTitle(){
      if(!isTopFrame())return;
      try{
        var t=String(document.title||location.href).replace(/[\t\r\n]/g,' ');
        window.mbQuery(++querySeq,['title','page','page','',t,'0','0','0','0','0','0','0','0'].join('\t'),function(){});
      }catch(e){}
    }
    reportTitle();document.addEventListener('DOMContentLoaded',reportTitle);
  }catch(e){window.__nmbInstalled=false}
})();(function(){
  if(window.__nmbHoverOn)return;window.__nmbHoverOn=true;
  var hasPE=false;try{hasPE=typeof window.PointerEvent==='function'}catch(e){}
  var chain=[];var X=0,Y=0;var lastX=0,lastY=0;var prevTop=null;
  // ── 档2：:hover 伪类 CSS 复制 ──
  // 浏览器不为合成事件激活 :hover 伪类，这里把样式表里 .sel:hover 的声明复制为
  // .sel.__nmb-h，给当前 hover 链的元素加 __nmb-h class 即可生效。
  var HC='__nmb-h';var cursorSeq=0;
  // 先定义核心函数：即使后续 CSS 克隆/MutationObserver 初始化抛异常，__nmbHoverMove 仍可用
  function setHoverClass(oldChain,newChain){
    for(var i=0;i<oldChain.length;i++){var el=oldChain[i];
      if(newChain.indexOf(el)<0){try{el.classList.remove(HC)}catch(e){}}}
    for(var j=0;j<newChain.length;j++){var el=newChain[j];
      if(oldChain.indexOf(el)<0){try{el.classList.add(HC)}catch(e){}}}
  }
  // ── 档3：光标手型 ──
  // 检测当前 top 元素的 computed cursor，cursor 变化时通过 mbQuery 通知 native 切系统光标。
  function updateCursor(top){
    var cur='default';
    if(top){try{cur=getComputedStyle(top).cursor||'default'}catch(e){}}
    if(cur.indexOf('pointer')>=0)cur='pointer';
    else if(cur.indexOf('text')>=0)cur='text';
    else cur='default';
    if(window.__nmbCursor!==cur){
      window.__nmbCursor=cur;
      try{var T=String.fromCharCode(9);if(window.mbQuery)window.mbQuery(++cursorSeq,'cursor'+T+T+T+T+cur,function(){})}catch(e){}
    }
  }
  function emit(el,type,isPointer,related){
    try{
      var e;
      if(isPointer&&hasPE){
        e=new PointerEvent(type,{bubbles:true,cancelable:true,composed:true,view:window,clientX:X,
            clientY:Y,screenX:X,screenY:Y,button:0,buttons:0,relatedTarget:related||null,
            pointerId:1,width:1,height:1,pressure:0,pointerType:'mouse',isPrimary:true});
      }else{
        e=new MouseEvent(type,{bubbles:true,cancelable:true,composed:true,view:window,clientX:X,
            clientY:Y,screenX:X,screenY:Y,button:0,buttons:0,relatedTarget:related||null});
      }
      el.dispatchEvent(e);
    }catch(e){}
  }
  window.__nmbHoverMove=function(nx,ny){
    X=nx;Y=ny;
    var hit=[];
    try{
      if(document.elementsFromPoint)hit=document.elementsFromPoint(X,Y)||[];
      else if(document.elementFromPoint){var t=document.elementFromPoint(X,Y);if(t)hit=[t]}
    }catch(e){hit=[]}
    var top=hit[0]||null;
    // 离开链：旧链中不在新链的元素；mouseleave 不冒泡，自内向外（chain 顶→底）。
    for(var i=0;i<chain.length;i++){var o=chain[i];
      if(hit.indexOf(o)<0)emit(o,'mouseleave',0,top)}
    // 仅当命中目标变化才补 over/out，否则每次移动都 over 会把站点的"进入"逻辑打满。
    if(top!==prevTop){
      if(prevTop){emit(prevTop,'mouseout',0,top);if(hasPE)emit(prevTop,'pointerout',1,top)}
      if(top){emit(top,'mouseover',0,prevTop);if(hasPE)emit(top,'pointerover',1,prevTop)}
    }
    // 进入链：新链中不在旧链的元素；mouseenter 不冒泡，自外向内（hit 底→顶）。
    for(var j=hit.length-1;j>=0;j--){var n=hit[j];
      if(chain.indexOf(n)<0)emit(n,'mouseenter',0,prevTop)}
    // 同一坐标重复送达时不再制造无意义的 DOM mousemove/pointermove；
    // 这能避免页面 hover 与媒体覆盖物同步互相触发重绘。
    if(top&&(X!==lastX||Y!==lastY)){
      emit(top,'mousemove',0,null);if(hasPE)emit(top,'pointermove',1,null)
    }
    lastX=X;lastY=Y;
    setHoverClass(chain,hit);
    updateCursor(top);
    chain=hit;prevTop=top;
  };
  // ── 档2：:hover 伪类 CSS 复制（放在核心函数之后，初始化失败不影响 __nmbHoverMove）──
  // 同源 CSS 直接读 cssRules；跨域 CSS（SecurityError）用 XHR 异步获取文本后正则提取 :hover。
  var hoverStyle=null;var hoverStyleCross=null;var crossRules='';var clonedSheets=new Set();
  function ensureHoverStyle(){
    if(!hoverStyle){hoverStyle=document.createElement('style');hoverStyle.setAttribute('data-nmb-hover','1')}
    if(!hoverStyleCross){hoverStyleCross=document.createElement('style');hoverStyleCross.setAttribute('data-nmb-hover-cross','1')}
    // onScriptContext 早期 documentElement 可能为 null：appendChild 目标为 null 时不做（等
    // initCssClone 延迟重试）；documentElement 可用后每次检查 parentNode 把 style 移到 head。
    var h=document.head||document.documentElement;
    if(!h)return;
    if(hoverStyle.parentNode!==h)h.appendChild(hoverStyle);
    if(hoverStyleCross.parentNode!==h)h.appendChild(hoverStyleCross);
  }
  function parseHoverFromCssText(text){
    var out='';var re=/(?:^|\})\s*([^{}]*:hover[^{}]*)\s*\{([^{}]*)\}/g;var m;
    while((m=re.exec(text))){
      var sel=m[1].trim();var decl=m[2].trim();
      var parts=sel.split(',');var made='';
      for(var k=0;k<parts.length;k++){
        var s=parts[k].trim();if(s.indexOf(':hover')<0)continue;
        made+=(made?',':'')+s.replace(/:hover\b/g,'.'+HC);
      }
      if(made){out+=made+'{'+decl+'}\n'}
    }
    return out;
  }
  function cloneHoverRules(){
    try{
      ensureHoverStyle();
      var out='';
      for(var si=0;si<document.styleSheets.length;si++){
        var ss=document.styleSheets[si];if(clonedSheets.has(ss))continue;
        var rules=null;
        try{rules=ss.cssRules||ss.rules}catch(e){
          clonedSheets.add(ss);
          if(ss.href){
            try{
              var xhr=new XMLHttpRequest();
              xhr.open('GET',ss.href,true);
              xhr.onreadystatechange=function(){
                if(xhr.readyState==4&&xhr.status>=200&&xhr.status<300&&xhr.responseText){
                  try{var extra=parseHoverFromCssText(xhr.responseText);
                    if(extra){crossRules+=extra;hoverStyleCross.textContent=crossRules}}catch(e2){}
                }
              };
              xhr.send();
            }catch(e2){}
          }
          continue;
        }
        clonedSheets.add(ss);
        for(var ri=0;ri<rules.length;ri++){
          var r=rules[ri];if(!r.selectorText||r.selectorText.indexOf(':hover')<0)continue;
          var sels=r.selectorText.split(',');var made='';
          for(var k=0;k<sels.length;k++){
            var s=sels[k].trim();if(s.indexOf(':hover')<0)continue;
            var c=s.replace(/:hover\b/g,'.'+HC);
            made+=(made?',':'')+c;
          }
          if(made){out+=made+'{'+r.style.cssText+'}\n'}
        }
      }
      if(out)hoverStyle.textContent=out;
    }catch(e){}
  }
  // onScriptContext 时机 documentElement 可能为 null（文档还没解析出 <html>）：
  // 此时 cloneHoverRules 和 mo.observe 都会静默失败，且 IIFE 幂等不会重入。
  // 用 setTimeout 延迟到 documentElement 可用后再初始化，确保 CSS 克隆和 MutationObserver 都就位。
  function initCssClone(){
    if(!document.documentElement){setTimeout(initCssClone,50);return}
    try{cloneHoverRules()}catch(e){}
    try{
      var mo=new MutationObserver(function(){try{cloneHoverRules()}catch(e){}});
      mo.observe(document.documentElement,{childList:true,subtree:true});
    }catch(e){}
  }
  initCssClone();
})();
