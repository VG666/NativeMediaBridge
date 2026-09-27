#pragma once
// nmb_internal.h —— 模块间共享的唯一内部头：公共/第三方头、类型别名、MbApi、
// 核心数据结构（nmb_types.h）、跨模块全局与函数声明。只有 NativeMediaBridge 自己的 .cpp 包含它。
// 对外 C ABI 见 native_media_bridge.h（NMB_* 导出），内部实现一律在 namespace nmb。

#include "api/native_media_bridge.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <mmsystem.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nmb {
using WebView = void*;
using Frame = void*;
using Exec = void*;
using JsValue = int64_t;
using DocumentReadyCallback = void(__cdecl*)(WebView, void*, Frame);
using PaintUpdatedCallback = void(__cdecl*)(WebView, void*, HDC, int, int, int, int);
// mbRunJs 不再把执行结果当返回值给出来，而是通过这个回调交回（本模块只用来注入脚本，不用结果）。
using RunJsCallback = void(__cdecl*)(WebView, void*, Exec, JsValue);
// 页面调用 window.mbQuery(请求号, 请求, 回调) 时进入这里，native 用 mbResponseQuery 应答。
using JsQueryCallback = void(__cdecl*)(WebView, void*, Exec, int64_t, int, const char*);
// 页面发起的每个网络请求都会进这里（wke 老约定：返回 true = 本请求由回调接管，内核不再自己加载）。
// 桥用它拦下内核对媒体字节的下载——内核没有解码器，下载纯属有害（<audio> 拉大文件会把页面搞死）。
using LoadUrlBeginCallback = bool(__cdecl*)(WebView, void*, const char*, void*);
// 脚本上下文建立时进入这里；最后两个参数是扩展分组和世界号（0 表示页面主世界）。
using ScriptContextCallback = void(__cdecl*)(WebView, void*, Frame, void*, int, int);

