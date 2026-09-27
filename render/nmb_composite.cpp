/**
 * @file nmb_composite.cpp
 * @brief 画面合成：paintVideo 帧缩放、paintBrowser 页面+视频帧离屏合成（控件条挖洞/硬洞/软洞抠像）、onWebViewPainted、合成诊断开关。
 *
 * 由 native_media_bridge.cpp 按职责拆分（P1-P5 重构）；逻辑未改。
 * 内部实现一律在 namespace nmb，跨模块接口集中声明于 nmb_internal.h。
 */
#include "core/nmb_internal.h"

namespace nmb {
bool paintVideo(Media* media, HDC dc, const RectI& rect) {
    std::vector<unsigned char> pixels;
    int frameWidth = 0;
    int frameHeight = 0;
    {
        std::lock_guard<std::mutex> lock(media->frameMutex);
        pixels = media->bgra;
        frameWidth = media->frameWidth;
        frameHeight = media->frameHeight;
    }
    if (pixels.empty() || frameWidth <= 0 || frameHeight <= 0 || rect.width <= 0 || rect.height <= 0 ||
        pixels.size() < static_cast<size_t>(frameWidth) * static_cast<size_t>(frameHeight) * 4)
        return false;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = frameWidth;
    bmi.bmiHeader.biHeight = -frameHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    const double scale = std::min(static_cast<double>(rect.width) / frameWidth,
                                  static_cast<double>(rect.height) / frameHeight);
    const int width = std::max(1, static_cast<int>(frameWidth * scale));
    const int height = std::max(1, static_cast<int>(frameHeight * scale));
    // COLORONCOLOR：直接丢弃被缩掉的行/列。HALFTONE 会在这条路径上做真正的滤波，
    // 1280x720 → 900x620 这种缩放下单帧要十几到几十毫秒，叠加在每一帧视频上就是发涩、拖窗口时更明显。
    // 视频帧本身是连续信号（不像界面文字有高频细节），0.7x 这个比例下最近邻采样的观感差异很小，
    // 换来的是每帧一次便宜的 StretchDIBits。
    SetStretchBltMode(dc, COLORONCOLOR);
    const int lines = StretchDIBits(dc, rect.x + (rect.width - width) / 2, rect.y + (rect.height - height) / 2,
                  width, height, 0, 0, frameWidth, frameHeight, pixels.data(), &bmi,
                  DIB_RGB_COLORS, SRCCOPY);
    return lines != 0 && lines != GDI_ERROR;
}
// 诊断开关：把"本模块确实合成过视频矩形"这件事标成洋红（默认关闭，零行为变化）。
// 用法：视频帧照常画在标记色上面，所以正常情况下这个颜色永远看不到；一旦在页面上看到它，
// 就说明这一帧是**本模块**画的、而 paintVideo 提前返回了（取不到帧）。反过来，若某一帧是
// 页面底色却没有这个标记色，就说明有**另一个绘制者**在本模块的合成之后把页面画到了窗口上。
// 滚动时"闪一下页面"只可能有这两个来源，只有这个开关能把它们分开（实测源 oceans.mp4 里
// 不会有洋红这种高饱和色，探针的亮度列上一眼可辨：洋红≈170、纯绿页面底≈85）。
bool markVideoRectEnabled() {
    static const bool enabled = [] {
        char buffer[8]{};
        return GetEnvironmentVariableA("NMB_MARK_VIDEO_RECT", buffer, sizeof(buffer)) > 0;
    }();
    return enabled;
}
// 诊断开关：把每次合成对视频的判定追加写进一个文件（默认关闭，零行为变化）。
// 这一条日志专门用来分开两种在屏幕上**完全一样**的现象（都是"视频区露出页面底色"）：
//   ①本模块这一帧跳过了视频 -> 日志里必然出现 empty / offscreen / novideo；
//   ②本模块把帧画上去了、却被**另一个绘制者**在之后盖掉 -> 日志里全是 drawn。
// 滚动时"闪一下页面"只可能有这两个来源，光看屏幕（或看探针亮度）永远分不开。
const char* paintLogPath() {
    static const std::string path = [] {
        char buffer[512]{};
        const DWORD n = GetEnvironmentVariableA("NMB_PAINT_LOG", buffer, sizeof(buffer));
        return (n > 0 && n < sizeof(buffer)) ? std::string(buffer, n) : std::string();
    }();
    return path.empty() ? nullptr : path.c_str();
}
// 诊断开关：在与视频**无关**的固定位置画一块洋红（默认关闭，零行为变化）。
// 它只能被"本模块之外的人"擦掉：屏幕上看不到它，就说明本模块的合成不是窗口的最后一个绘制者。
// 位置挑在页面恒定白底（滚动条带）上，洋红≈170 与白/浅蓝 244~255 在亮度列上一眼可辨。
bool markPageDotEnabled() {
    static const bool enabled = [] {
        char buffer[8]{};
        return GetEnvironmentVariableA("NMB_MARK_PAGE_DOT", buffer, sizeof(buffer)) > 0;
    }();
    return enabled;
}
// 将离屏 WebView 的画面与视频帧合成后绘制到宿主窗口。
void paintBrowser(Browser* browser, HDC dc, const RECT& target) {
    const int width = target.right - target.left;
    const int height = target.bottom - target.top;
    if (width <= 0 || height <= 0) return;

    // 缓冲只在尺寸变化时重建：见 Browser::paintDC 的注释。旧的先换出来再删（SelectObject 之后它仍归我们所有）。
    // 两块缓冲都用 32 位 top-down DIB section：除了照常供 GDI 选用，还能直接拿像素指针——
    // 软洞（透明背景上的弹幕/图标）只能逐像素从页面备份抠回，兼容位图给不了指针。
    if (!browser->paintDC) {
        browser->paintDC = CreateCompatibleDC(dc);
        if (!browser->paintDC) return;
        browser->pageDC = CreateCompatibleDC(dc);
        if (!browser->pageDC) return;
    }
    auto makeDib = [](int dibWidth, int dibHeight, void** bits) -> HBITMAP {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = dibWidth;
        bmi.bmiHeader.biHeight = -dibHeight; // 负值：top-down，行序与窗口坐标一致
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        return CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, bits, nullptr, 0);
    };
    if (!browser->paintBitmap || browser->paintWidth != width || browser->paintHeight != height) {
        void* freshPaintBits = nullptr;
        HBITMAP fresh = makeDib(width, height, &freshPaintBits);
        if (!fresh) return;
        void* freshPageBits = nullptr;
        HBITMAP freshPage = makeDib(width, height, &freshPageBits);
        if (!freshPage) { DeleteObject(fresh); return; }
        HGDIOBJ previous = SelectObject(browser->paintDC, fresh);
        if (!browser->paintBitmap) browser->paintOriginal = previous;
        else DeleteObject(browser->paintBitmap);
        HGDIOBJ previousPage = SelectObject(browser->pageDC, freshPage);
        if (!browser->pageBitmap) browser->pageOriginal = previousPage;
        else DeleteObject(browser->pageBitmap);
        browser->paintBitmap = fresh;
        browser->pageBitmap = freshPage;
        browser->paintBits = freshPaintBits;
        browser->pageBits = freshPageBits;
        browser->paintWidth = width;
        browser->paintHeight = height;
    }
    HDC buffer = browser->paintDC;
    HDC page = browser->pageDC;
    RECT area{0, 0, width, height};
    FillRect(buffer, &area, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    if (browser->view && g_kernel.getViewDC && g_kernel.unlockViewDC) {
        HDC source = g_kernel.getViewDC(browser->view);
        if (source) {
            BitBlt(buffer, 0, 0, width, height, source, target.left, target.top, SRCCOPY);
            // 同一份页面像素另存一页：软洞抠像要在视频帧盖上去之后还能取回原页面内容。
            BitBlt(page, 0, 0, width, height, source, target.left, target.top, SRCCOPY);
            g_kernel.unlockViewDC(browser->view);
        }
    }
    const char* const diagPath = paintLogPath();
    std::string diag;
    int diagMedia = 0;
    auto diagNote = [&](const char* what, const Media* m, const RectI& r) {
        if (!diagPath || diagMedia >= 4) return;
        ++diagMedia;
        char buf[128];
        snprintf(buf, sizeof(buf), " %s id=%ls rect=%d,%d,%d,%d", what,
                 m ? m->id.c_str() : L"", r.x, r.y, r.width, r.height);
        diag += buf;
    };
    {
        std::lock_guard<std::mutex> lock(browser->mutex);
        for (const auto& item : browser->media) {
            Media* media = item.second;
            if (!media || media->kind != L"video") continue;
            RectI rect = media->rect;
            rect.x -= target.left;
            rect.y -= target.top;
            if (rect.width <= 0 || rect.height <= 0) { diagNote("empty", media, media->rect); continue; }
            // 元素已经整体滚出可见区域：不要再画，否则画面会留在窗口边缘。
            if (rect.x + rect.width <= 0 || rect.y + rect.height <= 0 || rect.x >= width || rect.y >= height) { diagNote("offscreen", media, rect); continue; }
            // 画面只画"元素在页面里真正可见的那块"（Media::clipRect＝页面侧 clipBox 的结果，
            // 元素盒 ∩ overflow 裁剪祖先 ∩ 视口）。先转窗口坐标与目标窗口求交判空——
            // 交集为空＝元素被站点容器完全藏住（rect 还在视口里、页面却看不见它），帧若照画
            // 就是叠在别的元素上（用户报的"视频内容没锁死在组件内部"的极端形态）。
            const RectI& clip = media->clipRect;
            if (clip.width <= 0 || clip.height <= 0) { diagNote("noclip", media, media->rect); continue; }
            const int tx = static_cast<int>(target.left), ty = static_cast<int>(target.top);
            const int clipX1 = std::max(0, clip.x - tx);
            const int clipY1 = std::max(0, clip.y - ty);
            const int clipX2 = std::min(width, clip.x - tx + clip.width);
            const int clipY2 = std::min(height, clip.y - ty + clip.height);
            if (clipX2 <= clipX1 || clipY2 <= clipY1) { diagNote("noclip", media, media->rect); continue; }
            const int saved = SaveDC(buffer);
            // 控件条由页面绘制、画不到原生帧上面，所以这里把它的位置从视频帧里挖掉。
            // 位置＝上报表里的 barRect，不做任何加减：页面侧条的 DOM 坐标与这份上报是
            // 同一处算出的同一份数字（placeBar 直接写元素 rect 坐标），条与洞永远一致。
            // （曾经在这里按"当前 rect＋常量偏移"修正定位，已按用户要求整体回退。）
            const RectI& bar = media->barRect;
            if (bar.width > 0 && bar.height > 0)
                ExcludeClipRect(buffer, bar.x - target.left, bar.y - target.top,
                                bar.x - target.left + bar.width, bar.y - target.top + bar.height);
            // 站点自绘 UI（控制栏、弹幕、弹窗、遮罩）同样必须从帧里挖空：它们是页面 DOM，
            // 由前面的整页 BitBlt 画在缓冲里，视频帧一旦盖上去就再也透不出来。
            for (const RectI& hole0 : media->holes) {
                const RectI hole{hole0.x - target.left, hole0.y - target.top, hole0.width, hole0.height};
                if (hole.width <= 0 || hole.height <= 0) continue;
                ExcludeClipRect(buffer, hole.x, hole.y, hole.x + hole.width, hole.y + hole.height);
            }
            // 元素被站点容器**部分**裁剪：把 GDI 绘制区收到可见交集后照常按完整 rect 绘制
            //（下面的黑底/等比缩放基准都不变），交集之外 GDI 自动不画——容器裁掉多少画面
            // 就少画多少，帧内容不因裁剪跳变。放在挖洞之前或之后都行：GDI 裁剪区是交集语义，
            // 与 ExcludeClipRect 互相独立。
            IntersectClipRect(buffer, clipX1, clipY1, clipX2, clipY2);
            // 诊断标记（默认关闭，见 markVideoRectEnabled）：标了却没被视频帧盖住，
            // 就是"本模块画的、但这一帧没有帧可画"，和"别的绘制者盖掉了本模块"是两回事。
            if (markVideoRectEnabled()) {
                static HBRUSH markBrush = CreateSolidBrush(RGB(255, 0, 255));
                RECT mark{rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
                FillRect(buffer, &mark, markBrush);
            }
            const bool drawn = paintVideo(media, buffer, rect);
            GdiFlush();
            RestoreDC(buffer, saved);
            if (!drawn) { diagNote("noframe-or-blit-failed", media, rect); continue; }
            // 软洞抠像：从视频帧绘制前的页面备份里，把"亮于视频黑底"的像素（弹幕文字、SVG
            // 图标、浅色 logo）按亮度作 alpha 贴回已盖视频帧的缓冲。暗于阈值的像素视为内核给
            // video 元素画的黑底（或半透明黑遮罩），保留视频帧——这是"透明背景元素只露出文字
            // 不拖黑盒"的关键，整块挖空在这里只会得到一条黑矩形。
            if (browser->pageBits && browser->paintBits) {
                const auto* pageBits = static_cast<const uint8_t*>(browser->pageBits);
                auto* compBits = static_cast<uint8_t*>(browser->paintBits);
                for (const RectI& soft0 : media->softHoles) {
                    const int tx = static_cast<int>(target.left);
                    const int ty = static_cast<int>(target.top);
                    const int x1 = std::max({clipX1, rect.x, soft0.x - tx});
                    const int y1 = std::max({clipY1, rect.y, soft0.y - ty});
                    const int x2 = std::min({clipX2, rect.x + rect.width, soft0.x - tx + soft0.width});
                    const int y2 = std::min({clipY2, rect.y + rect.height, soft0.y - ty + soft0.height});
                    if (x2 <= x1 || y2 <= y1) continue;
                    for (int yy = y1; yy < y2; ++yy) {
                        const uint8_t* sp = pageBits + (static_cast<size_t>(yy) * width + x1) * 4;
                        uint8_t* dp = compBits + (static_cast<size_t>(yy) * width + x1) * 4;
                        for (int xx = x1; xx < x2; ++xx) {
                            const auto inside = [&](const RectI& hole) {
                                return xx + tx >= hole.x && yy + ty >= hole.y &&
                                    xx + tx < hole.x + hole.width && yy + ty < hole.y + hole.height;
                            };
                            bool excluded = inside(bar);
                            for (const RectI& hole : media->holes) {
                                if (inside(hole)) { excluded = true; break; }
                            }
                            const int lum = std::max({sp[0], sp[1], sp[2]});
                            if (!excluded && lum > 40) {
                                // 40 以下全透（视频黑底/暗遮罩），255 全不贴回，中间线性，
                                // 给文字抗锯齿边缘留渐变。
                                const int k = (lum - 40) * 256 / 216;
                                dp[0] = static_cast<uint8_t>((sp[0] * k + dp[0] * (256 - k)) >> 8);
                                dp[1] = static_cast<uint8_t>((sp[1] * k + dp[1] * (256 - k)) >> 8);
                                dp[2] = static_cast<uint8_t>((sp[2] * k + dp[2] * (256 - k)) >> 8);
                            }
                            sp += 4; dp += 4;
                        }
                    }
                }
            }
            diagNote("drawn", media, rect);
        }
    }
    // 诊断（见 markPageDotEnabled）：固定位置标记，与本模块画的视频无关。
    if (markPageDotEnabled()) {
        static HBRUSH dotBrush = CreateSolidBrush(RGB(255, 0, 255));
        RECT dot{600 - target.left, 520 - target.top, 720 - target.left, 600 - target.top};
        FillRect(buffer, &dot, dotBrush);
    }
    if (diagPath) {
        if (diagMedia == 0) diagNote("novideo", nullptr, RectI{0, 0, 0, 0});
        if (FILE* f = fopen(diagPath, "a")) {
            fprintf(f, "%lu target=%d,%d,%d,%d%s\n", static_cast<unsigned long>(GetTickCount()),
                    static_cast<int>(target.left), static_cast<int>(target.top), width, height, diag.c_str());
            fclose(f);
        }
    }
    // 合成完一次性上屏：页面与视频帧在同一块缓冲里，不存在"先页面后视频"的中间态。
    BitBlt(dc, target.left, target.top, width, height, buffer, 0, 0, SRCCOPY);
}

// WebView 内部重绘通知：只把对应区域标记为无效，交给 WM_PAINT 合成。
void __cdecl onWebViewPainted(WebView, void* param, HDC, int x, int y, int cx, int cy) {
    HWND hwnd = static_cast<HWND>(param);
    if (!hwnd) return;
    // 统一走消息队列，避免内核绘制线程直接 Invalidate 与宿主线程的 WM_PAINT/视频帧
    // 合成交错。WM_APP+2 会把页面脏区与视频脏区合并后再进入一次 WM_PAINT。
    auto* browser = reinterpret_cast<Browser*>(GetPropW(hwnd, L"NMB_BROWSER"));
    if (browser) {
        if (cx > 0 && cy > 0) {
            RECT rect{x, y, x + cx, y + cy};
            InvalidateRect(hwnd, &rect, FALSE);
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!browser->paintPosted.exchange(true) && !PostMessageW(hwnd, WM_APP + 2, 0, 0))
            browser->paintPosted.store(false);
        return;
    }
    if (cx > 0 && cy > 0) {
        RECT rect{x, y, x + cx, y + cy};
        InvalidateRect(hwnd, &rect, FALSE);
    } else {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

} // namespace nmb
