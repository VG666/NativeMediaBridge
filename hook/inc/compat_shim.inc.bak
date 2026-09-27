// 自动生成文件：真源为 hook/js/compat_shim.js，由 _nmb_gen_hooks_inc.py 生成，请勿手改。
R"NMBHOOK((function(){
  try{
    var mode=String(window.__nmbCompatMode||'');
    if(!mode||window.__nmbCompatInstalled)return;
    window.__nmbCompatInstalled=true;
    window.__nmbCapture=window.__nmbCapture||[];
    window.__nmbLog=window.__nmbLog||[];
    window.__nmbNet=window.__nmbNet||[];
    function log(t){try{if(window.__nmbLog.length<80)window.__nmbLog.push(String(t))}catch(e){}}
    // 站点脚本一抛异常，播放器就停在半路，而外面看只是"播放器区域空白"，什么线索都没有。
    // 这里把异常连栈一起记下来，排查时能直接看到是哪一句断的（资源 404 之类没有 message 的噪音跳过）。
    // 栈里每帧都带一长串重复的 URL，而我们要的只是"文件:行:列"，所以先压成文件名，省下的额度留给更多帧
    // （这次的凶手就藏在第二帧：第一帧只是 babel 的 instanceof 辅助，得看是谁把 undefined 的类传进去的）。
    try{
      function stk(s){return String(s==null?'':s).replace(/\s+/g,' ').replace(/https?:\/\/[^\s)]+/g,function(u){
          var a=u.split('/');return a[a.length-1]}).slice(0,700)}
      window.addEventListener('error',function(e){
        var m=e&&(e.message||(e.error&&e.error.message));
        if(!m)return;
        var where=e&&e.filename?String(e.filename).split('/').pop()+':'+e.lineno:'';
        log('err:'+where+' '+String(m).slice(0,60)+' @'+stk(e.error&&e.error.stack));},true);
      window.addEventListener('unhandledrejection',function(e){
        var r=e&&e.reason,m=(r&&(r.message||r))||'';
        if(!m)return;
        log('rej:'+String(m).slice(0,60)+' @'+stk(r&&r.stack));});
    }catch(e){}
    // 所有请求都留一份（只留前 50 条）。排查时"站点压根没请求"和"请求了但失败了"是两种病，
    // 只看媒体 URL 分不出来，所以这里不做过滤。
    function net(m,u){try{
        var s=String(u==null?'':u);
        if(!s||s==='[object Object]')return;
        if(window.__nmbNet.length<50)window.__nmbNet.push(String(m||'GET').toUpperCase().slice(0,5)+' '+s.slice(0,140));
      }catch(e){}}
    function note(u){try{
        var s=String(u==null?'':u);
        if(!s||s==='undefined'||s==='[object Object]')return;
        if(!/\.(m3u8|m4s|mpd|ts|mp4|flv|m4v|aac|mp3)(\?|#|$)/i.test(s))return;
        // 只留前若干个，避免长页面把内存堆起来；同一条只记一次。
        if(window.__nmbCapture.length<60&&window.__nmbCapture.indexOf(s)<0)window.__nmbCapture.push(s.slice(0,400));
      }catch(e){}}

    try{var xo=window.XMLHttpRequest.prototype.open;
      window.XMLHttpRequest.prototype.open=function(m,u){try{net(m,u);note(u)}catch(e){};return xo.apply(this,arguments)}}catch(e){}
    try{if(window.fetch){var of=window.fetch;window.fetch=function(i){
          var u='';try{u=i&&i.url?i.url:i;net('FETCH',u);note(u)}catch(e){}
          return of.apply(this,arguments)}}}catch(e){}
    // 内核把媒体相关的 DOM 接口整个砍了：window.HTMLVideoElement 根本不存在，
    // 而站点里到处是 x instanceof HTMLVideoElement（babel 的 instanceof 辅助函数会走到
    // "Right-hand side of 'instanceof' is not an object"），播放器把视频适配器一构造就断在这里。
    // 这里只补"内核没给的接口名"，并用 Symbol.hasInstance 按标签名判定，
    // 让 video instanceof HTMLVideoElement 得到正确结果（真）而不是一律假 —— 补的是接口，不是能力声明。
    try{
      var DIF={HTMLVideoElement:'VIDEO',HTMLAudioElement:'AUDIO',HTMLSourceElement:'SOURCE',HTMLTrackElement:'TRACK',
          HTMLImageElement:'IMG',HTMLCanvasElement:'CANVAS',HTMLDivElement:'DIV',HTMLSpanElement:'SPAN',
          HTMLAnchorElement:'A',HTMLButtonElement:'BUTTON',HTMLInputElement:'INPUT',HTMLIFrameElement:'IFRAME',
          HTMLScriptElement:'SCRIPT',HTMLStyleElement:'STYLE',HTMLLinkElement:'LINK',HTMLParagraphElement:'P',
          HTMLMediaElement:'VIDEO|AUDIO',HTMLElement:'*',Element:'*',Node:'*'};
      var dif=[];
      for(var dn in DIF){
        try{
          if(typeof window[dn]==='function')continue;
          var want=DIF[dn],F=function(){};
          Object.defineProperty(F,Symbol.hasInstance,{value:function(o){
              try{
                var t=String((o&&o.tagName)||'').toUpperCase();
                if(!t)return false;
                if(want==='*')return true;
                return ('|'+want+'|').indexOf('|'+t+'|')>=0;
              }catch(e){return false}}});
          window[dn]=F;dif.push(dn);
        }catch(e){}
      }
      if(dif.length)log('dom:'+dif.join('/'));
    }catch(e){}
    // 能力探测改口。注意只改"能不能播"的声明，不碰任何带状态的属性（duration/currentTime 等一律原样），
    // 站点于是照常走它自己的播放流程，只是不再因为探测失败而提前放弃。
    try{
      var MP=window.HTMLMediaElement&&window.HTMLMediaElement.prototype;
      if(MP&&typeof MP.canPlayType==='function'&&!MP.__nmbCpt){
        MP.__nmbCpt=true;var oc=MP.canPlayType;
        MP.canPlayType=function(t){
          var r='';try{r=oc.call(this,t)||''}catch(e){}
          var s=String(t==null?'':t).toLowerCase();
          if(s.indexOf('mpegurl')>=0){log('cpt:hls');return 'maybe'}
          if(/video\/(mp4|mpeg|webm)|audio\/(mp4|mpeg|webm)|avc1|mp4a|h264|aac/.test(s))return r||'maybe';
          return r};
      }
    }catch(e){}
    if(mode.indexOf('ios')>=0||mode.indexOf('safari')>=0||mode.indexOf('desktop')>=0){
      // ios/safari 保留历史 UA 伪装；desktop 只补媒体能力，保持 Windows 桌面身份和 HTTP UA 不变。
      var mob=mode.indexOf('ios')>=0;
      if(mode.indexOf('desktop')>=0&&!mob){
        log('desktop:hls');
      }
      if(mode.indexOf('desktop')<0){
        var FULL=mob?'iPhone; CPU iPhone OS 17_5 like Mac OS X':'Macintosh; Intel Mac OS X 10_15_7';
        var VER=mob?'Version/17.5 Mobile/15E148':'Version/17.5';
        function fake(owner,prop,value){try{Object.defineProperty(owner,prop,{configurable:true,get:function(){return value}})}catch(e){}}
        fake(Navigator.prototype,'userAgent','Mozilla/5.0 ('+FULL+') AppleWebKit/605.1.15 (KHTML, like Gecko) '+VER+' Safari/605.1.15');
        fake(Navigator.prototype,'appVersion','5.0 ('+FULL+') AppleWebKit/605.1.15 (KHTML, like Gecko) '+VER+' Safari/605.1.15');
        fake(Navigator.prototype,'platform',mob?'iPhone':'MacIntel');
        fake(Navigator.prototype,'vendor','Apple Computer, Inc.');
        fake(Navigator.prototype,'maxTouchPoints',mob?5:0);
        fake(Navigator.prototype,'userAgentData',undefined);
        log('ua:'+String(navigator.userAgent).slice(0,48));
      }
    }
    // No binary MSE bridge: preserve native APIs, never publish fake buffers.
    log('compat:'+mode);
  }catch(e){}
})();
)NMBHOOK"