// 本模块用到的内核接口。
// 内核从 2020 版升级到 miniblink 132 后，整套 C 接口由 wke* 改名成 mb*，字符串也从宽字符
// 变成 UTF-8，所以这里保留原来的字段名（几十处调用点因此不用动），只把底层换成 mb*：
// 签名对不上的（键盘事件多一个参数、初始化要传设置、加载页面要 UTF-8）用下面几个包装处理。
struct MbApi {
    HMODULE module{};
    void* (__cdecl* createInitSettings)(){};
    void (__cdecl* setInitSettings)(void* settings, const char* name, const char* value){};
    void (__cdecl* init)(void* settings){};
    void (__cdecl* setDebugConfig)(WebView, const char*, const char*){};
    void (__cdecl* setNodeJsEnable)(WebView, int){};
    WebView (__cdecl* createWebView)(){};
    WebView (__cdecl* createWebWindow)(int, HWND, int, int, int, int){};
    void (__cdecl* setHeadlessEnabled)(WebView, int){};
    void (__cdecl* destroyWebView)(WebView){};
    void (__cdecl* resize)(WebView, int, int){};
    void (__cdecl* showWindow)(WebView, int){};
    void (__cdecl* setHandle)(WebView, HWND){};
    void (__cdecl* setHandleOffset)(WebView, int, int){};
    void (__cdecl* setTransparent)(WebView, int){};
    // 桌面 B 站要求携带桌面 Chrome UA，UA 必须在首次导航前设好；宿主经此字段调 mbSetUserAgent。
    void (__cdecl* setUserAgent)(WebView, const char*){};
    // 离屏绘制：mb 接口要求成对调用（取 DC 后必须解锁），见 composite。
    HDC (__cdecl* getViewDC)(WebView){};
    void (__cdecl* unlockViewDC)(WebView){};
    void (__cdecl* onPaintUpdated)(WebView, PaintUpdatedCallback, void*){};
    // "不让内核自己往窗口上画"的正规开关，mb132 才有：mb108 取不到（它同时也没有
    // mbSetHeadlessEnabled），所以按可选加载、调用点判空，mb108 上靠"不给宿主窗口"达到同一目的，
    // 见 kernelHostWindowEnabled。画面由本模块合成（页面 + 原生视频帧）。
    void (__cdecl* setAutoDrawToHwnd)(WebView, int){};
    int (__cdecl* fireMouseEvent)(WebView, unsigned, int, int, unsigned){};
    int (__cdecl* fireMouseWheelEvent)(WebView, int, int, int, unsigned){};
    // mb 版本的键盘事件比 wke 多一个"是否系统键"参数。
    int (__cdecl* fireKeyDownEvent)(WebView, unsigned, unsigned, int){};
    int (__cdecl* fireKeyUpEvent)(WebView, unsigned, unsigned, int){};
    int (__cdecl* fireKeyPressEvent)(WebView, unsigned, unsigned, int){};
    int (__cdecl* fireWindowsMessage)(WebView, HWND, unsigned, WPARAM, LPARAM, LRESULT*){};
    void (__cdecl* setFocus)(WebView){};
    void (__cdecl* killFocus)(WebView){};
    bool (__cdecl* isMainFrame)(WebView, Frame){};
    void (__cdecl* setNavigationToNewWindowEnable)(WebView, int){};
    void (__cdecl* setCspCheckEnable)(WebView, int){};
    // 内核不再有 wkeLoadURLW/wkeLoadHTMLW 这种宽字符版本，只有 UTF-8 版本，
    // 这两个成员指向下面的包装函数，调用点照旧传宽字符串。
    void (__cdecl* loadURLW)(WebView, const wchar_t*){};
    void (__cdecl* loadHTMLW)(WebView, const wchar_t*){};
    void (__cdecl* mbLoadURL)(WebView, const char*){};
    void (__cdecl* mbLoadHtmlWithBaseUrl)(WebView, const char*, const char*){};
    void (__cdecl* onDocumentReady2)(WebView, DocumentReadyCallback, void*) {};
    // 同上，但回调在 blink 线程上触发。页面挂着待加载资源时 mbOnDocumentReady 可能一直不来，
    // 这条是备用的注入时机；注入脚本自己幂等（见 kInjection 开头的 __nmbInstalled），重复注入无害。
    void (__cdecl* onDocumentReadyInBlinkThread)(WebView, DocumentReadyCallback, void*) {};
    // 脚本上下文一建立就触发，是这里最早的注入时机；页面挂着没加载完的资源时它照样会来。
    void (__cdecl* didCreateScriptContext)(WebView, ScriptContextCallback, void*) {};
    void (__cdecl* wake)(WebView){};
    // 内核不再提供 wkeSetDirty/wkeRepaintIfNeeded，标脏与唤醒统一由 mbWake 完成。
    void (__cdecl* runJsByFrame)(WebView, Frame, const char*, int, RunJsCallback, void*, void*){};
    // UI 线程上随时取主 frame：鼠标移动合流（WM_MOUSEMOVE 没有 Frame 上下文）要靠它注入 hover 调用。
    Frame (__cdecl* getMainFrame)(WebView){};
    // 档3：读 document.title 做 JS→native 的低成本 cursor 回传（mbRunJs 同步写入、mbGetTitle 读）。
    const char* (__cdecl* getTitle)(WebView){};
    // mb108 的 mbRunJs 回调拿不到可用的 Exec（实测 es==nullptr、JsValue 是个小整数句柄），
    // 所以"合成前同步问页面要矩形"这条路走不通；滚动滞后改由页面上报滚动速度来补偿。
    // 页面 → native 的唯一通道（内核不再提供 wkeJsBindFunction 那套同步绑定）。
    void (__cdecl* onJsQuery)(WebView, JsQueryCallback, void*){};
    void (__cdecl* responseQuery)(WebView, int64_t, int, const char*){};
    // DevTools 调试器窗口（wke 时代接口）：mb108 删掉了导出，miniblink_x64.dll（2023 双名版）有。
    // path＝前端资源路径（发行包 front_end\inspector.html，宽字符）；callback/param 本桥用不上，恒空。
    void (__cdecl* showDevtools)(WebView, const wchar_t*, void*, void*){};
    // 拦截内核的媒体下载（见 mediaLoadUrlBegin）：可选接口，调用点逐处判空。
    void (__cdecl* onLoadUrlBegin)(WebView, LoadUrlBeginCallback, void*){};
    // 接管请求后往 job 里写应答体：给空体，内核的媒体元素走"加载失败"的正常分支。
    bool (__cdecl* netSetData)(void*, void*, int){};
};

