// 自动生成文件：真源为 hook/js/media_api_shim.js，由 _nmb_gen_hooks_inc.py 生成，请勿手改。
R"NMBHOOK((function(){
  if(window.__nmbMediaApiInstalled)return;window.__nmbMediaApiInstalled=true;
  try{
    function mk(name,duck,init){
      if(typeof window[name]==='function')return;
      var F=function(){if(init)try{init.apply(this,arguments)}catch(e){}};
      try{Object.defineProperty(F,'name',{value:name})}catch(e){}
      try{Object.defineProperty(F,Symbol.hasInstance,{value:function(o){
            try{return !!o&&(F.prototype.isPrototypeOf(o)||!!duck(o))}catch(e){return false}
          }})}catch(e){}
      try{window[name]=F}catch(e){}
    }
    mk('MediaError',function(o){return typeof o.code==='number'},function(c){this.code=Number(c)||0;this.message=''});
    mk('TimeRanges',function(o){return typeof o.length==='number'&&typeof o.start==='function'&&typeof o.end==='function'});
    mk('VideoPlaybackQuality',function(o){return typeof o.creationTime==='number'},
      function(){this.creationTime=0;this.totalVideoFrames=0;this.droppedVideoFrames=0;this.corruptedVideoFrames=0});
    // 站点常写 err.code===MediaError.MEDIA_ERR_DECODE 这种比较，常量缺了会当场抛 TypeError。
    // 桥自己报的错误码是 3（解码失败，见 installOverrides 里的 error），与这里对得上。
    try{
      var ME=window.MediaError;
      if(ME){ME.MEDIA_ERR_ABORTED=1;ME.MEDIA_ERR_NETWORK=2;ME.MEDIA_ERR_DECODE=3;ME.MEDIA_ERR_SRC_NOT_SUPPORTED=4}
    }catch(e){}
  }catch(e){window.__nmbMediaApiInstalled=false}
})();
)NMBHOOK"
