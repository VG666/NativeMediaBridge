/**
 * @file nmb_inject.cpp
 * @brief 页面注入：4 段注入脚本常量（真源 hook/js，经 hook/inc 引入）、onReady/onScriptContext 注入时机、注入相关环境开关与兼容垫片。
 *
 * 由 native_media_bridge.cpp 按职责拆分（P1-P5 重构）；逻辑未改。
 * 内部实现一律在 namespace nmb，跨模块接口集中声明于 nmb_internal.h。
 */
#include "core/nmb_internal.h"

namespace nmb {
bool holePunchDisabled();
const char* injectionPrefix();
bool cryptoShimDisabled();
std::string compatShimMode();
void injectCompatShim(WebView view, Frame frame);

// ── §6 注入到页面主世界的脚本（真源在 hook/js/，这里只 #include hook/inc/ 生成物）──
// 四个脚本的分工：
//   hook/js/injection.js      → kInjection（主脚本，约 64KB）：
//     1) 引导：__nmbInstalled 防重入、序号、异常兜底；
//     2) Fullscreen API polyfill（页面内全屏）；
//     3) 主控：状态对象、事件派发、原生控件条 buildBar/syncBar、媒体收养
//        adopt/hook/scan、rAF 周期 syncAll、subtree MutationObserver 发现新媒体；
//     4) 属性覆写：play/pause/seek/currentTime/src 等劫持到 mbQuery，
//        isPrimaryMedia 主媒体判定、挖洞(hole)收集；CSS 克隆等收尾。
//   hook/js/media_api_shim.js → kMediaApiShim：补 MediaError/VideoPlaybackQuality 等
//     被内核砍掉的接口名（让 instanceof 成立），默认随主脚本注入；
//   hook/js/compat_shim.js    → kCompatShim：chrome/ios/safari/mse 能力伪装与 MSE
//     假对象，默认不注入；NMB_COMPAT_SHIM 显式启用，NMB_NO_COMPAT_SHIM=1 可诊断关闭；
//   hook/js/crypto_shim.js    → kCryptoShim：SubtleCrypto 未实现分支的兜底垫片，
//     NMB_NO_CRYPTO_SHIM=1 可关。
// 注入时机：onScriptContext（脚本上下文刚建立，可能 DOM 还没节点），并在
// onReady/documentReady 再补一次（脚本幂等，重复注入无害）。
//
// 怎么改：只编辑 hook\js 下的 .js，然后跑 _nmb_gen_hooks_inc.py 重新生成 hook\inc 下的 .inc
// （根目录 build-zig.bat/build-cl.bat 编译前会自动跑）。生成器把 js 按 ≤16000 字节
// 在换行处切成相邻 R"NMBHOOK(...)NMBHOOK"，规避 MSVC 单字面量 16380 字节的
// C2026 静默截断；inc 是生成物，不要手改。js 里写 JS 注释，别写 C 注释。
// 改完可用根目录 check_inject_syntax.py（node --check 四个 js）复核语法。
const char kInjection[] =
// 真源 hook/js/injection.js；本 inc 由 _nmb_gen_hooks_inc.py 生成，勿手改。
#include "hook/inc/injection.inc"
;

// 内核把媒体侧的 DOM 接口砍了一批：关掉桥的注入实测（tests\testjs\_probe_api.html，mb108）——
//   MediaError=undefined、VideoPlaybackQuality=undefined ← 真缺，这里补；
//   TimeRanges=**function**（原生就有）、TextTrack/TextTrackList=function ← 不该补，mk 会跳过；
//   还缺 AudioTrack/VideoTrack/MediaStream/MediaSource/SourceBuffer（后两个由 kCompatShim 的 mse 模式管，
//   前三个没人拿它做 instanceof，不补）。
// 缺名字的后果不是"判成 false"而是当场抛 "Right-hand side of 'instanceof' is not an object"：
// 站点自己的流程被一句话打断（错误处理分支进不去、初始化中途抛异常），
// HTMLVideoElement 与 SourceBuffer 已经各踩过一次（补法见 kCompatShim 里的 DIF 与 MediaSource）。
// **不受 NMB_COMPAT_SHIM 影响**：这里补的是接口名，不是能力声明，站点能不能播仍只由 canPlayType 说了算，
// 所以开着它不会像 mse 模式那样把站点引到别的通路上。
// 两种让 instanceof 成立的手法都在用，别混淆：
//   · 这里给构造器挂 Symbol.hasInstance + 鸭子类型（实测本内核认 Symbol.hasInstance），
//     管的是"别人手上已经有了的、形状像的对象"（站点自己 new 出来的、或从别处拿到的）；
//   · kInjection 的 asIf 给**桥自己造**的替身（el.error / el.buffered）挂内核原型，管的是自产对象。
// 注意别用"带注入"的量法去判断内核缺什么：这段自己会把缺的名字建出来，量到的"存在"里有它自己的份
// （2026-09-14 就因此把 MediaError 误判成内核自带、差点把这段当死代码删掉）。
const char kMediaApiShim[] =
// 真源 hook/js/media_api_shim.js；本 inc 由 _nmb_gen_hooks_inc.py 生成，勿手改。
#include "hook/inc/media_api_shim.inc"
;

// 兼容垫片：内核对外宣称"什么媒体都不能播"（canPlayType 一律空串、window.MediaSource 不存在），
// 用 hls.js 的站点（腾讯视频等）在能力探测阶段就放弃，连 <video> 都不建，桥自然收不到任何 open。
// 这段按模式改口，默认不注入；NMB_COMPAT_SHIM 显式启用，NMB_NO_COMPAT_SHIM=1 可关闭：
//   ios → 报成"支持原生 HLS 的 iPhone Safari"。这类站点会走 <video src="…m3u8"> 的原生路，
//         桥拿到的就是一个普通 URL，而本项目的 ffmpeg 正好编了 hls demuxer，可以直接开；
//   mse → 补一套假 MediaSource/SourceBuffer，骗过 hls.js 的 isSupported() 与站点自检，
//         看它是否真的去拉 m3u8/分片（分片无法在本内核解码，所以这一路要靠抓 URL 转交 ffmpeg）。
// 两种模式都会把页面请求的媒体 URL 记进 window.__nmbCapture，供宿主侧回读（见 diag 脚本）。
//
// 2026-09-14 在腾讯视频移动播放页（m.v.qq.com/x/m/play?cid=…&vid=…&mobile）上的实测结论：
//   ios  → 可用。站点把真实 HLS 清单交给 <video src>（…f321002.ts.m3u8），桥用 ffmpeg 的 hls demuxer
//          直接开，连续 63 秒按**实时 1.00×** 推进（墙钟 63.1s / 媒体 63.1s），且站点自己就开播（不用点击）。
//          站点是"先播一段 15 秒的 .f2.mp4 头，再切到 205.6 秒的 m3u8 全集"，换源由本桥自动接管。
//   mse  → 陷阱，别默认开。站点一旦认定支持 MSE，长片就会走 MediaSource 自喂（元素 src 变成 blob:），
//          而本桥对 blob:/mediasource:/data: 是明确不碰的（sealed → unhook，见 sourceOf/servable），
//          表现就是"头段 15 秒播完就停"。mse 模式只留着做能力对照与取证，不要用它跑真实播放。
// 所以真实播放用 ios（或单独用 ios；ios+mse 会让站点选 MSE 那条不归本桥管的路）。
const char kCompatShim[] =
// 真源 hook/js/compat_shim.js；本 inc 由 _nmb_gen_hooks_inc.py 生成，勿手改。
#include "hook/inc/compat_shim.inc"
;

// 内核的 WebCrypto 有几条分支是"没实现"的，页面一碰就出事：
//   digest、importKey('pkcs8')、importKey('spki') 永远不落定——站点卡在 await 上（表现就是白屏、
//   点不动），日志里还会被 "xxx not impl" 刷屏；
//   importKey('jwk')、exportKey('jwk')、exportKey('spki') 更直接，调用的一瞬间宿主进程就没了。
// 这段在页面侧接管这几条分支：摘要改用 JS 自己算，内核不支持的导入/导出格式立刻以 NotSupportedError
// 拒绝。站点于是能走自己的降级或报错分支，而不是卡死或把进程带走。
// 只拦这几条，其余（getRandomValues、HMAC 的 raw 导入与签名、generateKey）内核本身是好的，原样透传。
// 诊断开关：设了 NMB_NO_CRYPTO_SHIM=1 就不注入，用来对照问题是不是出在这里。
const char kCryptoShim[] =
// 真源 hook/js/crypto_shim.js；本 inc 由 _nmb_gen_hooks_inc.py 生成，勿手改。
#include "hook/inc/crypto_shim.inc"
;
void __cdecl onReady(WebView view, void* param, Frame frame) {
    auto* browser = static_cast<Browser*>(param);
    if (!browser || browser->view != view) return;
    // 旧页面的媒体释放已经挪到 onScriptContext（新文档的脚本上下文一建立就做，原因见那里）。
    // 只有在拿不到那个时机的内核上才在这里兜底：mbOnDocumentReady 在页面挂着远程媒体时会晚到
    // （mb108 上实测晚到媒体已经接管之后），放这里会把刚建好的条目整表清掉。
    const bool mainFrame = !g_kernel.isMainFrame || g_kernel.isMainFrame(view, frame);
    if (mainFrame && !g_kernel.didCreateScriptContext) releaseAllMedia(browser);
    // 宿主脚本必须先排入当前脚本上下文：nw 应用在桥注入脚本执行期间就可能开始运行，
    // 若把宿主回调放在约 64KB 的媒体脚本之后，window.require/nw 会在首屏脚本运行时仍不存在。
    // 宿主注入自身幂等；桥侧兼容垫片和媒体脚本随后再排入，不改变桥功能。
    if (hostHooks().onDocumentReady)
        hostHooks().onDocumentReady(view, hostHooks().param, frame);
    // 兼容垫片要抢在页面自己的脚本之前改口：站点一读 canPlayType 就会决定要不要建 <video>。
    injectCompatShim(view, frame);
    // mbRunJs 的签名比 wkeRunJsByFrame 多三个参数（是否闭包执行、结果回调及其参数）。
    if (!mediaBridgeDisabled()) g_kernel.runJsByFrame(view, frame, (std::string(injectionPrefix()) + kInjection).c_str(), 0, nullptr, nullptr, nullptr);
    if (!mediaBridgeDisabled()) g_kernel.runJsByFrame(view, frame, kMediaApiShim, 0, nullptr, nullptr, nullptr);
    if (!cryptoShimDisabled()) g_kernel.runJsByFrame(view, frame, kCryptoShim, 0, nullptr, nullptr, nullptr);
}

// 脚本上下文刚建立就注入，不依赖"页面资源全部加载完"。
// 这是唯一不受流式媒体拖累的时机：mbOnDocumentReady 在页面还挂着待加载资源时可能一直不来，
// 而真实站点几乎总有慢资源，注入落不下去就等于整个媒体桥对页面不可见。
// worldId 非 0 的是内核给扩展/隔离世界建的上下文，注入到那里对页面没有意义，跳过。
void __cdecl onScriptContext(WebView view, void* param, Frame frame, void* context, int extensionGroup, int worldId) {
    (void)context; (void)extensionGroup;
    auto* browser = static_cast<Browser*>(param);
    if (!browser || browser->view != view) return;
    if (worldId != 0) return;
    // 新文档的脚本上下文一建立，就说明上一个文档已经作废：它的媒体必须在这里全部释放。
    // 放 onReady（mbOnDocumentReady / blink 线程版）是不行的：页面挂着远程媒体时那个回调会晚到，
    // mb108 上实测晚到"新文档的媒体已经 open 成功"之后，于是把刚建好的条目整表清掉——
    // 表现就是帧率 60、控件条正常，但之后每条 state 都回 {"ok":false,"missing":1}、时长永远是 0。
    // 这个时机在页面脚本之前，新文档自己还一个媒体都没有，清表是安全的。
    if (!g_kernel.isMainFrame || g_kernel.isMainFrame(view, frame)) releaseAllMedia(browser);
    // 宿主脚本要先于媒体桥脚本进入新上下文，否则页面首屏会先看到一个没有 nw/require 的环境。
    if (hostHooks().onScriptContext)
        hostHooks().onScriptContext(view, hostHooks().param, frame, context,
                                    extensionGroup, worldId);
    // 同上：能力探测必须赶在站点脚本前面改口。
    injectCompatShim(view, frame);
    if (!mediaBridgeDisabled()) g_kernel.runJsByFrame(view, frame, (std::string(injectionPrefix()) + kInjection).c_str(), 0, nullptr, nullptr, nullptr);
    if (!mediaBridgeDisabled()) g_kernel.runJsByFrame(view, frame, kMediaApiShim, 0, nullptr, nullptr, nullptr);
    if (!cryptoShimDisabled()) g_kernel.runJsByFrame(view, frame, kCryptoShim, 0, nullptr, nullptr, nullptr);
}
// 诊断开关：设了 NMB_NO_MEDIA_BRIDGE=1 就完全不注入媒体桥，
// 用来对照"页面卡顿是桥造成的，还是内核/站点本身的"。
bool mediaBridgeDisabled() {
    static const bool disabled = [] {
        char buffer[8]{};
        return GetEnvironmentVariableA("NMB_NO_MEDIA_BRIDGE", buffer, sizeof(buffer)) > 0;
    }();
    return disabled;
}

// 诊断开关：设 NMB_NO_HOLES=1 就停掉页面侧的覆盖物采集（collectHoles 恒返回空串），
// 合成退化为"视频帧盖住一切"。用来对照崩溃/卡顿是不是高频 elementsFromPoint 引起的。
bool holePunchDisabled() {
    static const bool disabled = [] {
        char buffer[8]{};
        return GetEnvironmentVariableA("NMB_NO_HOLES", buffer, sizeof(buffer)) > 0;
    }();
    return disabled;
}

// 按诊断开关拼注入前缀：目前只负责把 NMB_NO_HOLES 传进页面。
const char* injectionPrefix() {
    return holePunchDisabled() ? "window.__nmbNoHoles=1;" : "";
}
// 用来对照"页面卡死/进程消失到底是内核的加密分支造成的，还是别的原因"。
bool cryptoShimDisabled() {
    static const bool disabled = [] {
        char buffer[8]{};
        return GetEnvironmentVariableA("NMB_NO_CRYPTO_SHIM", buffer, sizeof(buffer)) > 0;
    }();
    return disabled;
}
// 默认只补通用桌面媒体能力，不改 UA；其余模式仅供显式诊断。
// NMB_COMPAT_SHIM 可显式改为 safari|ios|mse|ios+mse；NMB_NO_COMPAT_SHIM=1 可关闭。
std::string compatShimMode() {
    static const std::string mode = [] {
        char disabled[8]{};
        if (GetEnvironmentVariableA("NMB_NO_COMPAT_SHIM", disabled, sizeof(disabled)) > 0) return std::string();
        char buffer[16]{};
        if (GetEnvironmentVariableA("NMB_COMPAT_SHIM", buffer, sizeof(buffer)) == 0) return std::string();
        std::string text(buffer);
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return text;
    }();
    return mode;
}

void injectCompatShim(WebView view, Frame frame) {
    const std::string mode = compatShimMode();
    if (mode.empty()) return;
    const std::string code = "window.__nmbCompatMode='" + mode + "';" + kCompatShim;
    g_kernel.runJsByFrame(view, frame, code.c_str(), 0, nullptr, nullptr, nullptr);
}

} // namespace nmb
