// 临时校验脚本：把 native_media_bridge.cpp 里的 kCryptoShim 抠出来，放到 node 里跑一遍。
// 目的有两个：确认这段 JS 语法没问题、摘要算得对（拿 node:crypto 做裁判）；
// 以及确认三种 subtle 挂法（原型上/自有属性/每次新对象）下补丁到底装没装上。
var fs = require('fs'), crypto = require('crypto'), vm = require('vm');
var TAG = 'const char kCryptoShim[] = R"JS(', END = ')JS";';
var src = fs.readFileSync(process.argv[2], 'utf8');
var i = src.indexOf(TAG), j = src.indexOf(END, i);
if (i < 0 || j < 0) throw new Error('源码里没找到 kCryptoShim');
var code = src.slice(i + TAG.length, j);
console.log('提取到 ' + code.length + ' 字符');

function makeBase(calls) {
  return {
    importKey: function (f) { calls.push('importKey:' + f); return Promise.resolve({ type: 'native' }); },
    exportKey: function (f) { calls.push('exportKey:' + f); return Promise.resolve('nativeExport'); },
    digest: function () { calls.push('digest'); return new Promise(function () { }); },
    sign: function () { calls.push('sign'); return Promise.resolve('signed'); }
  };
}
// mode: proto=方法在原型上 / own=方法在对象自己身上 / fresh=每次访问 crypto.subtle 都是新对象
function sandbox(mode) {
  var calls = [], box = {};
  function build() {
    var base = makeBase(calls), s;
    if (mode === 'proto') { function S() { } S.prototype = base; s = new S(); }
    else { s = {}; for (var k in base) s[k] = base[k]; }
    return s;
  }
  if (mode === 'fresh') {
    Object.defineProperty(box, 'subtle', { get: build, configurable: true });
  } else box.subtle = build();
  var win = { crypto: box };
  win.window = win;
  vm.createContext(win);
  vm.runInContext(code, win);
  return { win: win, calls: calls, box: box };
}

function hex(ab) { return Buffer.from(ab).toString('hex'); }

// 有的形态下补丁装不上（subtle 每次访问都返回新对象时改原型/改实例都没用），
// 这时跑的又是内核那个永远不落定的实现，直接 await 会把校验脚本自己挂死——加个超时。
function within(promise, ms) {
  return Promise.race([
    Promise.resolve(promise).then(function (v) { return { ok: true, value: v }; }, function (e) { return { ok: false, error: e }; }),
    new Promise(function (r) { setTimeout(function () { r({ ok: false, error: { name: '无响应', message: '超过 ' + ms + 'ms' } }); }, ms); })
  ]);
}

(async function () {
  var bad = 0;
  function check(name, got, want) {
    var ok = String(got) === String(want);
    // 补丁装在原型/实例上，够不着"每次访问都新建一个 subtle 对象"的形态；真实内核不是这样
    // （探针实测：方法在原型上、每次返回同一对象），所以这组结果只作为已知限制标注，不计失败。
    if (name.indexOf('fresh ') === 0 && !ok) {
      console.log('  已知限制 ' + name + '  实际=' + got);
      return;
    }
    if (!ok) bad++;
    console.log((ok ? '  OK   ' : '  失败 ') + name + (ok ? '' : '  实际=' + got + ' 期望=' + want));
  }
  for (var m of ['proto', 'own', 'fresh']) {
    var sb = sandbox(m);
    console.log('=== subtle 挂法: ' + m + '  补丁标记=' + sb.win.__nmbCryptoShim + ' ===');
    if (m === 'fresh') console.log('  （这种形态补丁够不着，下面只作已知限制记录）');
    var S = sb.win.crypto.subtle;
    // 摘要：拿 node:crypto 的结果当标准答案。
    for (var pair of [['SHA-256', 'sha256'], ['SHA-1', 'sha1']]) {
      var cases = ['', 'abc', 'The quick brown fox jumps over the lazy dog', '中文测试🚀'];
      for (var text of cases) {
        var buf = Buffer.from(text, 'utf8');
        var want = crypto.createHash(pair[1]).update(buf).digest('hex');
        var got;
        // 顺带盯住返回类型：规范要求是 Promise<ArrayBuffer>，直接给 ArrayBuffer 的话
        // 站点一 .then 就抛 TypeError（await 能吞掉这个差异，所以必须单独断言）。
        var raw = S.digest(pair[0], new Uint8Array(buf));
        check(m + ' digest ' + pair[0] + ' 返回 thenable', typeof (raw || {}).then, 'function');
        var res = await within(raw, 500);
        got = res.ok ? hex(res.value) : ('未落定:' + (res.error && res.error.name));
        check(m + ' digest ' + pair[0] + ' "' + text.slice(0, 12) + '"', got, want);
      }
    }
    // 内核没实现的导入/导出格式：必须立刻拒绝，而且绝不能落到内核那条会卡死/崩溃的路上。
    for (var f of ['pkcs8', 'spki', 'jwk']) {
      var r = await S.importKey(f, new Uint8Array([1]), {}).then(function () { return '未拒绝' }, function (e) { return (e && e.name) + ''; });
      check(m + ' importKey ' + f, r, 'NotSupportedError');
    }
    for (var f of ['jwk', 'pkcs8', 'spki']) {
      var r2 = await S.exportKey(f, {}).then(function () { return '未拒绝' }, function (e) { return (e && e.name) + ''; });
      check(m + ' exportKey ' + f, r2, 'NotSupportedError');
    }
    // 内核本来就好的路径必须原样透传，不能被补丁挡住。
    var native = await S.importKey('raw', new Uint8Array([1]), {}).then(function (k) { return k && k.type; });
    check(m + ' importKey raw 透传', native, 'native');
    var sig = await S.sign({ name: 'HMAC' }, {}, new Uint8Array([1]));
    check(m + ' sign 透传', sig, 'signed');
    var ex = await S.exportKey('raw', {});
    check(m + ' exportKey raw 透传', ex, 'nativeExport');
    // 其它摘要算法：内核没实现，拒绝即可（调用方要能立刻知道，而不是永远挂着）。
    var other = await S.digest('SHA-384', new Uint8Array([1])).then(function () { return '未拒绝' }, function (e) { return e && e.name; });
    check(m + ' digest SHA-384 拒绝', other, 'NotSupportedError');
    // 非 BufferSource 输入按规范抛 TypeError。
    var te;
    try { S.digest('SHA-256', 'abc'); te = '未抛'; } catch (e) { te = e && e.name; }
    check(m + ' digest 非 BufferSource', te, 'TypeError');
    console.log('  内核被真正调到的次数: ' + sb.calls.join(', '));
  }
  console.log(bad ? ('\n共 ' + bad + ' 项失败') : '\n全部通过');
  process.exit(bad ? 1 : 0);
})();
