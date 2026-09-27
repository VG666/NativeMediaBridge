# -*- coding: utf-8 -*-
"""NativeMediaBridge.dll 的 ctypes 封装。

NativeMediaBridge 当前负责创建并管理自己的 Miniblink WebView；它不能接管
MBPython 已经创建的 WebView。调用方应在主线程初始化，并使用同一个消息循环。
"""

import ctypes
import os
from ctypes import c_int, c_void_p, c_wchar_p
from pathlib import Path


class NativeMediaBridge:
    """NativeMediaBridge.dll 的 Python 句柄式封装（建议 with 使用）。

    生命周期：构造即加载 DLL 并 NMB_Initialize；create_browser() 创建一个
    浏览器实例；load_url/load_html/show/resize 操作；close()/shutdown() 释放。
    默认从 ../NativeMediaBridge/bin 下查找 DLL，所有方法默认作用于最近创建
    的浏览器，也可显式传 browser 句柄。
    """

    def __init__(self, dll_path=None, miniblink_path=None, bass_path=None):
        self._dll_directory = None
        self._dll = None
        self._browser = None
        self._initialized = False
        self._load(dll_path)
        self._configure_api()
        self.initialize(miniblink_path, bass_path)

    @staticmethod
    def _default_dll_path():
        # 要跑别的产物：环境变量优先（NMB_DLL_PATH 给文件本身，NMB_BIN_DIR 给目录），
        # 构造时传 dll_path 仍然最优先（见 __init__）。
        override = os.environ.get("NMB_DLL_PATH")
        if override:
            return Path(override)
        bin_dir = os.environ.get("NMB_BIN_DIR")
        if bin_dir:
            return Path(bin_dir) / "NativeMediaBridge.dll"
        package_dir = Path(__file__).resolve().parent
        candidates = (
            package_dir.parent / "NativeMediaBridge" / "bin" / "NativeMediaBridge.dll",
            package_dir.parent / "tests" / "NativeMediaBridge" / "bin" / "NativeMediaBridge.dll",
            # 本仓库里跑（smoke_test\MBPython\）：bin 就在仓库根，不用靠相邻仓库布局
            package_dir.parent.parent / "bin" / "NativeMediaBridge.dll",
        )
        for candidate in candidates:
            if candidate.is_file():
                return candidate
        return candidates[0]

    def _load(self, dll_path):
        path = Path(dll_path) if dll_path else self._default_dll_path()
        if not path.is_file():
            raise FileNotFoundError(f"找不到 NativeMediaBridge.dll: {path}")
        if hasattr(os, "add_dll_directory"):
            self._dll_directory = os.add_dll_directory(str(path.parent))
        self._dll = ctypes.CDLL(str(path))

    def _configure_api(self):
        dll = self._dll
        dll.NMB_Initialize.argtypes = [c_wchar_p, c_wchar_p]
        dll.NMB_Initialize.restype = c_int
        dll.NMB_CreateBrowser.argtypes = [c_void_p, c_int, c_int, c_int, c_int]
        dll.NMB_CreateBrowser.restype = c_void_p
        dll.NMB_GetWebView.argtypes = [c_void_p]
        dll.NMB_GetWebView.restype = c_void_p
        dll.NMB_GetWindow.argtypes = [c_void_p]
        dll.NMB_GetWindow.restype = c_void_p
        dll.NMB_LoadURL.argtypes = [c_void_p, c_wchar_p]
        dll.NMB_LoadURL.restype = c_int
        dll.NMB_LoadHTML.argtypes = [c_void_p, c_wchar_p]
        dll.NMB_LoadHTML.restype = c_int
        dll.NMB_Resize.argtypes = [c_void_p, c_int, c_int]
        dll.NMB_Resize.restype = None
        dll.NMB_Show.argtypes = [c_void_p, c_int]
        dll.NMB_Show.restype = None
        dll.NMB_SetMaximized.argtypes = [c_void_p, c_int]
        dll.NMB_SetMaximized.restype = c_int
        dll.NMB_IsMaximized.argtypes = [c_void_p]
        dll.NMB_IsMaximized.restype = c_int
        dll.NMB_DestroyBrowser.argtypes = [c_void_p]
        dll.NMB_DestroyBrowser.restype = None
        dll.NMB_Shutdown.argtypes = []
        dll.NMB_Shutdown.restype = None
        dll.NMB_GetLastError.argtypes = []
        dll.NMB_GetLastError.restype = c_wchar_p

    def _error(self, operation):
        detail = self._dll.NMB_GetLastError() or "没有详细错误信息"
        return RuntimeError(f"{operation} 失败：{detail}")

    def initialize(self, miniblink_path=None, bass_path=None):
        if self._initialized:
            return self
        if not self._dll.NMB_Initialize(miniblink_path, bass_path):
            raise self._error("NMB_Initialize")
        self._initialized = True
        return self

    def create_browser(self, parent=None, x=0, y=0, width=1000, height=700):
        if not self._initialized:
            raise RuntimeError("NativeMediaBridge 尚未初始化")
        browser = self._dll.NMB_CreateBrowser(parent, x, y, width, height)
        if not browser:
            raise self._error("NMB_CreateBrowser")
        self._browser = browser
        return browser

    @property
    def browser(self):
        return self._browser

    def get_webview(self, browser=None):
        return self._dll.NMB_GetWebView(browser or self._browser)

    def get_window(self, browser=None):
        return self._dll.NMB_GetWindow(browser or self._browser)

    def load_url(self, url, browser=None):
        handle = browser or self._browser
        if not handle or not self._dll.NMB_LoadURL(handle, str(url)):
            raise self._error("NMB_LoadURL")

    def load_html(self, html, browser=None):
        handle = browser or self._browser
        if not handle or not self._dll.NMB_LoadHTML(handle, str(html)):
            raise self._error("NMB_LoadHTML")

    def resize(self, width, height, browser=None):
        self._dll.NMB_Resize(browser or self._browser, width, height)

    def show(self, visible=True, browser=None):
        self._dll.NMB_Show(browser or self._browser, int(bool(visible)))

    def set_maximized(self, maximized=True, browser=None):
        """最大化/还原独立顶层窗口，返回操作后的最大化状态。"""
        return bool(self._dll.NMB_SetMaximized(browser or self._browser, int(bool(maximized))))

    def is_maximized(self, browser=None):
        return bool(self._dll.NMB_IsMaximized(browser or self._browser))

    def close(self, browser=None):
        handle = browser or self._browser
        if handle:
            self._dll.NMB_DestroyBrowser(handle)
            if handle == self._browser:
                self._browser = None

    def shutdown(self):
        if self._dll is not None and self._initialized:
            self.close()
            self._dll.NMB_Shutdown()
            self._initialized = False
        if self._dll_directory is not None:
            self._dll_directory.close()
            self._dll_directory = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.shutdown()
        return False
