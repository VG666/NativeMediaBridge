# -*- coding: utf-8 -*-
"""NativeMediaBridge 交互测试：进度条拖动、滚动、页面跳转、多视频并发播放。

默认打开窗口后一切动作都由你手动操作，脚本只负责创建窗口并回读页面状态：
  1. 拖动媒体下方控件条的进度条（播放中和暂停时都要试），画面和时间应立即跟随；
  2. 滚动页面：画面与控件条要跟着元素走，元素滚出视口后画面不应残留在窗口边缘；
  3. 点页面里的链接（含 target=_blank）验证手动跳转，跳转后视频仍能播放；
  4. 最大化 / 还原 / 手动拉伸窗口，页面与视频区域应跟着缩放。

只有加 --auto 才会启用脚本化自检（自动窗口事件 + 页面 ?auto=1 自检），用于回归验证。
"""
import argparse
import ctypes
import os
import sys
import threading
import time
from ctypes import c_int, c_void_p, c_wchar_p
from ctypes import wintypes
from pathlib import Path

import win32gui

# 界面响应度观测与看门狗是宿主探针和内核对照程序共用的，单独放在 nmb_probe_common.py。
sys.path.insert(0, str(Path(__file__).resolve().parent))
from nmb_probe_common import ResponseMonitor, print_responsiveness  # noqa: E402

PROJECT_DIR = Path(__file__).resolve().parent.parent
NMB_BIN_DIR = PROJECT_DIR / "bin"
NMB_DLL_PATH = NMB_BIN_DIR / "NativeMediaBridge.dll"
# 要跑别的产物（例如旁路编译出的 NativeMediaBridge-test.dll）：设环境变量 NMB_DLL_PATH 覆盖。
# 命令行 --dll=... 仍然更优先（它的默认值取的就是这里）。
if os.environ.get("NMB_DLL_PATH"):
    NMB_DLL_PATH = Path(os.environ["NMB_DLL_PATH"])
DEFAULT_PAGE = Path(__file__).resolve().parent / "testjs" / "media_suite.html"


def _kernel_dll(directory=None):
    """返回与桥 DLL 同目录的内核 DLL，保持桥和 UA 操作使用同一内核。"""
    directory = Path(directory) if directory is not None else NMB_BIN_DIR
    for name in ("miniblink_x64.dll", "mb108_x64.dll", "mb132_x64.dll"):
        candidate = directory / name
        if candidate.is_file():
            return candidate
    loose = sorted(directory.glob("mb*_x64.dll"))
    return loose[0] if loose else directory / "miniblink_x64.dll"


# 取证脚本（diag_pagejs.py / probe_compat_shim.py 等）要自己 WinDLL 一次内核，
# 以前各写各的文件名，换内核时会有漏改的；统一从这里取。
NMB_KERNEL_DLL: Path = _kernel_dll()
# 自动化自检时给页面加的参数，页面看到 ?auto=1 才会跑脚本化自检。
AUTO_QUERY = "?auto=1"

DEFAULT_UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/60.0.3112.113 Safari/537.36"
)

# (延后秒数, 说明) —— 仅 --auto 时使用的窗口事件验证时间轴
WINDOW_EVENT_PLAN = (
    (4.0, "最大化窗口"),
    (8.0, "还原窗口"),
    (10.0, "调整窗口尺寸为 900x620"),
    (13.0, "恢复初始尺寸"),
)

MANUAL_CHECKLIST = """
窗口已打开，动作全部由你手动触发（页面不会自动跳转，也不会自动播放）：

  [1] 播放与拖动：点控件条的播放按钮，再拖动进度条（播放中和暂停时都要试），
      画面必须立刻跳到新位置，暂停时也要看到新画面。
  [2] 滚动：滚动页面，视频画面与控件条要跟着元素一起走；
      把视频滚出视口后，窗口边缘不应残留画面。
  [3] 声音：连续播放一段时间，声音应该是连续的，不应断断续续。
  [4] 多个视频：点“同时播放两个视频”，两路画面互不干扰，声音都应能听到。
  [5] 手动跳转：点腾讯视频 / 刷新式跳转 / target=_blank 三个链接，
      跳转后页面正常显示且视频仍能播放（旧视频不应残留在新页面上）。
  [6] 窗口事件：最大化、还原、拖边拉伸时，页面与视频区域都跟随缩放。
"""