extern MbApi g_kernel;

// ── 宿主钩子（NMB_SetHostHooks；ABI 契约见 native_media_bridge.h）──────────
// 桥占用 onDocumentReady/onScriptContext/onJsQuery/onPaintUpdated 四个内核单槽，
// nw 宿主这类"自己也要注入脚本、自己也要收页面 RPC"的宿主经这组钩子挂回来。
// onJsQuery 只转发首字符为 '{' 的 JSON 请求（桥的媒体协议是制表符字段，见
// nmb_protocol.cpp 的分流注释），返回 1 表示宿主已应答。
struct HostHooks {
    void* param{};
    void (__cdecl* onDocumentReady)(void* view, void* param, void* frame){};
    void (__cdecl* onScriptContext)(void* view, void* param, void* frame, void* context,
                                    int extensionGroup, int worldId){};
    int (__cdecl* onJsQuery)(void* view, void* param, void* execState, int64_t queryId,
                             int customMsg, const char* request){};
};

} // namespace nmb

// 核心运行时数据结构（FfmpegAudio/FfmpegVideo/Media/Browser）。
#include "core/nmb_types.h"

namespace nmb {

// ── 跨模块全局（定义在 nmb_util.cpp；g_error 只经 setError/clearError/lastErrorText 访问）──
extern std::mutex g_mutex;
extern std::vector<Browser*> g_browsers;
extern HMODULE g_self;

// ── nmb_util.cpp ──
void setError(const std::wstring& value);
void clearError();
const wchar_t* lastErrorText();
HostHooks& hostHooks();
std::wstring windowsError(DWORD code);
std::wstring moduleDirectory();
bool isAbsolutePath(const std::wstring& path);
// 内核的 node（nodeblink）会把宿主进程命令行当成 node 入口脚本去 require，
// 于是 --nwapp=... / -u / xxx.py 之类非脚本参数会让 node bootstrap 直接致命退出
// （表现就是窗口一闪就没）。设 NMB_NO_NODEJS=1 关掉它。详见长记忆。
bool nodeJsDisabled();
HMODULE loadDependency(const wchar_t* requested, const wchar_t* fallbackName, std::wstring& attempted);
HMODULE loadSiblingDependency(const wchar_t* fileName, std::wstring& attempted);
std::wstring utf8ToWide(const std::string& value);
std::string wideToUtf8(const std::wstring& value);
std::wstring ffmpegError(int code);
std::string ffmpegSource(const std::wstring& source);
std::vector<uint8_t> base64Decode(const std::string& in);
std::vector<std::wstring> splitFields(const std::wstring& request);
std::wstring field(const std::vector<std::wstring>& fields, size_t index);
double number(const std::wstring& text, double fallback = 0);
void ensureTimerResolution();

// ── FFmpeg 网络读的"可中断 + 有时限"（实现见 nmb_media.cpp）──────────────
// 解决的是"源站一挂起，整条链路就钉死"：原来从 avformat_open_input 到每一次 av_read_frame
// 都是裸阻塞调用，既没有中断回调也没有读超时。现场表现就是进度恒 0、一帧不出、
// 缓冲相关字段同时冻结十几秒，且关页面时 join 也回不来。
int interruptIo(void* opaque);
void armIo(IoDeadline& guard, Media* media, int milliseconds);
void disarmIo(IoDeadline& guard);
void applyIoOptions(AVDictionary** options);
void attachInterruptCallback(AVFormatContext* context, IoDeadline* guard, Media* media);
// 中断回调触发（AVERROR_EXIT）或协议层读超时（ETIMEDOUT）：这不是"解码失败"，
// 而是"这次没拿到数据"。调用方按"重试/放弃本轮"处理，不要判死。
bool isIoInterrupted(int result);

// 绑定一个内核导出到 MbApi 字段；缺失时写错误并返回 false（nmb_kernel.cpp 用）。
template<class T> bool loadProc(HMODULE module, const char* name, T& target) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!target) setError(L"缺少 DLL 导出: " + utf8ToWide(name));
    return target != nullptr;
}

