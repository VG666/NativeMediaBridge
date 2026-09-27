# -*- coding: utf-8 -*-
"""新内核（miniblink 132 / mb 接口）烟测。

换内核前必须先确认三件事，否则改桥就是盲改：
  1. mbInit + 离屏 view（mbCreateWebView + mbSetHandle）能不能跑起来、DC 能不能取到；
  2. 页面侧 window.mbQuery 的回调是**同步**还是**异步**——这决定注入脚本的
     nmbMedia(...) 调用要不要改成异步；
  3. 新内核的 UA / 编解码能力（老内核正是栽在这里，站点判定"设备不适合播放"）。

用法：
  python tests\\probe_mb132_smoke.py
  python tests\\probe_mb132_smoke.py --kernel "D:\\other\\mb_x64.dll" --seconds 12
"""
import argparse
import ctypes
import os
import struct
import sys
import tempfile
import time
from ctypes import CFUNCTYPE, WINFUNCTYPE, c_char_p, c_int, c_int64, c_void_p
from pathlib import Path

import win32con
import win32gui

DEFAULT_KERNEL = Path(r"F:\downloads\miniblink132_251212\mb132_x64.dll")

# 回调原型：mbWebView/mbWebFrameHandle/mbJsExecState 都是不透明指针，mbJsValue 是 int64。
DOC_READY = CFUNCTYPE(None, c_void_p, c_void_p, c_void_p)
JS_QUERY = CFUNCTYPE(None, c_void_p, c_void_p, c_void_p, c_int64, c_int, c_char_p)
TITLE_CHANGED = CFUNCTYPE(None, c_void_p, c_void_p, c_char_p)
RUNJS_DONE = CFUNCTYPE(None, c_void_p, c_void_p, c_void_p, c_int64)
# 回调参数放成 c_void_p，这样才能像 C++ 的 nullptr 一样传真正的 NULL。
RUNJS_NULL = WINFUNCTYPE(None, c_void_p, c_void_p, c_char_p, c_int, c_void_p, c_void_p, c_void_p)

PAGE = b"""<!doctype html><html><head><meta charset="utf-8"><title>MB132-SMOKE</title></head>
<body><h1>smoke</h1><script>
var out = [];
function rec(s){ out.push(s); }
var syncState = 'UNKNOWN', resp = null, threw = null;
try {
  window.mbQuery(4660, "hello", function(customMsg, response){ resp = response; });
  syncState = (resp === null) ? 'ASYNC' : 'SYNC';
} catch(e) { threw = String(e); }
rec('mbQuery=' + syncState + '|resp=' + resp + '|threw=' + threw + '|typeof=' + (typeof window.mbQuery));
var v = document.createElement('video');
rec('canMp4=' + (v.canPlayType('video/mp4') || 'no') + '|canHls=' + (v.canPlayType('application/vnd.apple.mpegurl') || 'no'));
var m = navigator.userAgent.match(/Chrome\\/(\\d+)/);
rec('chrome=' + (m ? m[1] : '?'));
document.title = 'RESULT|' + out.join('|');
setTimeout(function(){ document.title = 'LATE|' + syncState + '|resp=' + resp; }, 1500);
</script></body></html>
"""


def load_kernel(path):
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.SetDefaultDllDirectories(0x00000800 | 0x00001000)  # 只从已注册目录找依赖，避免误加载
    kernel32.AddDllDirectory(str(path.parent))
    return ctypes.WinDLL(str(path))


