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
    // 绑定窗（kernelHwnd）走的是同一个窗口类，用 NMB_KERNEL 区分：它不是可视宿主，
    // 只处理一件事——把内核"可拖动区域"的拖动请求转给真正可见的顶层窗口。
    if (auto* owner = reinterpret_cast<Browser*>(GetPropW(hwnd, L"NMB_KERNEL"))) {
        // mbFireMouseEvent 在按下 CSS `-webkit-app-region: drag` 节点时会向"绑定窗的根窗"
        // 投递 WM_SYSCOMMAND/SC_MOVE。绑定窗是永不显示的离屏工具窗、自身就是根窗，系统于是
        // 去拖那个看不见的窗口：用户端完全没反应（表现为无边框窗口拖不动）。这里把这条消息
        // 改投到可见宿主窗所在的顶层窗口，交给系统按原生方式拖动真正可见的窗口。
        if (message == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_MOVE) {
            HWND root = owner->hwnd;
            if (root) {
                for (HWND parent = GetParent(root); parent; parent = GetParent(root)) root = parent;
                PostMessageW(root, WM_SYSCOMMAND, wParam, lParam);
                return 0;
            }
        }
        // mbSetFocus 可能把系统焦点留在屏幕外的绑定窗；滚轮随后会投递到这里，
        // 不能让 DefWindowProc 丢掉，转交给真正可见的桥窗口处理。
        if ((message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) && owner->hwnd) {
            fireBrowserWheel(owner, wParam, lParam, message == WM_MOUSEHWHEEL);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
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
            // 绑定窗与 view 保持同尺寸，避免内核直绘/脏区计算按旧尺寸裁切帧缓冲。
            if (browser->kernelHwnd) SetWindowPos(browser->kernelHwnd, nullptr, -32000, -32000,
                                                   width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            g_kernel.resize(view, width, height);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        // 只合成**脏区**（ps.rcPaint），不要读 GetClientRect 整窗：paintBrowser 读的是内核的
        // getViewDC —— 那是内核正在往里画的实时帧缓冲，滚动时它分块重绘，整窗重刷会把"画到一半的
        // 页面"一起 blit 上屏（滚动时整窗闪、且每帧白刷一遍整块 framebuffer）。
        // paintBrowser 本来就按传进来的 target 做偏移，传脏区即可。
        paintBrowser(browser, dc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_APP + 2:
        // 标志在这里才清掉，期间积累的合成请求已经被合并掉了。
        browser->paintPosted.store(false);
        {
            // 这条只由"新视频帧到了"触发，而视频帧只会盖住它自己的矩形：把脏区收窄到这些矩形上。
            // 过去无条件 InvalidateRect(整窗) 等于每帧重刷整窗（见 WM_PAINT 的注释）。
            RECT dirty{};
            bool any = false;
            std::lock_guard<std::mutex> lock(browser->mutex);
            for (const auto& item : browser->media) {
                Media* media = item.second;
                if (!media || media->kind != L"video") continue;
                const RectI& r = media->rect;
                if (r.width <= 0 || r.height <= 0) continue;
                RECT box{r.x, r.y, r.x + r.width, r.y + r.height};
                if (!any) { dirty = box; any = true; } else UnionRect(&dirty, &dirty, &box);
            }
            // 没有视频条目（页面自己重绘）时仍旧整窗，交给内核的 onWebViewPainted 去收窄。
            if (any) InvalidateRect(hwnd, &dirty, FALSE);
            else InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_APP + 3: {
        // 鼠标移动合流的落点：一轮消息循环里无论到了多少条 WM_MOUSEMOVE，这里只按最新坐标调一次
        // JS。页面的 hover polyfill 用 elementsFromPoint 命中并合成事件；polyfill 尚未注入
        // （页面早期）或内核没导出取主 frame 的接口时直接跳过。
        // 档3的 cursor 值由 polyfill 通过 mbQuery 异步通知 native（见 mediaBridge 的 cursor op），
        // 不在这里碰 title（避免与诊断脚本的 title 回传机制冲突）。
        browser->mousePosted.store(false);
        if (view && g_kernel.runJsByFrame && g_kernel.getMainFrame) {
            Frame frame = g_kernel.getMainFrame(view);
            if (frame) {
                char code[96];
                const int n = std::snprintf(code, sizeof(code),
                                            "window.__nmbHoverMove&&window.__nmbHoverMove(%d,%d);",
                                            browser->mouseX.load(), browser->mouseY.load());
                if (n > 0 && n < static_cast<int>(sizeof(code)))
                    g_kernel.runJsByFrame(view, frame, code, 0, nullptr, nullptr, nullptr);
            }
        }
        return 0;
    }
    case WM_SETFOCUS:
        if (view && g_kernel.setFocus && !browser->syncingKernelFocus) {
            // mbSetFocus 会把 Win32 焦点转给绑定的 kernelHwnd。让内核先完成逻辑聚焦，再立即把
            // 系统焦点送回用户可见窗；同步期间产生的 KILL/SETFOCUS 是内部跳转，不能反向 killFocus。
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
        // 无按键移动只交给 hover polyfill。离屏模式下内核的无按键 move 不会正常派发 DOM
        // 事件，但仍可能更新内部 hover/触发一次重绘；再叠加下面的合成 mousemove 会让同一
        // 次悬浮产生两套 hover 状态切换，表现为按钮/覆盖层闪烁。按住按钮拖动时仍交给内核，
        // 保持原生拖拽手势的连续性。
        const bool dragging = (wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) != 0;
        if (dragging && view && g_kernel.fireMouseEvent)
            g_kernel.fireMouseEvent(view, message, mx, my, mouseFlags(wParam));
        // 同时把坐标合流给页面 hover polyfill（mb108 自己不为无按键移动派发 DOM 事件）。
        browser->mouseX.store(mx);
        browser->mouseY.store(my);
        bool expected = false;
        if (browser->mousePosted.compare_exchange_strong(expected, true))
            PostMessageW(hwnd, WM_APP + 3, 0, 0);
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        fireBrowserWheel(browser, wParam, lParam, message == WM_MOUSEHWHEEL);
        return 0;
    }
    case WM_SETCURSOR: {
        // 档3：mb108 离屏模式不子类化窗口，内核的 fireWindowsMessage 对鼠标零 DOM 输出。
        // cursor 值已在 WM_APP+3 的 hover move 里同步到 browser->cursorType，这里直接设光标。
        // 只在客户区（HTCLIENT）设光标，非客户区交给 DefWindowProc 画标准箭头。
        if (LOWORD(lParam) == 1 /* HTCLIENT */) {
            const int ct = browser->cursorType.load();
            HCURSOR h = nullptr;
            if (ct == 1)      h = LoadCursorA(nullptr, IDC_HAND);
            else if (ct == 2) h = LoadCursorA(nullptr, IDC_IBEAM);
            else              h = LoadCursorA(nullptr, IDC_ARROW);
            if (h) { SetCursor(h); return TRUE; }
        }
        break;
    }
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
    // 先销毁 view，让内核不再持有工具窗；再销毁工具窗本身。
    if (browser->kernelHwnd) {
        RemovePropW(browser->kernelHwnd, L"NMB_KERNEL");
        DestroyWindow(browser->kernelHwnd);
        browser->kernelHwnd = nullptr;
    }
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
