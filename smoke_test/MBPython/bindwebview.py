# -*- coding:utf-8 -*-
"""WebView 与宿主窗口的绑定、离屏画面上屏与窗口过程接管。

BindWebview.bind_webview(hwnd) 在内核里创建 webView，把它“贴”到给定 HWND
上：内核离屏渲染出的位图通过 wkeOnPaintUpdated 回调（__paint_func）贴到
窗口 DC；同时子类化窗口过程（__myWndProcCallBack），把鼠标/键盘/焦点等
消息转发给内核，并处理透明绘制（__transparentPaint）、命中测试
（__on_nchittest）等自绘窗口细节。set_video_bottom_inset 用于给页面底部
预留原生控件条高度。
"""
from ctypes import (
    windll,
    byref,
    c_void_p,
    c_int,
    c_uint,
    c_ulong
)
from ctypes.wintypes import RGB
from .winConst import WinConst
from .wkeStruct import (Rect,mPos,mSize,COMPOSITIONFORM,bitMap,blendFunction,PAINTSTRUCT)
from .method import method
from .callback import CallBack
from .wndproc import WndProcHook
from . import _LRESULT

gdi32=windll.gdi32
user32 = windll.user32
imm32 = windll.imm32

user32.InvalidateRect.argtypes = [c_void_p, c_void_p, c_int]
user32.InvalidateRect.restype = c_int
imm32.ImmGetContext.argtypes = [c_void_p]
imm32.ImmGetContext.restype = c_void_p
imm32.ImmReleaseContext.argtypes = [c_void_p, c_void_p]
imm32.ImmReleaseContext.restype = c_int
imm32.ImmSetCompositionWindow.argtypes = [c_void_p, c_void_p]
imm32.ImmSetCompositionWindow.restype = c_int

user32.BeginPaint.argtypes = [c_void_p, c_void_p]
user32.BeginPaint.restype = c_void_p
user32.EndPaint.argtypes = [c_void_p, c_void_p]
user32.EndPaint.restype = c_int

gdi32.CreateCompatibleDC.argtypes = [c_void_p]
gdi32.CreateCompatibleDC.restype = c_void_p
gdi32.CreateCompatibleBitmap.argtypes = [c_void_p, c_int, c_int]
gdi32.CreateCompatibleBitmap.restype = c_void_p
gdi32.SelectObject.argtypes = [c_void_p, c_void_p]
gdi32.SelectObject.restype = c_void_p
gdi32.BitBlt.argtypes = [
    c_void_p, c_int, c_int, c_int, c_int,
    c_void_p, c_int, c_int, c_ulong,
]
gdi32.BitBlt.restype = c_int
gdi32.DeleteObject.argtypes = [c_void_p]
gdi32.DeleteObject.restype = c_int
gdi32.DeleteDC.argtypes = [c_void_p]
gdi32.DeleteDC.restype = c_int
gdi32.GetCurrentObject.argtypes = [c_void_p, c_uint]
gdi32.GetCurrentObject.restype = c_void_p
gdi32.GetObjectA.argtypes = [c_void_p, c_int, c_void_p]
gdi32.GetObjectA.restype = c_int

