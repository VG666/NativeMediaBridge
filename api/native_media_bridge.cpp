/**
 * @file    native_media_bridge.cpp
 * @brief   NativeMediaBridge 对外 C ABI 的导出层（原 §9）。
 *
 * 2453 行单文件已按职责拆为多个模块（src\ 下按功能域分目录，全部在 namespace nmb）：
 *   core/  nmb_types.h 核心数据结构；nmb_internal.h 跨模块共享头；nmb_util.* 全局对象与工具
 *   kernel/ nmb_kernel.*  mb108 动态加载与函数指针绑定
 *   media/ nmb_audio.* 音频解码与 waveOut 播放；nmb_video.* 视频解码线程；nmb_media.* 生命周期与状态 JSON
 *   page/  nmb_inject.* 注入脚本（hook/js 真源）与注入时机；nmb_protocol.* mbQuery 协议与下载拦截
 *   render/ nmb_composite.* 页面+原生视频帧合成
 *   platform/ nmb_window.* 宿主窗口过程、输入转发与浏览器收尾
 * 本文件（api/）只保留 NMB_* 导出函数与 DllMain；C ABI/导出表不变，宿主与测试零感知。
 */
#include "core/nmb_internal.h"

using namespace nmb;

// ── §9 对外导出 API（契约注释见 native_media_bridge.h）────────────────────
// NMB_Initialize 必须最先调用：实现（找内核、绑定 MbApi、初始化）在 nmb_kernel.cpp。
int NMB_CALL NMB_Initialize(const wchar_t* miniblinkDll, const wchar_t*) {
    return initializeKernel(miniblinkDll) ? 1 : 0;
}
NMB_HANDLE NMB_CALL NMB_CreateBrowser(HWND parent, int x, int y, int width, int height) {
    if (!g_kernel.module) { setError(L"请先调用 NMB_Initialize"); return nullptr; }
    if (!g_kernel.createWebView || !g_kernel.setHandle || !g_kernel.getViewDC) {
        setError(L"当前 miniblink 内核缺少离屏渲染接口 (mbCreateWebView/mbSetHandle/mbGetLockedViewDC)");
        return nullptr;
    }
    if (width < 1) width = 800;
    if (height < 1) height = 600;
    auto* browser = new Browser();
    // 让结构由 shared_ptr 接管：异步 open 的收尾线程会持一份，见 Browser::self 的注释。
    browser->self.reset(browser);

    // 窗口完全由本模块创建和管理，Miniblink 只作为离屏渲染器绑定到该窗口。
    SetLastError(ERROR_SUCCESS);
    browser->hwnd = createHostWindow(parent, x, y, width, height);
    if (!browser->hwnd) {
        const DWORD code = GetLastError();
        browser->self.reset();
        setError(L"创建宿主窗口失败，错误 " + std::to_wstring(code) + L": " + windowsError(code));
        return nullptr;
    }
    browser->view = g_kernel.createWebView();
    // 设 NMB_NO_NODEJS=1 时把 node 关掉：内核 nodeblink 把宿主命令行当入口脚本 require，
    // 非脚本参数（--nwapp= / -u / xxx.py）会让 node bootstrap 致命退出（窗口闪退）。
    if (browser->view && g_kernel.setNodeJsEnable)
        g_kernel.setNodeJsEnable(browser->view, nodeJsDisabled() ? FALSE : TRUE);
    if (!browser->view) {
        DestroyWindow(browser->hwnd);
        delete browser;
        setError(L"创建 miniblink WebView 失败");
        return nullptr;
    }
    SetPropW(browser->hwnd, L"NMB_BROWSER", browser);
    // 最终路径（mb108）：不能不给 HWND（会杀页），也不能绑定可见窗（内核直绘会盖掉合成视频而闪烁）。
    // 给它同尺寸但永不显示的工具窗；真实可见窗仍只由 paintBrowser 从 getViewDC 合成后一次上屏。
    // NMB_KERNEL_HOST_WINDOW=1 仅保留为取证开关：强制绑定可见窗，复现双绘制者闪烁。
    if (!kernelHostWindowEnabled()) {
        browser->kernelHwnd = createKernelWindow(width, height);
        if (!browser->kernelHwnd) {
            const DWORD code = GetLastError();
            g_kernel.destroyWebView(browser->view);
            browser->view = nullptr;
            RemovePropW(browser->hwnd, L"NMB_BROWSER");
            DestroyWindow(browser->hwnd);
            browser->hwnd = nullptr;
            browser->self.reset();
            setError(L"创建内核绘制接收窗口失败，错误 " + std::to_wstring(code) + L": " + windowsError(code));
            return nullptr;
        }
        g_kernel.setHandle(browser->view, browser->kernelHwnd);
        // 标记这扇窗是"内核绑定窗"而非可视宿主：它的窗口过程要用它把
        // CSS -webkit-app-region: drag 触发的 SC_MOVE 转投到可见顶层窗口。
        SetPropW(browser->kernelHwnd, L"NMB_KERNEL", browser);
    } else {
        g_kernel.setHandle(browser->view, browser->hwnd);
    }
    // 有开关的内核（mb132+）本来也要显式关掉一次：交给窗口 ≠ 允许它自绘。
    if (g_kernel.setAutoDrawToHwnd) g_kernel.setAutoDrawToHwnd(browser->view, 0);
    if (g_kernel.setHandleOffset) g_kernel.setHandleOffset(browser->view, 0, 0);
    if (g_kernel.setTransparent) g_kernel.setTransparent(browser->view, false);
    // 页面内 target=_blank 之类的新窗口导航改为在当前视图内完成，避免弹出无人管理的窗口。
    if (g_kernel.setNavigationToNewWindowEnable) g_kernel.setNavigationToNewWindowEnable(browser->view, false);
    // 关闭 CSP 校验，保证注入脚本在任何页面上都能执行（否则跳转后媒体桥会失效）。
    if (g_kernel.setCspCheckEnable) g_kernel.setCspCheckEnable(browser->view, false);
    if (g_kernel.onPaintUpdated) g_kernel.onPaintUpdated(browser->view, onWebViewPainted, browser->hwnd);

    RECT client{};
    GetClientRect(browser->hwnd, &client);
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    if (browser->kernelHwnd) SetWindowPos(browser->kernelHwnd, nullptr, -32000, -32000,
                                           clientWidth, clientHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    g_kernel.resize(browser->view, clientWidth, clientHeight);
    if (g_kernel.showWindow) g_kernel.showWindow(browser->view, true);
    // mbShowWindow 会连带显示 view 绑定的 HWND；隐藏接收窗绝不能出现在 Z 序/命中测试里。
    if (browser->kernelHwnd) {
        ShowWindow(browser->kernelHwnd, SW_HIDE);
        SetWindowPos(browser->kernelHwnd, HWND_BOTTOM, -32000, -32000, clientWidth, clientHeight,
                     SWP_HIDEWINDOW | SWP_NOACTIVATE);
    }

    if (g_kernel.onDocumentReady2) g_kernel.onDocumentReady2(browser->view,onReady,browser);
    // 页面带远程媒体时 mbOnDocumentReady 可能迟迟不来（注入就永远落不下去），
    // blink 线程版是另一个触发点，两个都挂上；onReady 与注入脚本都是幂等的。
    if (g_kernel.onDocumentReadyInBlinkThread) g_kernel.onDocumentReadyInBlinkThread(browser->view,onReady,browser);
    // 真正的保底注入时机：页面还挂着慢资源时上面两条都可能不来，这一条一定会来。
    // 注入脚本自身幂等，重复触发只是多设一次 __nmbInstalled。
    if (g_kernel.didCreateScriptContext) g_kernel.didCreateScriptContext(browser->view, onScriptContext, browser);
    // 页面 → native 的媒体桥：新内核取消了 wkeJsBindFunction 那套同步绑定，
    // 改由页面调用 window.mbQuery(...)，内核回到 mediaBridge，native 再用 mbResponseQuery 应答。
    if (g_kernel.onJsQuery) g_kernel.onJsQuery(browser->view, mediaBridge, browser);
    // 拦截内核的媒体下载（见 mediaLoadUrlBegin）：元素一解析内核就开始拉 src，
    // 所以必须和 onJsQuery 一起、在任何 loadURL 之前挂上。mb108 上实测"挂上即死页"
    // （连无媒体的空白页都在 ~0.2s 抛 0xe06d7363），因此默认不挂，NMB_LUB_HOOK=1 才启用。
    static const bool lubHook = [] {
        char buffer[8]{};
        return GetEnvironmentVariableA("NMB_LUB_HOOK", buffer, sizeof(buffer)) > 0;
    }();
    if (lubHook && g_kernel.onLoadUrlBegin) {
        g_kernel.onLoadUrlBegin(browser->view, mediaLoadUrlBegin, nullptr);
        fprintf(stderr, "[nmb] LUB attached\n");
    }
    { std::lock_guard<std::mutex> lock(g_mutex); g_browsers.push_back(browser); }
    ShowWindow(browser->hwnd, SW_SHOW);
    UpdateWindow(browser->hwnd);
    return browser;
}

void* NMB_CALL NMB_GetWebView(NMB_HANDLE value){auto* b=static_cast<Browser*>(value);return b?b->view:nullptr;}
void NMB_CALL NMB_SetDebugConfig(NMB_HANDLE value, const char* name, const char* configValue) {
    auto* b = static_cast<Browser*>(value);
    if (b && b->view && name && configValue && g_kernel.setDebugConfig)
        g_kernel.setDebugConfig(b->view, name, configValue);
}
HWND NMB_CALL NMB_GetWindow(NMB_HANDLE value){auto* b=static_cast<Browser*>(value);return b?b->hwnd:nullptr;}

int NMB_CALL NMB_LoadURL(NMB_HANDLE value,const wchar_t* url){
    clearError();
    auto*b=static_cast<Browser*>(value);
    if(!b||!url){setError(L"NMB_LoadURL 参数无效");return 0;}
    g_kernel.loadURLW(b->view,url);
    // 新内核没有 wkeSetDirty，标脏+唤醒统一由 mbWake 完成。
    if (g_kernel.wake) g_kernel.wake(b->view);
    if (b->hwnd) InvalidateRect(b->hwnd,nullptr,FALSE);
    return 1;
}
int NMB_CALL NMB_LoadHTML(NMB_HANDLE value,const wchar_t* html){
    clearError();
    auto*b=static_cast<Browser*>(value);
    if(!b||!html){setError(L"NMB_LoadHTML 参数无效");return 0;}
    g_kernel.loadHTMLW(b->view,html);
    // 新内核没有 wkeSetDirty，标脏+唤醒统一由 mbWake 完成。
    if (g_kernel.wake) g_kernel.wake(b->view);
    if (b->hwnd) InvalidateRect(b->hwnd,nullptr,FALSE);
    return 1;
}
void NMB_CALL NMB_Resize(NMB_HANDLE value,int w,int h){
    auto*b=static_cast<Browser*>(value);
    if (!b || w < 1 || h < 1) return;
    if (b->hwnd) {
        if (GetParent(b->hwnd)) {
            SetWindowPos(b->hwnd,nullptr,0,0,w,h,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        } else {
            const DWORD style=static_cast<DWORD>(GetWindowLongPtrW(b->hwnd,GWL_STYLE));
            const DWORD exStyle=static_cast<DWORD>(GetWindowLongPtrW(b->hwnd,GWL_EXSTYLE));
            RECT frame{0,0,w,h};
            AdjustWindowRectEx(&frame,style,FALSE,exStyle);
            SetWindowPos(b->hwnd,nullptr,0,0,frame.right-frame.left,frame.bottom-frame.top,
                         SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        RECT client{};
        GetClientRect(b->hwnd,&client);
        const int clientWidth=client.right-client.left;
        const int clientHeight=client.bottom-client.top;
        if (b->kernelHwnd) SetWindowPos(b->kernelHwnd,nullptr,-32000,-32000,clientWidth,clientHeight,
                                        SWP_NOZORDER|SWP_NOACTIVATE);
        g_kernel.resize(b->view,clientWidth,clientHeight);
        if (g_kernel.wake) g_kernel.wake(b->view);
        InvalidateRect(b->hwnd,nullptr,FALSE);
    } else {
        g_kernel.resize(b->view,w,h);
    }
}
void NMB_CALL NMB_Show(NMB_HANDLE value,int show){
    auto*b=static_cast<Browser*>(value);
    if (!b) return;
    const BOOL visible = show != 0 ? TRUE : FALSE;
    if (b->view && g_kernel.showWindow) g_kernel.showWindow(b->view, visible != FALSE);
    // 内核 show 操作的是绑定窗；它只负责接收直绘，任何时候都不能显示、激活或挡住可见窗。
    if (b->kernelHwnd) {
        ShowWindow(b->kernelHwnd, SW_HIDE);
        SetWindowPos(b->kernelHwnd, HWND_BOTTOM, -32000, -32000, 0, 0,
                     SWP_NOSIZE | SWP_HIDEWINDOW | SWP_NOACTIVATE);
    }
    if (b->hwnd) {
        ShowWindow(b->hwnd, visible ? SW_SHOW : SW_HIDE);
        InvalidateRect(b->hwnd, nullptr, FALSE);
        UpdateWindow(b->hwnd);
    }
}
int NMB_CALL NMB_SetMaximized(NMB_HANDLE value,int maximized){
    auto*b=static_cast<Browser*>(value);
    if (!b || !b->hwnd) return 0;
    if (!GetParent(b->hwnd)) ShowWindow(b->hwnd, maximized ? SW_MAXIMIZE : SW_RESTORE);
    return IsZoomed(b->hwnd) ? 1 : 0;
}
int NMB_CALL NMB_IsMaximized(NMB_HANDLE value){
    auto*b=static_cast<Browser*>(value);
    return (b && b->hwnd && IsZoomed(b->hwnd)) ? 1 : 0;
}
// 打开内核的 DevTools 调试器窗口（wkeShowDevtools，四参 __cdecl）。path＝DevTools 前端资源，
// 指发行包 front_end 目录下 inspector.html 的全路径（宽字符）；传 nullptr 时内核用内置默认，
// 有没有取决于内核版本。callback/param 是窗口事件回调，本桥不需要，恒空。内核只提供"打开"，
// 没有"关闭"语义——调试器窗口由用户自己关。
int NMB_CALL NMB_ShowDevTools(NMB_HANDLE value,const wchar_t* path){
    clearError();
    auto*b=static_cast<Browser*>(value);
    if(!b||!b->view){setError(L"NMB_ShowDevTools 参数无效");return 0;}
    if(!g_kernel.showDevtools){setError(L"当前内核缺少 wkeShowDevtools 导出（仅 miniblink_x64.dll 有）");return 0;}
    g_kernel.showDevtools(b->view,path,nullptr,nullptr);
    return 1;
}
void NMB_CALL NMB_DestroyBrowser(NMB_HANDLE value){destroyBrowser(static_cast<Browser*>(value));}
void NMB_CALL NMB_Shutdown(){std::vector<Browser*> list;{std::lock_guard<std::mutex>lock(g_mutex);list=g_browsers;}for(auto*b:list)destroyBrowser(b);unloadKernel();}
const wchar_t* NMB_CALL NMB_GetLastError(){return lastErrorText();}

// 宿主钩子注册（契约见 native_media_bridge.h）：逐字段拷进全局表，传 NULL 清空。
// 钩子在桥自己的处理之后调用（见 nmb_inject.cpp 的 onReady/onScriptContext 尾部、
// nmb_protocol.cpp 的 mediaBridge 开头），桥自身行为不受影响。
void NMB_CALL NMB_SetHostHooks(const NMB_HostHooks* hooks) {
    HostHooks& target = hostHooks();
    if (!hooks) {
        target = HostHooks();
        return;
    }
    target.param = hooks->param;
    target.onDocumentReady = hooks->onDocumentReady;
    target.onScriptContext = hooks->onScriptContext;
    target.onJsQuery = hooks->onJsQuery;
}

// 媒体下载拦截的显式导出。内核的 onLoadUrlBegin 是 per-view 单槽：nw 宿主要用它做
// 同步 RPC 退路（后挂的会覆盖先挂的），桥模式下宿主在自己的回调里对"非 RPC 请求"
// 链式转发到这里，内核的媒体字节流照旧由 ffmpeg 接管（拦截语义见 nmb_protocol.cpp
// 的 mediaLoadUrlBegin 注释：写空应答体，真解码走 ffmpeg）。
int NMB_CALL NMB_MediaLoadUrlBegin(WebView view, void* param, const char* url, void* job) {
    return mediaLoadUrlBegin(view, param, url, job) ? 1 : 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
