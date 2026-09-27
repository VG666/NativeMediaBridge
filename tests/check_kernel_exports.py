# -*- coding: utf-8 -*-
"""检查一个 miniblink 内核 DLL 是否提供 NativeMediaBridge 需要的全部导出。

换内核时最容易踩的坑就是"新内核改名/删了某个导出"，这里按桥里的加载代码
逐项核对，并顺带确认这个 DLL 能不能真正加载起来（缺依赖会在这里暴露）。

清单来源：native_media_bridge.cpp 的 NMB_Initialize —— MB(...) 宏是必需项（缺一个就初始化失败），
loadProc(...) 是可选项（取不到就留空指针，调用点逐处判空）。
实测：mb108 的 mb* 导出（124 个）是 mb132（242 个）的真子集，缺的正是下面 DRAW_SWITCHES
这三个"内核自己上屏"的开关，桥里都已按可选处理。

用法：
  python tests\\check_kernel_exports.py "F:\\downloads\\mb108_241102\\bin\\win_x64\\mb108_x64.dll"
"""
import ctypes
import sys
from pathlib import Path

# 桥里 MB(...) 加载的硬性导出：缺一个就 NMB_Initialize 失败。
REQUIRED = [
    "mbCreateInitSettings",
    "mbInit",
    "mbCreateWebView",
    "mbDestroyWebView",
    "mbResize",
    "mbShowWindow",
    "mbSetHandle",
    "mbGetLockedViewDC",
    "mbUnlockViewDC",
    "mbOnDocumentReady",
    "mbWake",
    "mbRunJs",
    "mbOnJsQuery",
    "mbResponseQuery",
]

# 取证脚本（diag_pagejs.py / probe_compat_shim.py 等）直接从内核取的接口：
# 桥本身不需要，但"换内核后还能不能进页面执行 JS"全看它们。
TOOLS = [
    "mbWebFrameGetMainFrame",
    "mbJsToString",
    "mbGetTitle",
]

# 桥里 loadProc(...) 加载的可选项。
OPTIONAL = [
    "mbCreateWebWindow",
    "mbSetHeadlessEnabled",
    "mbSetHandleOffset",
    "mbSetTransparent",
    "mbOnPaintUpdated",
    "mbOnDocumentReadyInBlinkThread",
    "mbOnDidCreateScriptContext",
    "mbFireMouseEvent",
    "mbFireMouseWheelEvent",
    "mbFireKeyDownEvent",
    "mbFireKeyUpEvent",
    "mbFireKeyPressEvent",
    "mbFireWindowsMessage",
    "mbSetFocus",
    "mbKillFocus",
    "mbIsMainFrame",
    "mbSetNavigationToNewWindowEnable",
    "mbSetCspCheckEnable",
    "mbLoadURL",
    "mbLoadHtmlWithBaseUrl",
]

# 换内核时最值得盯的一组：控制"内核要不要自己往窗口上画"。
# mb108 三个都没有（132 才有），而它的 mbCreateWebView 是纯离屏视图、本来就不上屏，
# 所以缺了不影响；反过来若哪天换内核后画面被内核自己盖掉，先回来查这三个。
DRAW_SWITCHES = ["mbSetAutoDrawToHwnd", "mbSetHeadlessEnabled", "mbSetHandleOffset"]

LOAD_WITH_ALTERED_SEARCH_PATH = 0x00000008


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    path = Path(sys.argv[1]).resolve()
    if not path.is_file():
        print(f"找不到 {path}")
        return 2

    print(f"Python 位数: {ctypes.sizeof(ctypes.c_void_p) * 8}")
    print(f"待检内核  : {path}  ({path.stat().st_size} 字节)")

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.LoadLibraryExW.restype = ctypes.c_void_p
    module = kernel32.LoadLibraryExW(str(path), None, LOAD_WITH_ALTERED_SEARCH_PATH)
    if not module:
        err = ctypes.get_last_error()
        print(f"\n加载失败，Win32 错误 {err}（126 通常是缺依赖 DLL，193 通常是位数不对）")
        return 1
    print("加载结果  : 成功")

    kernel32.GetProcAddress.restype = ctypes.c_void_p
    kernel32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    def has(name):
        return bool(kernel32.GetProcAddress(ctypes.c_void_p(module), name.encode("ascii")))

    problems = 0
    for title, names, fatal in (
        ("桥必需导出", REQUIRED, True),
        ("取证脚本用", TOOLS, False),
        ("可选能力", OPTIONAL, False),
        ("上屏开关", DRAW_SWITCHES, False),
    ):
        missing = [n for n in names if not has(n)]
        state = "全部具备" if not missing else ("缺 " + ", ".join(missing))
        print(f"{title:12s}: {state}")
        if missing and fatal:
            problems += 1

    kernel32.FreeLibrary(ctypes.c_void_p(module))
    if problems:
        print("\n结论: 内核缺少桥的必需导出，直接换上去 NMB_Initialize 会失败。")
    else:
        print("\n结论: 必需导出齐全，可以直接替换内核。")
        print("      上屏开关那一组若报缺，说明内核不提供“自动上屏”能力，桥已按可选处理。")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
