(function(){
  if(window.__nmbCryptoShim)return;
  try{
    var C=window.crypto;if(!C)return;
    var S=C.subtle;if(!S||typeof S.importKey!=='function')return;
    // 原生方法必须先抓在手里：补丁是装在原型上的，装完再从 S 上读就只会读到补丁本身（无限递归）。
    var OI=S.importKey,OE=S.exportKey;
    function rol(x,n){return (x<<n)|(x>>>(32-n))}
    function bytesOf(d){
      if(d&&typeof ArrayBuffer!=='undefined'&&d instanceof ArrayBuffer)return new Uint8Array(d);
      try{if(ArrayBuffer.isView&&ArrayBuffer.isView(d))return new Uint8Array(d.buffer,d.byteOffset,d.byteLength)}catch(e){}
      return null;
    }
    // 长度域按规范补成 64 位大端位长。网页里不会有超过 2GB 的输入，但还是照规范写全，不留暗坑。
    function pad(m){
      var l=m.length,k=((l+9+63)&~63),b=new Uint8Array(k);
      b.set(m);b[l]=0x80;
      var hi=Math.floor(l/536870912),lo=(l*8)>>>0;
      b[k-8]=(hi>>>24)&255;b[k-7]=(hi>>>16)&255;b[k-6]=(hi>>>8)&255;b[k-5]=hi&255;
      b[k-4]=(lo>>>24)&255;b[k-3]=(lo>>>16)&255;b[k-2]=(lo>>>8)&255;b[k-1]=lo&255;
      return b;
    }
    function sha1(m){
      var b=pad(m),w=new Int32Array(80);
      var H=[1732584193,-271733879,-1732584194,271733878,-1009589776];
      for(var i=0;i<b.length;i+=64){
        for(var t=0;t<16;t++)w[t]=(b[i+t*4]<<24)|(b[i+t*4+1]<<16)|(b[i+t*4+2]<<8)|b[i+t*4+3];
        for(var t=16;t<80;t++)w[t]=rol(w[t-3]^w[t-8]^w[t-14]^w[t-16],1);
        var a=H[0],c=H[1],d=H[2],e=H[3],f=H[4];
        for(var t=0;t<80;t++){
          var g=t<20?((c&d)|(~c&e)):t<40?(c^d^e):t<60?((c&d)|(c&e)|(d&e)):(c^d^e);
          var kk=t<20?1518500249:t<40?1859775393:t<60?-1894007588:-899497514;
          var h=(rol(a,5)+g+f+kk+w[t])|0;
          f=e;e=d;d=rol(c,30);c=a;a=h;
        }
        H[0]=(H[0]+a)|0;H[1]=(H[1]+c)|0;H[2]=(H[2]+d)|0;H[3]=(H[3]+e)|0;H[4]=(H[4]+f)|0;
      }
      var o=new Uint8Array(20);
      for(var i=0;i<5;i++){var v=H[i];o[i*4]=(v>>>24)&255;o[i*4+1]=(v>>>16)&255;o[i*4+2]=(v>>>8)&255;o[i*4+3]=v&255}
      return o;
    }
    function sha256(m){
      var b=pad(m),w=new Int32Array(64);
      var H=[1779033703,-1150833019,1013904242,-1521486534,1359893119,-1694144372,528734635,1541459225];
      var K=[1116352408,1899447441,-1245643825,-373957723,961987163,1508970993,-1841331548,-1424204075,-670586216,310598401,607225278,1426881987,1925078388,-2132889090,-1680079193,-1046744716,-459576895,-272742522,264347078,604807628,770255983,1249150122,1555081692,1996064986,-1740746414,-1473132947,-1341970488,-1084653625,-958395405,-710438585,113926993,338241895,666307205,773529912,1294757372,1396182291,1695183700,1986661051,-2117940946,-1838011259,-1564481375,-1474664885,-1035236496,-949202525,-778901479,-694614492,-200395387,275423344,430227734,506948616,659060556,883997877,958139571,1322822218,1537002063,1747873779,1955562222,2024104815,-2067236844,-1933114872,-1866530822,-1538233109,-1090935817,-965641998];
      for(var i=0;i<b.length;i+=64){
        for(var t=0;t<16;t++)w[t]=(b[i+t*4]<<24)|(b[i+t*4+1]<<16)|(b[i+t*4+2]<<8)|b[i+t*4+3];
        for(var t=16;t<64;t++){
          var x=w[t-15],y=w[t-2];
          var s0=(x>>>7|x<<25)^(x>>>18|x<<14)^(x>>>3);
          var s1=(y>>>17|y<<15)^(y>>>19|y<<13)^(y>>>10);
          w[t]=(w[t-16]+s0+w[t-7]+s1)|0;
        }
        var v0=H[0],v1=H[1],v2=H[2],v3=H[3],v4=H[4],v5=H[5],v6=H[6],v7=H[7];
        for(var t=0;t<64;t++){
          var S1=(v4>>>6|v4<<26)^(v4>>>11|v4<<21)^(v4>>>25|v4<<7);
          var ch=(v4&v5)^(~v4&v6);
          var t1=(v7+S1+ch+K[t]+w[t])|0;
          var S0=(v0>>>2|v0<<30)^(v0>>>13|v0<<19)^(v0>>>22|v0<<10);
          var mj=(v0&v1)^(v0&v2)^(v1&v2);
          var t2=(S0+mj)|0;
          v7=v6;v6=v5;v5=v4;v4=(v3+t1)|0;v3=v2;v2=v1;v1=v0;v0=(t1+t2)|0;
        }
        H[0]=(H[0]+v0)|0;H[1]=(H[1]+v1)|0;H[2]=(H[2]+v2)|0;H[3]=(H[3]+v3)|0;
        H[4]=(H[4]+v4)|0;H[5]=(H[5]+v5)|0;H[6]=(H[6]+v6)|0;H[7]=(H[7]+v7)|0;
      }
      var o=new Uint8Array(32);
      for(var i=0;i<8;i++){var v=H[i];o[i*4]=(v>>>24)&255;o[i*4+1]=(v>>>16)&255;o[i*4+2]=(v>>>8)&255;o[i*4+3]=v&255}
      return o;
    }
    function algName(a){return String((a&&a.name!==undefined)?a.name:(a===undefined||a===null?'':a))}
    function digestOf(alg,data){
      var n=algName(alg).toUpperCase().replace(/[\-_]/g,''),b=bytesOf(data);
      // 不是 BufferSource 时按规范抛 TypeError；SHA-1/256 之外的摘要内核同样没实现（一样卡死），
      // 也一并明确拒绝——站点至少能立刻知道这条路走不通，而不是永远等下去。
      if(!b)throw new TypeError('digest: 第 2 个参数必须是 ArrayBuffer 或 TypedArray');
      if(n==='SHA1')return sha1(b).buffer;
      if(n==='SHA256')return sha256(b).buffer;
      return null;
    }
    function reject(what){
      var e;
      try{e=new DOMException(what+' 未实现','NotSupportedError')}
      catch(x){e=new Error(what+' 未实现');e.name='NotSupportedError'}
      return Promise.reject(e);
    }
    function badFmt(f){f=String(f===undefined||f===null?'':f).toLowerCase();return f==='jwk'||f==='pkcs8'||f==='spki'}
    function defineMethod(t,name,fn){
      try{
        var d=Object.getOwnPropertyDescriptor(t,name);
        if(d&&!d.configurable&&!d.writable)return false;
        Object.defineProperty(t,name,{value:fn,writable:true,configurable:true,enumerable:d?!!d.enumerable:true});
      }catch(e){try{t[name]=fn}catch(e2){return false}}
      return t[name]===fn;
    }
    // 在 target 上装三个补丁。装不上、或装完仍读不到（对象自有属性会盖住原型上的改动）都返回 false，
    // 交给调用方换个地方再装。
    function patch(target,base){
      var ik=function(){var f=arguments[0];if(badFmt(f))return reject('importKey("'+String(f).toLowerCase()+'")');return OI.apply(base,arguments)};
      var ek=function(){var f=arguments[0];if(badFmt(f))return reject('exportKey("'+String(f).toLowerCase()+'")');return OE.apply(base,arguments)};
      // 摘要按规范返回 Promise<ArrayBuffer>：漏了这层包装，站点拿到的就是裸 ArrayBuffer，
      // 一 .then 就抛 TypeError，比卡住还难查。
      var dg=function(){var r=digestOf(arguments[0],arguments[1]);return r?Promise.resolve(r):reject('digest("'+algName(arguments[0])+'")')};
      var fns=[['importKey',ik],['exportKey',ek],['digest',dg]];
      for(var i=0;i<3;i++)if(!defineMethod(target,fns[i][0],fns[i][1]))return false;
      for(var i=0;i<3;i++)if(target[fns[i][0]]!==fns[i][1])return false;
      return true;
    }
    var proto=null;
    try{proto=Object.getPrototypeOf(S)}catch(e){}
    // 先改原型：站点一般是在注入之后才拿到 subtle 的，改原型对它们一律生效。
    var ok=!!(proto&&proto!==Object.prototype&&patch(proto,S)&&S.importKey===proto.importKey);
    // 原型改不动、或内核每次访问都重建对象时，退一步改这一份实例。
    if(!ok)ok=patch(S,S);
    window.__nmbCryptoShim=!!ok;
  }catch(e){}
})();