def declare(mb):
    mb.mbInit.argtypes = [c_void_p]
    mb.mbInit.restype = None
    mb.mbCreateInitSettings.restype = c_void_p
    mb.mbSetInitSettings.argtypes = [c_void_p, c_char_p, c_char_p]
    mb.mbSetInitSettings.restype = None

    mb.mbCreateWebView.restype = c_void_p
    mb.mbDestroyWebView.argtypes = [c_void_p]
    mb.mbSetHandle.argtypes = [c_void_p, c_void_p]
    mb.mbSetAutoDrawToHwnd.argtypes = [c_void_p, c_int]
    mb.mbResize.argtypes = [c_void_p, c_int, c_int]
    mb.mbShowWindow.argtypes = [c_void_p, c_int]
    mb.mbLoadURL.argtypes = [c_void_p, c_char_p]
    mb.mbGetTitle.restype = c_char_p
    mb.mbWake.argtypes = [c_void_p]
    mb.mbGetLockedViewDC.argtypes = [c_void_p]
    mb.mbGetLockedViewDC.restype = c_void_p
    mb.mbUnlockViewDC.argtypes = [c_void_p]

    mb.mbOnDocumentReady.argtypes = [c_void_p, DOC_READY, c_void_p]
    mb.mbOnJsQuery.argtypes = [c_void_p, JS_QUERY, c_void_p]
    mb.mbOnTitleChanged.argtypes = [c_void_p, TITLE_CHANGED, c_void_p]
    mb.mbRunJs.argtypes = [c_void_p, c_void_p, c_char_p, c_int, RUNJS_DONE, c_void_p, c_void_p]
    mb.mbRunJsSync.argtypes = [c_void_p, c_void_p, c_char_p, c_int]
    mb.mbRunJsSync.restype = c_int64
    mb.mbWebFrameGetMainFrame.argtypes = [c_void_p]
    mb.mbWebFrameGetMainFrame.restype = c_void_p
    mb.mbGetContentWidth.argtypes = [c_void_p]
    mb.mbGetContentWidth.restype = c_int
    mb.mbGetContentHeight.argtypes = [c_void_p]
    mb.mbGetContentHeight.restype = c_int
    mb.mbIsMainFrame.argtypes = [c_void_p, c_void_p]
    mb.mbIsMainFrame.restype = c_int
    mb.mbGetGlobalExecByFrame.argtypes = [c_void_p, c_void_p]
    mb.mbGetGlobalExecByFrame.restype = c_void_p
    mb.mbJsToString.argtypes = [c_void_p, c_int64]
    mb.mbJsToString.restype = c_char_p
    mb.mbOnDocumentReadyInBlinkThread.argtypes = [c_void_p, DOC_READY, c_void_p]
    mb.mbResponseQuery.argtypes = [c_void_p, c_int64, c_int, c_char_p]
    mb.mbResponseQuery.restype = None
    mb.mbJsToString.argtypes = [c_void_p, c_int64]
    mb.mbJsToString.restype = c_char_p


def make_host_window(width, height):
    wc = win32gui.WNDCLASS()
    wc.lpszClassName = "Mb132SmokeProbe"
    wc.hbrBackground = win32con.COLOR_WINDOW + 1
    wc.lpfnWndProc = {}
    atom = win32gui.RegisterClass(wc)
    hwnd = win32gui.CreateWindow(
        atom, "MB132 smoke", win32con.WS_OVERLAPPEDWINDOW, 80, 80, width, height, 0, 0, 0, None
    )
    win32gui.ShowWindow(hwnd, win32con.SW_SHOWNORMAL)
    win32gui.UpdateWindow(hwnd)
    return hwnd