def configure_api(nmb):
    nmb.NMB_Initialize.argtypes = [c_wchar_p, c_wchar_p]
    nmb.NMB_Initialize.restype = c_int

    nmb.NMB_CreateBrowser.argtypes = [
        wintypes.HWND,
        c_int,
        c_int,
        c_int,
        c_int,
    ]
    nmb.NMB_CreateBrowser.restype = c_void_p

    nmb.NMB_LoadURL.argtypes = [c_void_p, c_wchar_p]
    nmb.NMB_LoadURL.restype = c_int

    nmb.NMB_Show.argtypes = [c_void_p, c_int]
    nmb.NMB_Show.restype = None

    nmb.NMB_ShowDevTools.argtypes = [c_void_p, c_wchar_p]
    nmb.NMB_ShowDevTools.restype = c_int

    nmb.NMB_Resize.argtypes = [c_void_p, c_int, c_int]
    nmb.NMB_Resize.restype = None

    nmb.NMB_GetWindow.argtypes = [c_void_p]
    nmb.NMB_GetWindow.restype = wintypes.HWND

    nmb.NMB_GetWebView.argtypes = [c_void_p]
    nmb.NMB_GetWebView.restype = c_void_p

    nmb.NMB_SetMaximized.argtypes = [c_void_p, c_int]
    nmb.NMB_SetMaximized.restype = c_int

    nmb.NMB_IsMaximized.argtypes = [c_void_p]
    nmb.NMB_IsMaximized.restype = c_int

    nmb.NMB_DestroyBrowser.argtypes = [c_void_p]
    nmb.NMB_DestroyBrowser.restype = None

    nmb.NMB_Shutdown.argtypes = []
    nmb.NMB_Shutdown.restype = None

    nmb.NMB_GetLastError.argtypes = []
    nmb.NMB_GetLastError.restype = c_wchar_p


def last_error(nmb, operation):
    detail = nmb.NMB_GetLastError()
    return f"{operation} 失败：{detail or '没有详细错误信息'}"


def describe_window(hwnd):
    if not hwnd or not win32gui.IsWindow(hwnd):
        return "窗口不存在"
    left, top, right, bottom = win32gui.GetWindowRect(hwnd)
    client = win32gui.GetClientRect(hwnd)
    return (
        f"窗口 {right - left}x{bottom - top} @({left},{top})，"
        f"客户区 {client[2] - client[0]}x{client[3] - client[1]}"
    )


def apply_window_event(nmb, browser, index, size, results):
    """执行时间轴上的一项窗口事件，并记录结果。"""
    hwnd = nmb.NMB_GetWindow(browser)
    if index == 0:
        state = nmb.NMB_SetMaximized(browser, 1)
        results.append(("最大化", state == 1, describe_window(hwnd)))
    elif index == 1:
        state = nmb.NMB_SetMaximized(browser, 0)
        results.append(("还原", state == 0, describe_window(hwnd)))
    elif index == 2:
        nmb.NMB_Resize(browser, 900, 620)
        results.append(("调整尺寸 900x620", True, describe_window(hwnd)))
    else:
        nmb.NMB_Resize(browser, size[0], size[1])
        results.append(("恢复尺寸", True, describe_window(hwnd)))


