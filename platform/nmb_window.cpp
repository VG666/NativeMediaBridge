/**
 * @file nmb_window.cpp
 * @brief 宿主窗口：browserWindowProc（输入转发/合成消息/焦点/光标）、窗口类注册、可见宿主窗与内核隔离工具窗的创建、destroyBrowser 收尾、内核宿主窗开关。
 *
 * 由 native_media_bridge.cpp 按职责拆分（P1-P5 重构）；逻辑未改。
 * 内部实现一律在 namespace nmb，跨模块接口集中声明于 nmb_internal.h。
 */
#include "core/nmb_internal.h"

namespace nmb {
//
// mb108 的三个已证事实：
//   ① 完全不调 mbSetHandle：无注入、无媒体的空白页约 0.2s 抛 0xe06d7363，随后 UI 死锁；
//   ② mbSetHandle 绑定可见窗：内核直接上屏，与 paintBrowser 双重绘制；洋红标记 37/70 被擦、视频区
//      22/67 露出页面底色，即"不滚也闪，滚动更闪"；
//   ③ GetWindowLongPtr 在 mbSetHandle 前后相同（subclassed=0）：内核不是靠子类化/WM_PAINT 直绘，
//      抢回 WndProc 的方案无效。
// 最终路径：仍给内核合法 HWND，但给的是屏幕外、不可见、同尺寸的工具窗；内核直绘被隔离，真实可见窗
// 只由 paintBrowser 从 getViewDC 合成后一次上屏。NMB_KERNEL_HOST_WINDOW=1 仅用于绑定可见窗、复现旧闪烁。
// A/B 旧证据：F:\ffbuild\p_h1.log（有 HWND 存活）vs p_n2.log（无 HWND 死亡）；绘制证据见 p_flick.log。
bool kernelHostWindowEnabled() {
    static const bool enabled = [] {
        char buffer[8]{};
        return GetEnvironmentVariableA("NMB_KERNEL_HOST_WINDOW", buffer, sizeof(buffer)) > 0;
    }();
    return enabled;
}
unsigned mouseFlags(WPARAM wParam) {
    unsigned flags = 0;
    if (wParam & MK_CONTROL) flags |= 8;
    if (wParam & MK_SHIFT) flags |= 4;
    if (wParam & MK_LBUTTON) flags |= 1;
    if (wParam & MK_MBUTTON) flags |= 16;
    if (wParam & MK_RBUTTON) flags |= 2;
    return flags;
}

unsigned keyFlags(LPARAM lParam) {
    unsigned flags = 0;
    if (lParam & (1 << 30)) flags |= 0x4000;
    if (lParam & (1 << 24)) flags |= 0x0100;
    return flags;
}

void fireBrowserWheel(Browser* browser, WPARAM wParam, LPARAM lParam, bool horizontal) {
    if (!browser || !browser->view || !browser->hwnd || !g_kernel.fireMouseWheelEvent) return;
    // mb108 的 fireMouseWheelEvent 参数语义是页面视口客户区坐标。虽然 WebView 绑定到
    // 屏幕外 kernelHwnd，命中测试仍必须使用可见 browser->hwnd 的本地坐标；若转换到
    // kernelHwnd，会得到约 32000 的越界坐标，DOM wheel 和 overflow 滚动都无法命中。
    POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
    ScreenToClient(browser->hwnd, &point);
    const int delta = static_cast<short>(HIWORD(wParam));
    unsigned flags = mouseFlags(wParam);
    if (horizontal) flags |= 4; // WKE_SHIFT: miniblink wheel API has no axis parameter.
    g_kernel.fireMouseWheelEvent(browser->view, point.x, point.y, delta, flags);
}
// ── §8 宿主窗口过程：输入转发、合成与窗口控制 ────────────────────────────
// 该窗口是宿主唯一可见的表面。关键消息：
//   WM_PAINT      只按 ps.rcPaint 脏区调 paintBrowser 合成（见文件头链路图）；
//   WM_APP+2      “新视频帧到达”的合流消息：有视频时把脏区收窄到视频矩形，
//                 无视频时整窗交给 onWebViewPainted 的脏区；
//   WM_APP+3      mousemove 合流：一轮消息循环只向页面 polyfill 报一次坐标；
//   鼠标/键盘     翻译成 mb* 内核输入事件（离屏内核不收真实窗口消息）；
//   WM_SETCURSOR  按页面上报的 cursorType 设置箭头/手型/光标；
//   WM_CLOSE/DESTROY/SIZE 止声、同步内核表面尺寸与重绘。
LRESULT CALLBACK browserWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* browser = reinterpret_cast<Browser*>(GetPropW(hwnd, L"NMB_BROWSER"));
    if (!browser) return DefWindowProcW(hwnd, message, wParam, lParam);
    WebView view = browser->view;

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        // 我们自己建的顶层窗口：用户点右上角关闭。宿主的消息循环是 GetMessage 驱动，
        // 没人 PostQuitMessage 的话它就一直在转、进程不退出、声音也留着。
        silenceBrowser(browser);
        if (GetParent(hwnd)) break;   // 嵌在宿主窗口里的子控件：关不关由宿主决定，这里只止声
        RemovePropW(hwnd, L"NMB_BROWSER");
        browser->hwnd = nullptr;
        DestroyWindow(hwnd);
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        // 宿主自己销毁窗口（父窗口被关、或直接 DestroyWindow）时也要止声，
        // 否则页面没了，解码线程和声卡里的 PCM 还在往下走。
        silenceBrowser(browser);
        break;
    case WM_SIZE: {
        const int width = LOWORD(lParam);
        const int height = HIWORD(lParam);
        if (view && width > 0 && height > 0) {
            g_kernel.resize(view, width, height);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: {
        // 上屏渲染：内核绑定的就是这扇窗、自己画，这里交给默认过程即可。
        // 曾经的 paintBrowser 离屏合成（getViewDC 拷贝 + 视频贴帧）随绑定窗一起退役。
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    case WM_APP + 2:
        // 帧线程的合成通知：上屏模式下视频帧由页面 canvas 绘制（内部绘制），
        // native 不再上屏任何东西。保留消息入口只为清掉合并标志，什么都不做。
        browser->paintPosted.store(false);
        return 0;
    case WM_APP + 3:
        // hover 合流消息：离屏模式的历史产物（polyfill 补偿）。上屏模式下内核
        // 直接收真实 WM_MOUSEMOVE，hover 由内核原生处理，这里没有触发源、无事可做。
        browser->mousePosted.store(false);
        return 0;
    case WM_SETFOCUS:
        if (view && g_kernel.setFocus && !browser->syncingKernelFocus) {
            // mbSetFocus 会把 Win32 焦点转给 view 绑定的 HWND；上屏模式下绑的就是本窗，
            // 通常不会再跳。保守保留同步逻辑：内核若仍有内部焦点迁移，这里兜回来。
            browser->syncingKernelFocus = true;
            g_kernel.setFocus(view);
            if (GetFocus() != hwnd) SetFocus(hwnd);
            browser->syncingKernelFocus = false;
        }
        return 0;
    case WM_KILLFOCUS:
        if (browser->syncingKernelFocus) return 0;
        if (view && g_kernel.killFocus) g_kernel.killFocus(view);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // mb 版键盘事件比 wke 版多一个"是否系统键"参数（Alt 组合键走这里）。
        if (view && g_kernel.fireKeyDownEvent && g_kernel.fireKeyDownEvent(view, static_cast<unsigned>(wParam), keyFlags(lParam), message == WM_SYSKEYDOWN ? 1 : 0)) return 0;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (view && g_kernel.fireKeyUpEvent && g_kernel.fireKeyUpEvent(view, static_cast<unsigned>(wParam), keyFlags(lParam), message == WM_SYSKEYUP ? 1 : 0)) return 0;
        break;
    case WM_CHAR:
        if (view && g_kernel.fireKeyPressEvent && g_kernel.fireKeyPressEvent(view, static_cast<unsigned>(wParam), keyFlags(lParam), 0)) return 0;
        break;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        SetFocus(hwnd);
        SetCapture(hwnd);
        if (view && g_kernel.fireMouseEvent) g_kernel.fireMouseEvent(view, message, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)), mouseFlags(wParam));
        // mbFireMouseEvent(BUTTONDOWN) 也可能把系统焦点切回绑定的 kernelHwnd；事件已送达后再次
        // 恢复可见窗焦点。用同步标志避免嵌套 WM_SETFOCUS 又调用 mbSetFocus 形成往返。
        if (GetFocus() != hwnd) {
            browser->syncingKernelFocus = true;
            SetFocus(hwnd);
            browser->syncingKernelFocus = false;
        }
        // 内核也会把捕获切到绑定的 kernelHwnd；真实 BUTTONUP 随后就不会再投递到可见窗。
        // 按下送达后强制把捕获抢回，页面点击与拖动才能收到完整 DOWN→MOVE→UP 链。
        if (GetCapture() != hwnd) SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        // 保持捕获直到内核收到 UP；先释放会让内核把它当成被取消的手势。
        if (view && g_kernel.fireMouseEvent) g_kernel.fireMouseEvent(view, message, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)), mouseFlags(wParam));
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
        if (view && g_kernel.fireMouseEvent) g_kernel.fireMouseEvent(view, message, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)), mouseFlags(wParam));
        return 0;
    case WM_MOUSEMOVE: {
        const int mx = static_cast<short>(LOWORD(lParam));
        const int my = static_cast<short>(HIWORD(lParam));
        // 上屏模式下内核直接收真实窗口消息：无按键移动按住拖动时仍显式转发给内核
        // （保持原生拖拽手势连续性），无按键移动交给内核自己的 hover 处理——
        // 不再需要 hover polyfill（那是离屏模式对"内核收不到真实 move"的补偿）。
        if (view && g_kernel.fireMouseEvent)
            g_kernel.fireMouseEvent(view, message, mx, my, mouseFlags(wParam));
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        fireBrowserWheel(browser, wParam, lParam, message == WM_MOUSEHWHEEL);
        return 0;
    }
    case WM_SETCURSOR:
        // 上屏模式：光标由内核原生管理（页面 CSS cursor 直接生效），交给默认过程。
        break;
    case WM_TIMER:
        // 页面侧定时器兜底在某些页面不派发（实测：桥经 mbRunJs 注入的脚本注册的
        // setInterval 从不触发，页面自己的定时器却正常）。扫描是接管视频的前提，
        // 所以由 native 用 Win32 定时器驱动：每秒调一次页面暴露的 __nmbIdleScan
        // （见注入脚本 armTimers），它内部再驱动 syncAll。mbRunJs 通道已被
        // hover polyfill 长期验证是活的。
        if (wParam == 1 && view && g_kernel.runJsByFrame && g_kernel.getMainFrame) {
            Frame frame = g_kernel.getMainFrame(view);
            if (frame) g_kernel.runJsByFrame(view, frame, "window.__nmbIdleScan&&window.__nmbIdleScan();", 0, nullptr, nullptr, nullptr);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

constexpr const wchar_t* kHostWindowClass = L"NativeMediaBridgeHostWindow";

void ensureHostWindowClass() {
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = browserWindowProc;
        wc.hInstance = g_self;
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kHostWindowClass;
        RegisterClassExW(&wc);
    });
}