def main():
    parser = argparse.ArgumentParser(description="miniblink 132 (mb API) 烟测")
    parser.add_argument("--kernel", default=str(DEFAULT_KERNEL), help="内核 DLL 路径")
    parser.add_argument("--seconds", type=float, default=8.0, help="观察时长")
    parser.add_argument("--size", default="1000x700", help="离屏 view 尺寸")
    parser.add_argument("--url", default="", help="直接加载这个远程地址（不给就加载内置的烟测页）")
    args = parser.parse_args()

    kernel_path = Path(args.kernel).resolve()
    if not kernel_path.is_file():
        raise FileNotFoundError(f"找不到内核 {kernel_path}")
    width, _, height = args.size.partition("x")
    width, height = int(width), int(height)

    print(f"内核: {kernel_path}  ({kernel_path.stat().st_size} 字节)")

    mb = load_kernel(kernel_path)
    declare(mb)

    class LiveLog(list):
        # 边跑边打印：探针里有些调用会让进程直接崩掉，写在最后打印就什么都看不到了。
        def append(self, item):
            super().append(item)
            print("  [%s] %s" % (item[0], item[1]), flush=True)

    logs = LiveLog()
    ready_frame = [None]  # 文档就绪回调给过来的 frame 句柄，留给后面做对照

    def on_title(view, param, title):
        text = title.decode("utf-8", "replace") if title else ""
        logs.append(("标题回调", text))

    def on_ready(view, param, frame):
        # 页面脚本已就绪。这里要一次问清两件事：
        #   1. mbOnDocumentReady 到底会不会触发（会不会被回调）；
        #   2. mbRunJs 的 isInClosure 取 0/1 时，顶层 function 声明会不会变成 window 上的全局。
        #      （媒体桥的注入脚本靠顶层 function 暴露 send/adopt，如果变不成全局，注入就等于没做。）
        # 用 mbRunJsSync 保证两次调用的先后顺序，结果写回 document.title 后立刻读回来。
        ready_frame[0] = frame
        # 桥就是在"文档就绪回调内部"发起注入的。这里复现同一时机：
        # 如果这样发的 mbRunJs 不生效、而在消息循环里发的生效，那问题就出在调用时机。
        def ready_done(v, param, es, value):
            logs.append(("就绪内RunJs", "回调被触发"))
        try:
            ready_run_cb = RUNJS_DONE(ready_done)
            mb.mbRunJs(view, frame, b"window.__fromReady=1;", 0, ready_run_cb, None, None)
            logs.append(("文档就绪", "已触发，frame=%s；已在回调内发起 mbRunJs" % frame))
            # 给了 cpp 路径就把桥真正注入的那段脚本原样跑一遍，
            # 看它在同一个内核里会不会抛错（用一个全局量把错误带出来）。
            inj_cpp = os.environ.get("MB_INJECT_CPP")
            if inj_cpp:
                src = open(inj_cpp, encoding="utf-8", errors="replace").read()
                begin = src.find('R"JS(')
                finish = src.find(')JS"', begin)
                code = src[begin + 5:finish] if 0 <= begin and finish > begin else ""
                logs.append(("注入脚本", "抽出 %d 字节" % len(code)))
                mb.mbRunJs(
                    view, frame,
                    ("window.__injErr='';try{" + code
                     + "}catch(e){window.__injErr=String((e&&e.stack)||e)}").encode("utf-8"),
                    0, ready_run_cb, None, None)
                mb.mbRunJs(
                    view, frame,
                    b"document.title='INJ send='+(typeof send)+' adopt='+(typeof adopt)"
                    b"+' err='+window.__injErr",
                    0, ready_run_cb, None, None)
        except Exception as exc:  # noqa: BLE001
            logs.append(("文档就绪", f"回调内发起 mbRunJs 抛出 {type(exc).__name__}: {exc}"))

    def on_query(view, param, es, query_id, custom_msg, request):
        # 立刻应答：如果 JS 侧回调在 mbQuery 返回前就拿到值，说明是同步通道。
        text = request.decode("utf-8", "replace") if request else ""
        logs.append(("页面查询", f"{text} -> 已同步应答"))
        # 顺带试一下：在 JS 回调上下文里执行原生脚本能不能生效。
        run_js_sync = getattr(mb, "mbRunJsSync", None)
        if run_js_sync is not None:
            try:
                run_js_sync(view, ready_frame[0], b"document.title='FROM-QUERY-CTX'", 0)
                got = mb.mbGetTitle(view)
                logs.append(("查询内RunJs", "标题=" + (got.decode("utf-8", "replace") if got else "(空)")))
            except Exception as exc:  # noqa: BLE001
                logs.append(("查询内RunJs", f"抛出 {type(exc).__name__}: {exc}"))
        mb.mbResponseQuery(view, query_id, custom_msg, ("pong:" + text).encode("utf-8"))

    def on_blink_ready(view, param, frame):
        # 另一个"文档就绪"回调，号称在 blink 线程上：同样在这一时机发起一次注入。
        def blink_done(v, param, es, value):
            logs.append(("blink就绪RunJs", "回调被触发"))
        try:
            blink_run_cb = RUNJS_DONE(blink_done)
            mb.mbRunJs(view, frame, b"window.__fromBlink=1;", 0, blink_run_cb, None, None)
            logs.append(("blink就绪", "已在此回调内发起 mbRunJs"))
        except Exception as exc:  # noqa: BLE001
            logs.append(("blink就绪", f"抛出 {type(exc).__name__}: {exc}"))

    title_cb = TITLE_CHANGED(on_title)
    ready_cb = DOC_READY(on_ready)
    query_cb = JS_QUERY(on_query)
    blink_ready_cb = DOC_READY(on_blink_ready)

    settings = mb.mbCreateInitSettings()
    mb.mbInit(settings)
    print("mbInit: 完成")

    hwnd = make_host_window(width, height)
    view = mb.mbCreateWebView()
    print(f"mbCreateWebView: {view}")
    if not view:
        print("创建 view 失败，无法继续")
        return 1
    mb.mbSetHandle(view, ctypes.c_void_p(hwnd))
    mb.mbSetAutoDrawToHwnd(view, 0)  # 离屏模式下自己合成，不让内核直接上屏
    mb.mbResize(view, width, height)
    mb.mbShowWindow(view, 1)
    mb.mbOnTitleChanged(view, title_cb, None)
    mb.mbOnDocumentReady(view, ready_cb, None)
    mb.mbOnDocumentReadyInBlinkThread(view, blink_ready_cb, None)
    mb.mbOnJsQuery(view, query_cb, None)

    if args.url:
        # 远程页面：直接看这个新内核处理真实站点时会不会卡（老内核在腾讯首页上会死）。
        target = args.url
    else:
        page_path = Path(tempfile.gettempdir()) / "mb132_smoke.html"
        page_path.write_bytes(PAGE)
        target = page_path.as_uri()
    mb.mbLoadURL(view, target.encode("utf-8"))
    print(f"已加载: {target}")

    started = time.monotonic()
    last_tick = started
    max_gap = 0.0
    ticks = 0
    dc_seen = 0
    probes_done = False
    pump = getattr(win32gui, "PumpWaitingMessages")
    while True:
        pump()
        now = time.monotonic()
        # 决定性问题：mbRunJs/mbRunJsSync 的第 2 个参数到底是「框架句柄」还是「JS 执行上下文」。
        # 本地 mb.h 写的是 mbWebFrameHandle frameId，官网文档写的是 es（由 mbGetGlobalExecByFrame 取）。
        # 两者互斥：只有一种能让脚本真正落到主框架上。逐个试，每次试完都泵一会儿消息再读标题。
        if not probes_done and now - started > 2.5:
            probes_done = True
            run_js_sync = getattr(mb, "mbRunJsSync", None)
            frame = mb.mbWebFrameGetMainFrame(view)
            # 文档里 mbGetGlobalExecByFrame 的第 2 参是 frameName（字符串，null/空串表示主框架），
            # 不是 frameId；两种都取一次，看能不能拿到可用的执行上下文。
            es_by_name = mb.mbGetGlobalExecByFrame(view, None)
            es_by_frame = mb.mbGetGlobalExecByFrame(view, frame)
            logs.append(("框架判定", "isMainFrame=%s frame=%s | execByName=%s execByFrame=%s" % (
                mb.mbIsMainFrame(view, frame), frame, es_by_name, es_by_frame)))
            mb.mbWake(view)

            def pump_for(seconds):
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    pump()
                    time.sleep(0.005)

            def read_title():
                got = mb.mbGetTitle(view)
                return got.decode("utf-8", "replace") if got else "(空)"

            if run_js_sync is None:
                logs.append(("RunJs对照", "内核没有导出 mbRunJsSync"))
            else:
                def run_done_cb(v, param, es, value):
                    # 回调被触发说明内核真的走到"执行脚本"这一步了；顺便把脚本结果读出来。
                    text = "(无)"
                    try:
                        raw = mb.mbJsToString(es, value)
                        text = raw.decode("utf-8", "replace") if raw else "(空)"
                    except Exception as exc:  # noqa: BLE001
                        text = "读取失败 %s" % exc
                    logs.append(("RunJs回调", "被触发，脚本结果=%s" % text))
                # 每次都用不同的标题值，读回来就能知道是哪一次调用真正生效了。
                done_cb = RUNJS_DONE(run_done_cb)
                # 再复现一次桥当前的调用形态：第 5 个参数传真正的 NULL。
                # ctypes 的原型不允许 None，所以按同一地址另建一个放宽的原型来调用。
                raw_addr = ctypes.cast(mb.mbRunJs, c_void_p).value
                runjs_null = RUNJS_NULL(raw_addr)
                # 桥目前用的是「回调传 NULL」的形态；另外再试一份和注入脚本体量相当的长脚本，
                # 排除"短脚本能跑、长脚本跑不动"这种可能。
                long_script = ";".join(
                    "function nmbLong%d(){return %d}" % (i, i) for i in range(400)
                ) + ";document.title='W2-'+typeof nmbLong399;"
                attempts = [
                    # 先看两个就绪回调里发出的脚本到底谁生效了（用全局量判定，避免被页面自己改标题干扰）。
                    ("读注入标志", lambda: mb.mbRunJs(
                        view, frame,
                        b"document.title='DR='+(window.__fromReady||0)+' DB='+(window.__fromBlink||0)",
                        0, done_cb, None, None)),
                    ("async+frameId+nocallback", lambda: runjs_null(
                        view, frame, b"document.title='W1'", 0, None, None, None)),
                    ("async+long+nocallback", lambda: runjs_null(
                        view, frame, long_script.encode("utf-8"), 0, None, None, None)),
                    ("async+long+callback", lambda: mb.mbRunJs(
                        view, frame, long_script.encode("utf-8"), 0, done_cb, None, None)),
                ]
                for label, call in attempts:
                    try:
                        call()
                        pump_for(0.4)
                        logs.append(("RunJs对照", "%s -> 标题=%s" % (label, read_title())))
                    except Exception as exc:  # noqa: BLE001
                        logs.append(("RunJs对照", "%s 抛出 %s: %s" % (label, type(exc).__name__, exc)))


        max_gap = max(max_gap, now - last_tick)
        last_tick = now
        ticks += 1
        if dc_seen < 3:
            dc = mb.mbGetLockedViewDC(view)
            if dc:
                dc_seen += 1
                mb.mbUnlockViewDC(view)
        elapsed = now - started
        if elapsed >= args.seconds:
            break
        time.sleep(0.01)

    title = mb.mbGetTitle(view)
    print("\n---- 回调记录 ----")
    for kind, text in logs:
        print(f"  [{kind}] {text}")
    print("\n---- 现场 ----")
    print(f"  mbGetTitle      : {title.decode('utf-8', 'replace') if title else '(空)'}")
    print(f"  取到 DC 次数    : {dc_seen}（>0 说明离屏绘制通道正常）")
    print(f"  泵消息          : {ticks} 次 / {time.monotonic() - started:.1f}s，最长停顿 {max_gap * 1000:.0f} ms")

    mb.mbDestroyWebView(view)
    print("\n已销毁 view。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