def run_message_loop(nmb, browser, size, run_window_events, duration=0):
    """自己泵消息，这样可以在阻塞式消息循环之外做定时验证。

    界面线程就是这个循环，所以"每秒泵到多少次消息、最长一次停顿多久"直接反映界面
    有没有被拖住（正常约 100 次/秒、单次停顿 ~10ms）。真正卡死时连 pump() 都回不来，
    主循环里的 --duration 判断不到，因此交给共用看门狗兜底（见 nmb_probe_common）。
    """
    pump = getattr(win32gui, "PumpWaitingMessages", None)
    if pump is None:
        win32gui.PumpMessages()
        return [], [], None

    hwnd = nmb.NMB_GetWindow(browser)
    # 无响应超过 duration+8 秒视为卡死并强制退出；手动模式（无 --duration）只报告不退出。
    monitor = ResponseMonitor()
    monitor.start_watchdog((duration + 8.0) if duration else 0)

    step = 0
    results = []
    titles = []
    last_title = ""
    while True:
        pump()
        monitor.tick()
        if not win32gui.IsWindow(hwnd):
            print("\n窗口已关闭，结束消息循环。")
            break
        elapsed = monitor.elapsed
        title = win32gui.GetWindowText(hwnd)
        if title and title != last_title:
            last_title = title
            titles.append(f"[{elapsed:5.1f}s] {title}")
        if run_window_events and step < len(WINDOW_EVENT_PLAN):
            delay, _ = WINDOW_EVENT_PLAN[step]
            if elapsed >= delay:
                apply_window_event(nmb, browser, step, size, results)
                step += 1
        if duration and elapsed >= duration:
            print(f"\n已达到 --duration {duration} 秒，自动结束。")
            break
        time.sleep(0.01)

    return results, titles, monitor.stats()