HWND createHostWindow(HWND parent, int x, int y, int width, int height) {
    ensureHostWindowClass();
    // 父窗口存在时作为子控件嵌入，否则创建独立顶层窗口（由 Miniblink 之外的本模块管理）。
    const DWORD style = parent
        ? (WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS)
        : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    return CreateWindowExW(0, kHostWindowClass, parent ? L"" : L"NativeMediaBridge",
                           style, x, y, width, height, parent, nullptr, g_self, nullptr);
}

HWND createKernelWindow(int width, int height) {
    ensureHostWindowClass();
    // mb108 没有 HWND 会杀页；绑定可见窗又会与 paintBrowser 双重上屏。这个无标题工具窗专门
    // 接住内核直绘，始终放在虚拟桌面之外且不进任务栏/Alt+Tab。真实画面仍从 getViewDC 合成。
    return CreateWindowExW(WS_EX_TOOLWINDOW, kHostWindowClass, L"",
                           WS_POPUP, -32000, -32000, width, height,
                           nullptr, nullptr, g_self, nullptr);
}

void destroyBrowser(Browser* browser) {
    if (!browser) return;
    // 停掉 native 驱动的页面扫描定时器（见 browserWindowProc 的 WM_TIMER）。
    if (browser->hwnd) KillTimer(browser->hwnd, 1);
    // 合成缓冲归浏览器所有（不跟着媒体走）：DC 要先换回自带的单色位图才能删。
    if (browser->paintDC) {
        if (browser->paintOriginal) SelectObject(browser->paintDC, browser->paintOriginal);
        if (browser->paintBitmap) DeleteObject(browser->paintBitmap);
        DeleteDC(browser->paintDC);
        browser->paintDC = nullptr;
        browser->paintBitmap = nullptr;
        browser->paintOriginal = nullptr;
        browser->paintBits = nullptr;
    }
    if (browser->pageDC) {
        if (browser->pageOriginal) SelectObject(browser->pageDC, browser->pageOriginal);
        if (browser->pageBitmap) DeleteObject(browser->pageBitmap);
        DeleteDC(browser->pageDC);
        browser->pageDC = nullptr;
        browser->pageBitmap = nullptr;
        browser->pageOriginal = nullptr;
        browser->pageBits = nullptr;
    }
    releaseAllMedia(browser);
    if (browser->view && g_kernel.destroyWebView) { g_kernel.destroyWebView(browser->view); browser->view = nullptr; }
    if (browser->hwnd) {
        RemovePropW(browser->hwnd, L"NMB_BROWSER");
        DestroyWindow(browser->hwnd);
        browser->hwnd = nullptr;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it=g_browsers.begin();it!=g_browsers.end();++it) if(*it==browser){g_browsers.erase(it);break;}
    // 不在这里 delete：异步 open 的收尾线程可能还持着一份 self（见 Browser::self 的注释）。
    // 放掉自己这份后，结构活到最后一个持有者放手为止。
    browser->self.reset();
}

} // namespace nmb