// ── nmb_kernel.cpp ──
bool initializeKernel(const wchar_t* miniblinkDll);
// 卸载内核（NMB_Shutdown 调用）：经兼容层时由兼容层负责释放真内核，否则直连释放。
void unloadKernel();

// ── nmb_media.cpp / nmb_audio.cpp / nmb_video.cpp ──
void closeFfmpegVideo(FfmpegVideo& video);
void closeFfmpegAudio(FfmpegAudio& audio);
// 诊断（NMB_SYNC_LOG 未设时空转，实现见 nmb_video.cpp 的 syncLogPath）：
// 记一行音频侧状态。为什么音频那边也要记：视频的 drift 只说明"画面在等声音"，
// 等的原因却在声音这边——是没包、没空闲缓冲、还是在等 buffer 水位，看视频日志分不出来。
void noteSyncAudio(const char* event, double writtenUntil, double clock, double origin,
                   int pending, size_t queueDepth, int playing);

void releaseMedia(Media* media);
void releaseMediaAsync(Media* media);
void silenceBrowser(Browser* browser);
void notifyComposite(Browser* browser);
// 打开唯一的那条连接（分配/打开上下文、找齐两条流、启动 demuxLoop）。幂等：
// openVideo 与 openAudio 都先调它，谁先来谁开，另一方直接复用——一个媒体只开一条连接。
bool openInput(Media* media);
// 后台打开（open 与通用 MSE 接管共用）：起 demux+解码线程并收尾写 openOk。
void openMediaAsync(Media* media, WebView view, HWND hwnd);
void decodeLoop(Media* media, WebView view, HWND hwnd);
void audioLoop(Media* media);
// 解复用线程：在唯一连接上读包，按流号分进 Media::videoQueue / audioQueue，
// 并统一负责缓冲水位、读超时收口与位置调整（seek）。
void demuxLoop(Media* media);
bool openVideo(Media* media, WebView view, HWND hwnd);
bool openAudio(Media* media);
void releaseAllMedia(Browser* browser);
std::wstring numberToWide(double value);
double mediaDuration(Media* media);
double mediaPosition(Media* media);
std::wstring mediaStateJson(Media* media);

// ── nmb_inject.cpp ──
void __cdecl onReady(WebView view, void* param, Frame frame);
void __cdecl onScriptContext(WebView view, void* param, Frame frame, void* context, int extensionGroup, int worldId);
bool mediaBridgeDisabled();

// ── nmb_protocol.cpp ──
// 一次写齐 rect/barRect/clipRect（挖洞直接用上报的 barRect，画面裁剪直接用上报的 clipRect，
// 见 Media::barRect / Media::clipRect 的注释）。
void setMediaRects(Media* media, const RectI& rect, const RectI& barRect, const RectI& clipRect);
bool __cdecl mediaLoadUrlBegin(WebView view, void* param, const char* url, void* job);
void __cdecl mediaBridge(WebView view, void* param, Exec, int64_t queryId, int customMsg, const char* request);

// ── nmb_composite.cpp ──
void paintBrowser(Browser* browser, HDC dc, const RECT& target);
void __cdecl onWebViewPainted(WebView, void* param, HDC, int x, int y, int cx, int cy);

// ── nmb_window.cpp ──
bool kernelHostWindowEnabled();
HWND createHostWindow(HWND parent, int x, int y, int width, int height);
HWND createKernelWindow(int width, int height);
void destroyBrowser(Browser* browser);

} // namespace nmb