def main():
    parser = argparse.ArgumentParser(description="NativeMediaBridge 交互测试")
    parser.add_argument("--page", default=str(DEFAULT_PAGE), help="要加载的本地页面")
    parser.add_argument("--url", default="", help="直接加载这个远程地址（用于复现真实站点的卡顿/播放问题）")
    parser.add_argument("--size", default="1000x700", help="初始窗口尺寸，例如 1000x700")
    parser.add_argument(
        "--auto",
        action="store_true",
        help="启用脚本化自检：自动最大化/还原/改尺寸，并让页面执行 ?auto=1 自检（默认全手动）",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=0,
        help="多少秒后自动结束（0 表示一直等到手动关闭窗口），用于快速自检",
    )
    parser.add_argument(
        "--no-bridge",
        action="store_true",
        help="完全不注入媒体桥（对照用）：用来判断页面卡顿是桥造成的还是内核/站点本身的",
    )
    parser.add_argument(
        "--ua",
        default=DEFAULT_UA,
        help='浏览器 UA（默认 Windows 桌面 Chrome）；传空字符串 --ua "" 使用 DLL 默认 UA',
    )
    parser.add_argument(
        "--devtools",
        default="",
        help="启动后打开内核 DevTools 调试器；值为前端资源路径（miniblink 发行包 "
        "front_end 目录下 inspector.html 的全路径，例：F:\\downloads\\miniblink-20230412\\release\\front_end\\inspector.html）",
    )
    parser.add_argument(
        "--compat-shim",
        choices=("", "chrome", "desktop", "safari", "ios", "mse", "ios+mse"),
        default="chrome",
        help='媒体能力兼容模式（默认 chrome，不改 UA；desktop 仅补桌面媒体能力）；传 --compat-shim "" 可关闭',
    )
    parser.add_argument(
        "--dll",
        default=str(NMB_DLL_PATH),
        help="要加载的桥 DLL（默认 bin\\NativeMediaBridge.dll）",
    )
    args = parser.parse_args()

    width, _, height = args.size.partition("x")
    size = (int(width), int(height))

    dll_path = Path(args.dll).resolve()
    if not dll_path.is_file():
        raise FileNotFoundError(f"找不到 {dll_path}")
    dll_directory_path = dll_path.parent
    kernel_path = _kernel_dll(dll_directory_path)
    if not kernel_path.is_file():
        raise FileNotFoundError(f"找不到与桥 DLL 同目录的内核 DLL：{kernel_path}")

    if args.url:
        # 远程页面：不做本地页面校验，也不加自检参数（页面不是我们的）。
        page_url = args.url
        local_page = False
    else:
        # 允许 --page 直接带查询串（例如 _probe_query.html?src=...），
        # 这样不新建文件就能给探针换参数；--auto 的自检参数再追加在后面。
        raw_page, _, raw_query = args.page.partition("?")
        page_path = Path(raw_page).resolve()
        if not page_path.is_file():
            raise FileNotFoundError(f"找不到 {page_path}")
        suffix = ("?" + raw_query) if raw_query else ""
        if args.auto:
            suffix += AUTO_QUERY
        page_url = page_path.as_uri() + suffix
        local_page = True

    # 测试宿主不是 Node 入口；禁用内嵌 Node，避免它把 Python 的 -u/脚本参数当成 require 目标。
    os.environ.setdefault("NMB_NO_NODEJS", "1")
    # 必须在 DLL 初始化前设好，DLL 只在第一次用到时读这些变量。
    if args.no_bridge:
        os.environ["NMB_NO_MEDIA_BRIDGE"] = "1"
    if args.compat_shim:
        os.environ["NMB_COMPAT_SHIM"] = args.compat_shim
        os.environ.pop("NMB_NO_COMPAT_SHIM", None)
    else:
        os.environ.pop("NMB_COMPAT_SHIM", None)
        os.environ["NMB_NO_COMPAT_SHIM"] = "1"

    print(f"DLL   : {dll_path}")
    print(f"页面  : {page_url}")
    print(f"UA    : {args.ua or '（内核默认）'}")
    print(f"兼容  : {args.compat_shim or '关闭'}")
    print("模式  : " + ("自动自检（--auto）" if args.auto else "手动（页面不会自动跳转，也不会自动播放）"))
    if args.no_bridge:
        print("对照  : 已关闭媒体桥注入（NMB_NO_MEDIA_BRIDGE=1），此时页面完全由内核自己处理")
    if local_page:
        print(MANUAL_CHECKLIST)
    else:
        print("\n窗口已打开，动作全部由你手动触发；结束后会自动打印界面响应度。\n")

    dll_directory = os.add_dll_directory(str(dll_directory_path))
    nmb = None
    initialized = False
    browser = None
    results = []
    titles = []
    stats = None
    runtime_error = ""

    try:
        nmb = ctypes.CDLL(str(dll_path))
        configure_api(nmb)

        if not nmb.NMB_Initialize(None, None):
            raise RuntimeError(last_error(nmb, "NMB_Initialize"))
        initialized = True

        browser = nmb.NMB_CreateBrowser(None, 120, 80, size[0], size[1])
        if not browser:
            raise RuntimeError(last_error(nmb, "NMB_CreateBrowser"))

        if args.devtools:
            if nmb.NMB_ShowDevTools(browser, args.devtools):
                print(f"DevTools: 已请求打开（{args.devtools}）")
            else:
                print(f"[warn] DevTools 打不开: {last_error(nmb, 'NMB_ShowDevTools')}")

        if args.ua:
            # UA 必须在首次导航前设好，HTTP 请求头才会带上；navigator.userAgent 同步生效。
            # 桥没封装这个导出，直接对内核 DLL 调 mbSetUserAgent（同一个已加载模块，无副作用）。
            kernel = ctypes.CDLL(str(kernel_path))
            kernel.mbSetUserAgent.argtypes = [c_void_p, ctypes.c_char_p]
            kernel.mbSetUserAgent.restype = None
            kernel.mbSetUserAgent(nmb.NMB_GetWebView(browser), args.ua.encode("utf-8"))

        if not nmb.NMB_LoadURL(browser, page_url):
            raise RuntimeError(last_error(nmb, "NMB_LoadURL"))

        nmb.NMB_Show(browser, 1)
        print(f"窗口已创建：{describe_window(nmb.NMB_GetWindow(browser))}\n")

        results, titles, stats = run_message_loop(
            nmb, browser, size, args.auto, args.duration
        )
        # 原生解码/播放失败时会记录在这里，可用来判断视频到底有没有被真正打开。
        runtime_error = nmb.NMB_GetLastError() or ""
    finally:
        if nmb is not None and browser:
            nmb.NMB_DestroyBrowser(browser)
        if nmb is not None and initialized:
            nmb.NMB_Shutdown()
        dll_directory.close()

    if results:
        print("\n窗口事件验证结果：")
        for name, ok, detail in results:
            print(f"  [{'通过' if ok else '失败'}] {name} —— {detail}")
    if titles:
        print("\n页面状态回读（窗口标题变化）：")
        for line in titles:
            print(f"  {line}")
    print_responsiveness(stats)
    print(f"\n运行期错误信息：{runtime_error or '无'}")
    print("\n测试结束。")


if __name__ == "__main__":
    main()
