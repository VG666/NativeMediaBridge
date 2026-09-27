# -*- coding: utf-8 -*-
"""关窗探针：用户点窗口关闭按钮后，必须"窗口销毁 + 消息循环能退出 + 解码/音频线程停住"。

背景（这就是"关不干净、后台还有声音"的成因）：
宿主的消息循环（MBPython/window.py 的 message_loop）是阻塞式 GetMessage 驱动的，
只有收到 WM_QUIT 才会退出；而窗口过程以前既不处理 WM_CLOSE 也不处理 WM_DESTROY，
于是点关闭按钮之后窗口没了、宿主循环还在转、进程不退出、解码线程与声卡里的 PCM 继续跑。

本探针直接量三件事：
  1. 窗口是否真的销毁（IsWindow 变假）；
  2. 消息队列里有没有 WM_QUIT（宿主靠它退出循环，进程才有可能结束）；
  3. 进程内线程数有没有回落（每个媒体一路解码线程 + 一路音频线程，没回落就是"还在后台放"）。

用法：
  python tests\\probe_close_window.py
  python tests\\probe_close_window.py --page tests\\testjs\\_probe_buffer.html --play 8
"""
import argparse
import ctypes
import sys
import time
from ctypes import c_int, c_void_p, c_wchar_p
from ctypes import wintypes
from pathlib import Path

import win32gui

PROJECT_DIR = Path(__file__).resolve().parent.parent
NMB_DLL_PATH = PROJECT_DIR / "bin" / "NativeMediaBridge.dll"

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

WM_CLOSE = 0x0010
WM_QUIT = 0x0012

# 线程计数用 Toolhelp 快照：只需要数当前进程的线程，不需要额外依赖。
TH32CS_SNAPTHREAD = 0x00000004


class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ThreadID", wintypes.DWORD),
        ("th32OwnerProcessID", wintypes.DWORD),
        ("tpBasePri", wintypes.LONG),
        ("tpDeltaPri", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
    ]


