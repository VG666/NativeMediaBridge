# -*- coding: utf-8 -*-
import ctypes
import threading
import time
from ctypes import wintypes

import cv2


BI_RGB = 0
DIB_RGB_COLORS = 0
HALFTONE = 4
SRCCOPY = 0x00CC0020
WM_VIDEO_FRAME = 0x8000 + 42


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ('biSize', wintypes.DWORD),
        ('biWidth', wintypes.LONG),
        ('biHeight', wintypes.LONG),
        ('biPlanes', wintypes.WORD),
        ('biBitCount', wintypes.WORD),
        ('biCompression', wintypes.DWORD),
        ('biSizeImage', wintypes.DWORD),
        ('biXPelsPerMeter', wintypes.LONG),
        ('biYPelsPerMeter', wintypes.LONG),
        ('biClrUsed', wintypes.DWORD),
        ('biClrImportant', wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [('bmiHeader', BITMAPINFOHEADER), ('bmiColors', wintypes.DWORD * 3)]


gdi32 = ctypes.windll.gdi32
gdi32.StretchDIBits.argtypes = [
    ctypes.c_void_p,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    ctypes.c_void_p,
    ctypes.POINTER(BITMAPINFO),
    ctypes.c_uint,
    ctypes.c_ulong,
]
gdi32.StretchDIBits.restype = ctypes.c_int
gdi32.SetStretchBltMode.argtypes = [ctypes.c_void_p, ctypes.c_int]
gdi32.SetStretchBltMode.restype = ctypes.c_int
gdi32.SetBrushOrgEx.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
gdi32.SetBrushOrgEx.restype = ctypes.c_int


class OpenCVVideo:
    """OpenCV software video decoder with synchronized frame presentation."""

    def __init__(self, hwnd, width=None, height=None):
        self.hwnd = hwnd
        self.width = int(width or 0)
        self.height = int(height or 0)
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._capture = None
        self._thread = None
        self._frame = None
        self._bmi = None
        self._fps = 25.0
        self._duration = 0.0
        self._position = 0.0
        self._paused = False
        self._ended = False
        self._closed = False

    def play(self, source=None):
        if source is None:
            with self._lock:
                if self._closed or self._capture is None:
                    return False
                self._paused = False
                self._ended = False
            self._wake.set()
            return True

        self._stop_decoder()
        capture = cv2.VideoCapture(str(source), cv2.CAP_FFMPEG)
        if not capture.isOpened():
            capture.release()
            raise RuntimeError('OpenCV 无法打开视频: %s' % source)

        fps = float(capture.get(cv2.CAP_PROP_FPS))
        if not fps or fps <= 0 or fps > 240:
            fps = 25.0
        frame_count = float(capture.get(cv2.CAP_PROP_FRAME_COUNT))
        duration = frame_count / fps if frame_count > 0 else 0.0
        with self._lock:
            if self._closed:
                capture.release()
                raise RuntimeError('OpenCV 视频后端已关闭')
            self._capture = capture
            self._fps = fps
            self._duration = duration
            self._position = 0.0
            self._paused = False
            self._ended = False
            self._frame = None
            self._thread = threading.Thread(target=self._decode_loop, name='OpenCVVideo', daemon=True)
            thread = self._thread
        thread.start()
        return True

    def _decode_loop(self):
        deadline = time.perf_counter()
        while True:
            with self._lock:
                capture = self._capture
                closed = self._closed
                paused = self._paused
                fps = self._fps
            if closed or capture is None:
                break
            if paused:
                self._wake.wait(0.1)
                self._wake.clear()
                deadline = time.perf_counter()
                continue

            ok, frame = capture.read()
            if not ok:
                with self._lock:
                    self._ended = True
                    self._paused = True
                continue

            bgra = cv2.cvtColor(frame, cv2.COLOR_BGR2BGRA)
            height, width = bgra.shape[:2]
            position = float(capture.get(cv2.CAP_PROP_POS_MSEC)) / 1000.0
            if position <= 0:
                position = float(capture.get(cv2.CAP_PROP_POS_FRAMES)) / fps
            bmi = self._make_bitmap_info(width, height)
            with self._lock:
                if self._closed or capture is not self._capture:
                    break
                self.width = width
                self.height = height
                self._frame = bgra
                self._bmi = bmi
                self._position = max(0.0, position)
            if self.hwnd:
                ctypes.windll.user32.PostMessageW(self.hwnd, WM_VIDEO_FRAME, 0, 0)

            deadline += 1.0 / fps
            delay = deadline - time.perf_counter()
            if delay > 0:
                self._wake.wait(delay)
                self._wake.clear()
            else:
                deadline = time.perf_counter()

    @staticmethod
    def _make_bitmap_info(width, height):
        bmi = BITMAPINFO()
        bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bmi.bmiHeader.biWidth = width
        bmi.bmiHeader.biHeight = -height
        bmi.bmiHeader.biPlanes = 1
        bmi.bmiHeader.biBitCount = 32
        bmi.bmiHeader.biCompression = BI_RGB
        bmi.bmiHeader.biSizeImage = width * height * 4
        return bmi

    def pause(self, paused=True):
        with self._lock:
            if self._capture is None:
                return False
            self._paused = bool(paused)
        self._wake.set()
        return True

    def seek(self, seconds):
        with self._lock:
            capture = self._capture
            if capture is None:
                return False
            target = max(0.0, min(float(seconds), self._duration or float(seconds)))
            ok = capture.set(cv2.CAP_PROP_POS_MSEC, target * 1000.0)
            if ok:
                self._position = target
                self._ended = False
        self._wake.set()
        return bool(ok)

    def set_volume(self, volume):
        return False

    def set_muted(self, muted):
        return False

    def state(self):
        with self._lock:
            return {
                'time': self._position,
                'duration': self._duration,
                'playing': self._capture is not None and not self._paused and not self._ended,
                'ended': self._ended,
            }

    def paint(self, hdc, dest_rect):
        with self._lock:
            frame = self._frame
            bmi = self._bmi
            if frame is None or bmi is None:
                return False
            src_height, src_width = frame.shape[:2]
            dest_width = dest_rect.Right - dest_rect.Left
            dest_height = dest_rect.Bottom - dest_rect.Top
            if dest_width <= 0 or dest_height <= 0:
                return False
            scale = min(float(dest_width) / src_width, float(dest_height) / src_height)
            draw_width = max(1, int(src_width * scale))
            draw_height = max(1, int(src_height * scale))
            draw_x = dest_rect.Left + (dest_width - draw_width) // 2
            draw_y = dest_rect.Top + (dest_height - draw_height) // 2
            gdi32.SetStretchBltMode(hdc, HALFTONE)
            gdi32.SetBrushOrgEx(hdc, 0, 0, None)
            result = gdi32.StretchDIBits(
                hdc,
                draw_x, draw_y, draw_width, draw_height,
                0, 0, src_width, src_height,
                ctypes.c_void_p(frame.ctypes.data),
                ctypes.byref(bmi),
                DIB_RGB_COLORS,
                SRCCOPY,
            )
            return result != 0

    def _stop_decoder(self):
        with self._lock:
            capture = self._capture
            thread = self._thread
            self._capture = None
            self._thread = None
            self._paused = True
        self._wake.set()
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=2.0)
        if capture is not None:
            capture.release()

    def close(self):
        with self._lock:
            if self._closed:
                return
            self._closed = True
        self._stop_decoder()
        with self._lock:
            self._frame = None
            self._bmi = None