class BindWebview():
    def __init__(self,miniblink,webview=0):
        global js
        self.mb=miniblink
        self.m_webview=webview
        self.cb=CallBack(miniblink)
        self.cb.wkePaintUpdatedCallback=self.__paint_func
        self.video_backend=None
        self.video_bottom_inset=0
        self._video_frame_rect=Rect(0, 0, 0, 0)
    def set_video_bottom_inset(self, pixels):
        self.video_bottom_inset=max(0, int(pixels))
        if hasattr(self, '_host_hwnd'):
            user32.InvalidateRect(self._host_hwnd, None, False)
    def bind_webview(self,hwnd=0,isTransparent=False,isZoom=True):
        
        if user32.IsWindow(hwnd)==0:
            return 0
        if self.m_webview==0:
           self.m_webview=self.mb.wkeCreateWebView()
        if self.m_webview==0:
            return 0
        self.isZoom=isZoom
        self.mb.wkeSetHandle(self.m_webview,hwnd)
        self.cb.wkeOnPaintUpdated(self.m_webview,hwnd)
        if isTransparent:
            self.mb.wkeSetTransparent(self.m_webview,1)
            exStyle=user32.GetWindowLongW(hwnd,WinConst.GWL_EXSTYLE)
            user32.SetWindowLongW(hwnd,WinConst.GWL_EXSTYLE,exStyle | WinConst.WS_EX_LAYERED)
        else:
            self.mb.wkeSetTransparent(self.m_webview,0)
        

        self._host_hwnd=hwnd
        self._wndproc_hook=WndProcHook(hwnd,self.m_webview)
        self._wndproc_hook.onWndProcCallback=self.__myWndProcCallBack
        self._wndproc_hook.hook_WndProc()


        rc=Rect()
        user32.GetClientRect(hwnd,byref(rc))
        self.mb.wkeResize(self.m_webview,rc.Right - rc.Left, rc.Bottom - rc.Top)
        return self.m_webview
    def __paint_func(self,**kwargs):
        webview=kwargs['webview']
        param=kwargs['param']

        hdc=kwargs['hdc']
        x=kwargs['x']
        y=kwargs['y']
        cx=kwargs['cx']
        cy=kwargs['cy']

        if param==None:return
        hwnd=param
        if (user32.GetWindowLongW(hwnd,WinConst.GWL_EXSTYLE) & WinConst.WS_EX_LAYERED)== WinConst.WS_EX_LAYERED:

            self.__transparentPaint(hwnd, hdc, x, y, cx, cy)
        else:
            rc=Rect(0,0,x+cx,y+cy)
            user32.InvalidateRect(hwnd, byref(rc), False)
    def __myWndProcCallBack(self,hwnd,msg,wParam,lParam):
        if msg==WinConst.WM_PAINT:
   
            if WinConst.WS_EX_LAYERED!=(WinConst.WS_EX_LAYERED & user32.GetWindowLongW(hwnd,WinConst.GWL_EXSTYLE)):
                ps=PAINTSTRUCT()
                hdc=user32.BeginPaint(hwnd,byref(ps))
                try:
                    rcClip = ps.rcPaint
                    rcClient=Rect()
                    user32.GetClientRect(hwnd,byref(rcClient))
                    video_bottom = max(rcClient.Top, rcClient.Bottom - self.video_bottom_inset)
                    self._video_frame_rect = Rect(rcClient.Left, rcClient.Top, rcClient.Right, video_bottom)

                    rcInvalid=Rect()
                    if (rcClip.Right > rcClip.Left) and (rcClip.Bottom > rcClip.Top):
                        user32.IntersectRect(byref(rcInvalid),byref(rcClip),byref(rcClient))
                        srcX = rcInvalid.Left - rcClient.Left
                        srcY = rcInvalid.Top - rcClient.Top
                        destX = rcInvalid.Left
                        destY = rcInvalid.Top
                        width = rcInvalid.Right - rcInvalid.Left
                        height = rcInvalid.Bottom - rcInvalid.Top
                        if width > 0 and height > 0:
                            buffer_dc = gdi32.CreateCompatibleDC(hdc)
                            buffer_bitmap = gdi32.CreateCompatibleBitmap(hdc, width, height)
                            old_bitmap = gdi32.SelectObject(buffer_dc, buffer_bitmap)
                            try:
                                tmp_dc=self.mb.wkeGetViewDC(self.m_webview)
                                try:
                                    gdi32.BitBlt(buffer_dc, 0, 0, width, height, tmp_dc, srcX, srcY, WinConst.SRCCOPY)
                                finally:
                                    self.mb.wkeUnlockViewDC(self.m_webview)
                                if self.video_backend is not None:
                                    video_rect = self._video_frame_rect
                                    dest_rect = Rect(
                                        video_rect.Left - destX,
                                        video_rect.Top - destY,
                                        video_rect.Right - destX,
                                        video_rect.Bottom - destY,
                                    )
                                    self.video_backend.paint(buffer_dc, dest_rect)
                                gdi32.BitBlt(hdc, destX, destY, width, height, buffer_dc, 0, 0, WinConst.SRCCOPY)
                            finally:
                                gdi32.SelectObject(buffer_dc, old_bitmap)
                                gdi32.DeleteObject(buffer_bitmap)
                                gdi32.DeleteDC(buffer_dc)
                finally:
                    user32.EndPaint(hwnd,byref(ps))
                return 0
        elif msg==WinConst.WM_VIDEO_FRAME:
            video_rect = self._video_frame_rect
            if video_rect.Right > video_rect.Left and video_rect.Bottom > video_rect.Top:
                user32.InvalidateRect(hwnd, byref(video_rect), False)
            return 0
        elif msg==WinConst.WM_DESTROY:
            if self.video_backend is not None:
                self.video_backend.close()
                self.video_backend = None
        elif msg==WinConst.WM_ERASEBKGND:
            return 1
        elif msg==WinConst.WM_SIZE:
            width= lParam & 65535
            height= lParam >> 16
            self.mb.wkeResize(self.m_webview,width,height)
            return 0
        elif msg==WinConst.WM_KEYDOWN:
            virtualKeyCode=wParam
            flags=0
            if ((lParam >> 16) & WinConst.KF_REPEAT)!=0:
                flags=flags | 0x4000
            if ((lParam >> 16)  & WinConst.KF_EXTENDED)!=0:
                flags=flags | 0x0100
            if self.mb.wkeFireKeyDownEvent(self.m_webview,virtualKeyCode,flags)!=0:
                return 0
        elif msg==WinConst.WM_KEYUP:
            virtualKeyCode=wParam
            flags=0
            if virtualKeyCode==116:
                self.mb.wkeReload(self.m_webview)
            if ((lParam >> 16) & WinConst.KF_REPEAT)!=0:
                flags=flags | 0x4000
            if ((lParam >> 16) & WinConst.KF_EXTENDED)!=0:
                flags=flags | 0x0100
            if self.mb.wkeFireKeyUpEvent(self.m_webview,virtualKeyCode,flags)!=0:
                return 0
        elif msg==WinConst.WM_CHAR:
            virtualKeyCode=wParam
            flags=0
            if ((lParam >> 16) & WinConst.KF_REPEAT)!=0:
                flags=flags | 0x4000
            if self.mb.wkeFireKeyPressEvent(self.m_webview,virtualKeyCode,flags)!=0:
                return 0
        elif msg in [WinConst.WM_LBUTTONDOWN,WinConst.WM_MBUTTONDOWN,WinConst.WM_RBUTTONDOWN,WinConst.WM_LBUTTONDBLCLK,WinConst.WM_MBUTTONDBLCLK,WinConst.WM_RBUTTONDBLCLK,WinConst.WM_LBUTTONUP,WinConst.WM_MBUTTONUP,WinConst.WM_RBUTTONUP]:
            x=lParam & 65535
            y=lParam >> 16
            flags=0
            if msg in [WinConst.WM_LBUTTONDOWN,WinConst.WM_MBUTTONDOWN,WinConst.WM_RBUTTONDOWN]:
                if user32.GetFocus()!=hwnd:
                    user32.SetFocus(hwnd)
                user32.SetCapture(hwnd)
            elif msg in [WinConst.WM_LBUTTONUP,WinConst.WM_MBUTTONUP,WinConst.WM_RBUTTONUP]:
                user32.ReleaseCapture()
            if (wParam & WinConst.MK_CONTROL)!=0:
                flags=flags | 8
            elif (wParam & WinConst.MK_SHIFT)!=0:
                flags=flags | 4
            elif (wParam & WinConst.MK_LBUTTON)!=0:
                flags=flags | 1
            elif (wParam & WinConst.MK_MBUTTON)!=0:
                flags=flags | 16
            elif (wParam & WinConst.MK_RBUTTON)!=0:
                flags=flags | 2
            self.mb.wkeFireMouseEvent(self.m_webview, msg, x, y, flags)
            return 0

        elif msg==WinConst.WM_MOUSEMOVE:
            x=lParam & 65535
            y=lParam >> 16
            flags=0
            if (wParam & WinConst.MK_LBUTTON)!=0:
                flags=flags | 1
            if self.mb.wkeFireMouseEvent(self.m_webview, msg, x, y, flags)!=0:
                return 0
        elif msg==WinConst.WM_CONTEXTMENU:
            pt=mPos()
            pt.x=lParam & 65535
            pt.y=lParam >> 16
            if pt.x!=-1 and pt.y!=-1:
                user32.ScreenToClient(hwnd,byref(pt))
            flags=0
            if (wParam & WinConst.MK_CONTROL)!=0:
                flags=flags | 8
            if (wParam & WinConst.MK_SHIFT)!=0:
                flags=flags | 4
            if (wParam & WinConst.MK_LBUTTON)!=0:
                flags=flags | 1
            if (wParam & WinConst.MK_MBUTTON)!=0:
                flags=flags | 16
            if (wParam & WinConst.MK_RBUTTON)!=0:
                flags=flags | 2

            if self.mb.wkeFireContextMenuEvent(self.m_webview,pt.x,pt.y, flags)!=0:
                return 0
        elif msg==WinConst.WM_MOUSEWHEEL:
            pt=mPos()
            pt.x=lParam & 65535
            pt.y=lParam >> 16
            user32.ScreenToClient(hwnd,byref(pt))
            delta= wParam >> 16
            flags=0

            if (wParam & WinConst.MK_CONTROL)!=0:
                flags=flags | 8
            if (wParam & WinConst.MK_SHIFT)!=0:
                flags=flags | 4
            if (wParam & WinConst.MK_LBUTTON)!=0:
                flags=flags | 1
            if (wParam & WinConst.MK_MBUTTON)!=0:
                flags=flags | 16
            if (wParam & WinConst.MK_RBUTTON)!=0:
                flags=flags | 2
            if self.mb.wkeFireMouseWheelEvent(self.m_webview,pt.x,pt.y,delta,flags)!=0:
                return 0
        elif msg==WinConst.WM_SETFOCUS:
            user32.SetFocus(hwnd)
            return 0
        elif msg==WinConst.WM_KILLFOCUS:
            self.mb.wkeKillFocus(self.m_webview)
            return 0
        elif msg==WinConst.WM_IME_STARTCOMPOSITION:
            caret=self.mb.wkeGetCaretRect(self.m_webview)
            mposForm=COMPOSITIONFORM()
            mposForm.dwStyle = 2 | 32
            mposForm.ptCurrentPos.x = caret.x
            mposForm.ptCurrentPos.y = caret.y
            hIMC=imm32.ImmGetContext(hwnd)
            imm32.ImmSetCompositionWindow(hIMC,byref(mposForm))
            imm32.ImmReleaseContext(hwnd,hIMC)
            return 0
        elif msg==WinConst.WM_SETCURSOR:
            if self.mb.wkeFireWindowsMessage(self.m_webview,hwnd,WinConst.WM_SETCURSOR,wParam,lParam,0)!=0:
                return 0
        elif msg==WinConst.WM_NCHITTEST:
            if self.isZoom:
                return self.__on_nchittest(hwnd,lParam)
        elif msg==WinConst.WM_INPUTLANGCHANGE:
            return user32.DefWindowProcA(hwnd, msg, _LRESULT(wParam), _LRESULT(lParam))
            
    def __transparentPaint(self,hwnd,hdc,x,y,cx,cy):
        rectDest=Rect()
        user32.GetClientRect(hwnd,byref(rectDest))
        user32.OffsetRect(byref(rectDest),-rectDest.Left,-rectDest.Top)

        width = rectDest.Right - rectDest.Left
        height = rectDest.Bottom - rectDest.Top
        hBitmap = gdi32.GetCurrentObject(hdc, WinConst.OBJ_BITMAP)

        bmp=bitMap()
        bmp.bmType=0
        cbBuffer=gdi32.GetObjectA(hBitmap, 24,0)
        gdi32.GetObjectA(hBitmap, cbBuffer,byref(bmp))
        sizeDest=mSize()
        sizeDest.cx =bmp.bmWidth
        sizeDest.cy =bmp.bmHeight

        hdcScreen = self.mb.wkeGetViewDC(self.m_webview)# user32.GetDC(_LRESULT(hwnd))
        blendFunc32bpp=blendFunction()
        blendFunc32bpp.BlendOp = 0   
        blendFunc32bpp.BlendFlags = 0
        blendFunc32bpp.SourceConstantAlpha = 255
        blendFunc32bpp.AlphaFormat = 1  
        pointSource=mPos()
        callOk = user32.UpdateLayeredWindow(hwnd, hdcScreen, 0, byref(sizeDest), hdc, byref(pointSource), RGB(255,255,255), byref(blendFunc32bpp), WinConst.ULW_ALPHA)
        self.mb.wkeUnlockViewDC(self.m_webview)
    def __on_nchittest(self,hwnd,lParam):
        if user32.IsZoomed(hwnd)!=0:
            return 1
        pt=mPos()
        pt.x=lParam & 65535
        pt.y=lParam >> 16
        user32.ScreenToClient(hwnd,byref(pt))
        rc=Rect()
        user32.GetClientRect(hwnd,byref(rc))
        iWidth = rc.Right - rc.Left
        iHeight = rc.Bottom - rc.Top
        if user32.PtInRect(byref(Rect(5, 0, iWidth - 5, 5)),pt):
            retn=12#HTTOP
        elif user32.PtInRect(byref(Rect(0, 5, 5, iHeight - 5)),pt):
            retn=10#HTLEFT
        elif user32.PtInRect(byref(Rect(iWidth - 5, 5, iWidth, iHeight - 10)),pt):
            retn=11#HTRIGHT
        elif user32.PtInRect(byref(Rect(5, iHeight - 5, iWidth - 10, iHeight)),pt):
            retn=15#HTBOTTOM
        elif user32.PtInRect(byref(Rect(0, 0, 5, 5)),pt):
            retn=13#HTTOPLEFT
        elif user32.PtInRect(byref(Rect(0, iHeight - 5, 5, iHeight)),pt):
            retn=16#HTBOTTOMLEFT
        elif user32.PtInRect(byref(Rect(iWidth - 5, 0, iWidth, 5)),pt):
            retn=14#HTTOPRIGHT
        elif user32.PtInRect(byref(Rect(iWidth - 10, iHeight - 10, iWidth, iHeight)),pt):
            retn=17#HTBOTTOMRIGHT
        else:
            retn=1
        return retn