def count_threads():
    """当前进程的线程数：解码/音频线程还在跑的话，这个数就不会落回去。"""
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.Thread32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(THREADENTRY32)]
    kernel32.Thread32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(THREADENTRY32)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if snapshot == wintypes.HANDLE(-1).value or not snapshot:
        return -1
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(THREADENTRY32)
    pid = kernel32.GetCurrentProcessId()
    total = 0
    try:
        ok = kernel32.Thread32First(snapshot, ctypes.byref(entry))
        while ok:
            if entry.th32OwnerProcessID == pid:
                total += 1
            ok = kernel32.Thread32Next(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return total


def take_quit():
    """线程队列里有没有 WM_QUIT：宿主那个阻塞式 GetMessage 循环靠它返回 0 退出。

    只能整队取出来看：WM_QUIT 是线程消息（hwnd 为空），带消息范围过滤的 PeekMessage 取不到它
    （实测过滤 0x12 仍为假），GetQueueStatus 的状态位又落在返回值高字里，容易看错。
    取走没关系——探针紧接着就要自己销毁浏览器，宿主那一轮已经等价地"看到"它了。
    """
    msg = wintypes.MSG()
    while user32.PeekMessageW(ctypes.byref(msg), 0, 0, 0, 1):  # PM_REMOVE
        if msg.message == WM_QUIT:
            return True
    return False


def configure_api(dll):
    dll.NMB_Initialize.argtypes = [c_wchar_p, c_wchar_p]
    dll.NMB_Initialize.restype = c_int
    dll.NMB_CreateBrowser.argtypes = [wintypes.HWND, c_int, c_int, c_int, c_int]
    dll.NMB_CreateBrowser.restype = c_void_p
    dll.NMB_LoadURL.argtypes = [c_void_p, c_wchar_p]
    dll.NMB_LoadURL.restype = c_int
    dll.NMB_Show.argtypes = [c_void_p, c_int]
    dll.NMB_Show.restype = None
    dll.NMB_GetWindow.argtypes = [c_void_p]
    dll.NMB_GetWindow.restype = wintypes.HWND
    dll.NMB_DestroyBrowser.argtypes = [c_void_p]
    dll.NMB_DestroyBrowser.restype = None
    dll.NMB_Shutdown.argtypes = []
    dll.NMB_Shutdown.restype = None
    dll.NMB_GetLastError.argtypes = []
    dll.NMB_GetLastError.restype = c_wchar_p


def pump(seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        win32gui.PumpWaitingMessages()
        time.sleep(0.01)


def main():
    parser = argparse.ArgumentParser(description="关窗探针：验证关窗后消息循环可退出、线程停住")
    parser.add_argument("--page", default=str(Path(__file__).resolve().parent / "testjs" / "_probe_buffer.html"))
    parser.add_argument("--size", default="1000x700")
    parser.add_argument("--play", type=float, default=8.0, help="关窗前先播多久（秒）")
    parser.add_argument("--after", type=float, default=2.5, help="关窗后观察多久（秒）")
    args = parser.parse_args()

    width, height = (int(v) for v in args.size.lower().split("x"))
    page = Path(args.page)
    if not page.is_absolute():
        page = PROJECT_DIR / page
    url = page.as_uri()

    print(f"DLL   : {NMB_DLL_PATH}")
    print(f"页面  : {url}")
    print("模式  : 顶层窗口（带关闭按钮），点关闭后检查窗口/消息循环/线程\n")

    nmb = ctypes.CDLL(str(NMB_DLL_PATH))
    configure_api(nmb)
    if not nmb.NMB_Initialize(None, None):
        raise SystemExit("NMB_Initialize 失败：" + (nmb.NMB_GetLastError() or ""))

    threads_idle = count_threads()
    browser = nmb.NMB_CreateBrowser(None, 120, 80, width, height)
    if not browser:
        raise SystemExit("NMB_CreateBrowser 失败：" + (nmb.NMB_GetLastError() or ""))
    if not nmb.NMB_LoadURL(browser, url):
        raise SystemExit("NMB_LoadURL 失败：" + (nmb.NMB_GetLastError() or ""))
    nmb.NMB_Show(browser, 1)
    hwnd = nmb.NMB_GetWindow(browser)
    print(f"窗口  : hwnd={hwnd}")

    # 等媒体真的播起来：页面把播放读数写在标题上，标题在动就说明解码/音频线程都在跑。
    deadline = time.time() + args.play
    last_title = ""
    while time.time() < deadline:
        pump(0.2)
        title = win32gui.GetWindowText(hwnd) if win32gui.IsWindow(hwnd) else ""
        if title:
            last_title = title
    threads_playing = count_threads()
    print(f"播放中: 线程数={threads_playing}（空载 {threads_idle}）")
    print(f"        标题={last_title[:150]}")

    if not win32gui.IsWindow(hwnd):
        raise SystemExit("窗口在关闭前就没了，探针无法继续")

    # 等价于用户点右上角关闭按钮：系统发的就是 WM_CLOSE，宿主 GetMessage 循环派发给窗口过程。
    print("\n== 发送 WM_CLOSE（等价于点关闭按钮）==")
    user32.SendMessageW(hwnd, WM_CLOSE, 0, 0)

    still_alive = bool(win32gui.IsWindow(hwnd))
    quit_posted = take_quit()
    print(f"窗口销毁 : {'否（还在）' if still_alive else '是'}")
    print(f"WM_QUIT  : {'有（宿主循环会退出）' if quit_posted else '没有（宿主循环会一直转）'}")

    pump(args.after)
    threads_after = count_threads()
    # 关窗后 webview 内核自己的线程还在（要等 NMB_DestroyBrowser 才收），
    # 这里要看的是"每个媒体一路解码 + 一路音频"这两条有没有停掉。
    stopped = threads_playing >= 0 and threads_after < threads_playing
    print(f"线程回落 : 播放中 {threads_playing} -> 关窗后 {threads_after}"
          f"（少 {threads_playing - threads_after} 条，每个媒体应有解码+音频共 2 条停下；空载 {threads_idle}）")

    print("\n== 收尾 ==")
    started = time.time()
    nmb.NMB_DestroyBrowser(browser)
    cost = time.time() - started
    threads_final = count_threads()
    print(f"NMB_DestroyBrowser 用时 {cost:.2f}s，线程数 {threads_after} -> {threads_final}（应收回空载附近 {threads_idle}）")
    print("（若解码线程还卡在网络读取里，这里的 join 就会明显变慢甚至吊住）")
    nmb.NMB_Shutdown()

    ok = (not still_alive) and quit_posted and stopped and cost < 1.0
    print("\n结论: " + ("关窗干净：窗口销毁、宿主循环可退出、解码/音频线程停住" if ok else "关窗仍有残留，见上面逐项"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
