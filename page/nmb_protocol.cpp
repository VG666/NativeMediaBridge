/**
 * @file nmb_protocol.cpp
 * @brief 页面↔native 协议：mediaLoadUrlBegin 拦截内核媒体下载、mediaBridge 处理 mbQuery（open/close/play/pause/seek/volume/rect/holes/title/cursor）。
 *
 * 由 native_media_bridge.cpp 按职责拆分（P1-P5 重构）；逻辑未改。
 * 内部实现一律在 namespace nmb，跨模块接口集中声明于 nmb_internal.h。
 */
#include "core/nmb_internal.h"

namespace nmb {
// 单个页面同时交给原生解码的媒体路数上限：视频路要占一路解码线程和一路音频线程，
// 不设上限时"打开腾讯视频首页"这类页面会一次开出几十路，界面直接卡死。
constexpr size_t kMaxActiveMedia = 6;
// 页面侧一次请求的全部字段（见 kInjection 里的 send），按制表符拼接后用 mbQuery 发过来。
constexpr size_t kFieldOp = 0, kFieldId = 1, kFieldKind = 2, kFieldSource = 3, kFieldValue = 4;
constexpr size_t kFieldUserAgent = 5, kFieldReferer = 6, kFieldCookie = 7, kFieldOrigin = 8;
constexpr size_t kFieldRectX = 9, kFieldRectY = 10, kFieldRectW = 11, kFieldRectH = 12;
constexpr size_t kFieldBarX = 13, kFieldBarY = 14, kFieldBarW = 15, kFieldBarH = 16;
constexpr size_t kFieldClipX = 17, kFieldClipY = 18, kFieldClipW = 19, kFieldClipH = 20;

// 一次写齐 rect/barRect/clipRect。挖洞就用上报表里的 barRect 绝对坐标，画面裁剪就用
// clipRect 绝对坐标——页面侧条的位置、元素的可见区域与这份上报是同一份数字，native
// 不做任何二次推算（"当前 rect＋偏移"的定位修正行为已按用户要求整体回退）。
static void invalidateMediaRect(Media* media, const RectI& rect) {
    if (!media->browser || !media->browser->hwnd || rect.width <= 0 || rect.height <= 0) return;
    RECT dirty{rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
    InvalidateRect(media->browser->hwnd, &dirty, FALSE);
}

void setMediaRects(Media* media, const RectI& rect, const RectI& barRect, const RectI& clipRect) {
    const auto equal = [](const RectI& a, const RectI& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    };
    if (equal(media->rect, rect) && equal(media->barRect, barRect) && equal(media->clipRect, clipRect)) return;
    invalidateMediaRect(media, media->rect);
    invalidateMediaRect(media, media->barRect);
    media->rect = rect;
    media->barRect = barRect;
    media->clipRect = clipRect;
    invalidateMediaRect(media, rect);
    invalidateMediaRect(media, barRect);
}

// No trustworthy request destination in the bound API: preserve fetch/XHR bytes.
// Direct URL/HLS decoding remains independent of this hook.
bool __cdecl mediaLoadUrlBegin(WebView view, void* param, const char* url, void* job) {
    (void)view; (void)param; (void)url; (void)job;
    return false;
}
// 页面 → native 的媒体桥入口：内核换成 mb 接口后，原来的 wkeJsBindFunction 同步绑定没有了，
// 页面改用 window.mbQuery(请求号, 请求, 回调)，native 在这里处理并把结果经 mbResponseQuery 回给
// 那一次调用的回调。回调是异步的，所以页面侧也改成了事件驱动（见 kInjection 里的 send/adopt）。
void __cdecl mediaBridge(WebView view, void* param, Exec exec, int64_t queryId, int customMsg, const char* request) {
    // 宿主钩子分流：本桥的媒体协议是制表符拼接的位置字段（op 在第 0 格，见
    // kFieldOp），首字符不可能是 '{'；而 nw 宿主的页面 RPC 全是 JSON（{"c":...}）。
    // 所以"首字符是 '{'"就是宿主请求的充分判据——转发给宿主钩子，桥不解析、不应答
    // （宿主返回 1 表示它已用 mbResponseQuery 回过包）。
    if (request && request[0] == '{' && hostHooks().onJsQuery) {
        if (hostHooks().onJsQuery(view, hostHooks().param, exec, queryId, customMsg, request)) return;
    }
    auto* browser = static_cast<Browser*>(param);
    const std::vector<std::wstring> fields = splitFields(utf8ToWide(request ? request : ""));
    // 应答：内核的回调本来就只属于这一个请求号，不需要再额外带关联字段。
    auto respond = [&](const std::wstring& json) {
        if (view && g_kernel.responseQuery) g_kernel.responseQuery(view, queryId, customMsg, wideToUtf8(json).c_str());
    };
    if (!browser || mediaBridgeDisabled()) { respond(L"{\"ok\":false}"); return; }
    std::wstring op = field(fields, kFieldOp), id = field(fields, kFieldId), kind = field(fields, kFieldKind), source = field(fields, kFieldSource);
    std::wstring value = field(fields, kFieldValue);
    const std::string userAgent = wideToUtf8(field(fields, kFieldUserAgent));
    const std::string referer = wideToUtf8(field(fields, kFieldReferer));
    const std::string cookie = wideToUtf8(field(fields, kFieldCookie));
    const std::string origin = wideToUtf8(field(fields, kFieldOrigin));
    if (source.rfind(L"file://", 0) == 0)
        source = utf8ToWide(ffmpegSource(source));
    RectI rect{static_cast<int>(number(field(fields, kFieldRectX))), static_cast<int>(number(field(fields, kFieldRectY))),
               static_cast<int>(number(field(fields, kFieldRectW))), static_cast<int>(number(field(fields, kFieldRectH)))};
    RectI barRect{static_cast<int>(number(field(fields, kFieldBarX))), static_cast<int>(number(field(fields, kFieldBarY))),
                  static_cast<int>(number(field(fields, kFieldBarW))), static_cast<int>(number(field(fields, kFieldBarH)))};
    // 元素在页面里真正可见的区域（元素盒 ∩ overflow 裁剪祖先 ∩ 视口）。send 每条消息
    // 都带，无元素通道（__nmbMedia）补全零——全零按"完全不可见、不画"处理。
    RectI clipRect{static_cast<int>(number(field(fields, kFieldClipX))), static_cast<int>(number(field(fields, kFieldClipY))),
                   static_cast<int>(number(field(fields, kFieldClipW))), static_cast<int>(number(field(fields, kFieldClipH)))};
    // 页面标题回写到宿主窗口标题：宿主程序可以直接读到页面当前状态（也用于测试自检回读）。
    if (op == L"title") {
        if (browser->hwnd) SetWindowTextW(browser->hwnd, value.c_str());
        respond(L"{\"ok\":true}");
        return;
    }
    // 档3：hover polyfill 通过 mbQuery 通知当前 cursor 类型（异步，仅 cursor 变化时发）。
    if (op == L"cursor") {
        if (value == L"pointer")      browser->cursorType.store(1);
        else if (value == L"text")   browser->cursorType.store(2);
        else                          browser->cursorType.store(0);
        respond(L"{\"ok\":true}");
        return;
    }
    // 状态改动全程持锁；应答一定放到放锁之后再做——mbResponseQuery 会当场回调页面脚本，
    // 页面脚本有可能立刻再发一次查询进来，占着 browser->mutex 去应答就会把自己锁死。
    const std::wstring json = [&]() -> std::wstring {
        std::lock_guard<std::mutex> lock(browser->mutex);
        // 页面侧主动放弃接管（元素移出 DOM、源换成 blob: 等 sealed 情况）：条目连同覆盖洞一起销毁。
        // 过去 close 落到通用分支里什么都不做，条目和最后一帧都留在原地：视频像素继续盖着站点 UI，
        // 表现为"换源/删除元素后画面冻在原处擦不掉"。真要重开时由 open 重建即可。
        if (op == L"close") {
            auto it = browser->media.find(id);
            if (it != browser->media.end()) {
                Media* media = it->second;
                invalidateMediaRect(media, media->rect);
                invalidateMediaRect(media, media->barRect);
                // 先摘表：paintBrowser 立刻不再画它（解决最后一帧冻屏盖住页面的问题）。
                browser->media.erase(it);
                // open 是 detach 的后台线程，openRunning 期间它正在读写这个对象（openVideo/
                // openAudio 会写 formatContext、起解码线程）。这里 releaseMedia 等于 use-after-free：
                // 慢源、反复换源时线程还没回来，实测腾讯页直接 0xC0000005 / 0xC0000409 崩掉。
                // 只摘表不 delete：open 线程收尾发现自己已不在线上（!listed），成功失败都会自行释放。
                if (media) {
                    media->stop = true;
                    if (!media->openRunning.load()) releaseMediaAsync(media);
                }
            }
            return L"{\"ok\":true}";
        }
        // 未知 id 的普通操作只回状态，不往表里插空条目（重页面上这些空条目会越积越多）。
        auto found = browser->media.find(id);
        if (op != L"open" && (found == browser->media.end() || !found->second))
            return L"{\"ok\":false,\"missing\":1}";
        Media*& media = browser->media[id];
        // 通用 MSE 接管：页面把 SourceBuffer.appendBuffer 的字节（base64 分块）喂进来。
        // 首次到达时建条目并起后台打开线程；之后只把字节追加进对应流的 ringbuffer。
        if (op == L"mse") {
            if (id.empty()) return L"{\"ok\":false}";
            Media* m = browser->media[id];
            if (!m) {
                size_t active = 0;
                for (const auto& it : browser->media) if (it.second) ++active;
                if (active >= kMaxActiveMedia) return L"{\"ok\":false,\"busy\":1}";
                m = new Media(); m->id = id; m->kind = L"video"; m->mseMode = true;
                m->source = L"__nmb_mse__"; // 哨兵存进 source，使 open 分支复用判定统一（详见下方说明）
                m->browser = browser; m->openRunning = true;
                browser->media[id] = m;
                openMediaAsync(m, view, browser->hwnd);
            }
            std::string mime = wideToUtf8(value);
            std::string b64 = wideToUtf8(field(fields, 21));
            std::vector<uint8_t> bytes = base64Decode(b64);
            const bool isVideo = mime.compare(0, 6, "video/") == 0;
            {
                std::lock_guard<std::mutex> lk(m->mseMtx);
                if (isVideo) { m->mseVideo.insert(m->mseVideo.end(), bytes.begin(), bytes.end()); m->mseVideoMime = mime; }
                else { m->mseAudio.insert(m->mseAudio.end(), bytes.begin(), bytes.end()); m->mseAudioMime = mime; }
                m->mseCv.notify_all();
            }
            return L"{\"ok\":true}";
        }
        if (op == L"open") {
            if (source.empty()) return L"{\"ok\":true}";
            // 一页几十个 <video> 各自开解码线程会把界面直接拖死：
            // 到上限就明确拒绝（busy），页面会退避重试，实在排不上就把元素交还给内核自己处理。
            if (!(media && media->source == source)) {
                size_t active = 0;
                for (const auto& item : browser->media) if (item.second) ++active;
                if (active >= kMaxActiveMedia) return L"{\"ok\":false,\"busy\":1}";
            }
            if (media && media->source == source) {
                // 上一次 open 还在后台开（见下面的异步化）：回 busy 让页面退避后再问。
                // 千万不能把半开状态当成功交出去——页面会接管一个还没有解码器的空壳。
                if (media->openRunning) return L"{\"ok\":false,\"busy\":1,\"reason\":\"opening\"}";
                if (!media->openOk && !media->openError.empty()) {
                    // Report once; the next open can retry instead of staying poisoned.
                    Media* failed = media;
                    browser->media.erase(id);
                    releaseMediaAsync(failed);
                    return L"{\"ok\":false,\"error\":\"open-failed\"}";
                }
                // 同一个地址被页面重新打开（站点重建播放器、src 重设成同一地址、用户点重试）：
                // 条目是复用的，连带上一轮的熔断标志一起复用，页面这边拿了 ok:true 就去 adopt，
                // native 那边解码线程却还停在"出错不出帧"那一支——表现就是"重新打开也放不动，
                // 但缓冲还在长"。用户重新发起就是一次重试，这里必须把熔断清掉。
                media->error = false;
                media->stalled = false;
                setMediaRects(media, rect, barRect, clipRect);
                return mediaStateJson(media);
            }
            // open 必须异步。avformat_open_input/find_stream_info 是无超时的网络阻塞读，过去直接跑在
            // 这条查询路径上（内核在界面线程里回调 mediaBridge，这里还占着 browser->mutex），源站一慢
            // 整个界面就冻住：表现是音频元素的 open 永远不回、控件条永远建不出来（用户报的
            // "音频进度条不显示"，视频快所以从不显形），套件里则是看门狗报"界面无响应"。现在查询线程
            // 只登记条目并立刻回 busy，真正的打开挪去后台线程；页面按 busy 退避重问，开成之后的那次
            // 重问拿到真实状态并接管。换源挤掉的旧条目不在这里 release——它的 open 可能还在天上，
            // 收尾时发现表里没有自己会自行释放。
            const bool evictOpen = media && media->openRunning;
            if (media) {
                invalidateMediaRect(media, media->rect);
                invalidateMediaRect(media, media->barRect);
                media->stop = true;
            }
            if (media && !evictOpen) releaseMediaAsync(media);
            media = new Media(); media->id=id; media->kind=kind; media->source = source;
            if (source == L"__nmb_mse__") media->mseMode = true; // source 已是哨兵，复用判定走统一路径
            setMediaRects(media, rect, barRect, clipRect);
            media->userAgent = userAgent; media->referer = referer; media->cookie = cookie; media->origin = origin;
            media->browser = browser;
            invalidateMediaRect(media, rect);
            media->openRunning = true;
            openMediaAsync(media, view, browser->hwnd);
            return L"{\"ok\":false,\"busy\":1}";
        } else if (media) {
            if (media->openRunning && op != L"rect" && op != L"holes") return L"{\"ok\":false,\"busy\":1,\"reason\":\"opening\"}";
            // 几何的权威只有两个来源：open（建条目那一次）和**变化驱动**的上报（rect/holes）。
            // 页面侧的 send() 每条消息都顺手带一份"当下读到的 rect/barRect"，若照单全收，就等于
            // 让页面脚本每次例行轮询（state/audio 每 200ms 一次）都用绝对坐标把画面和洞重摆一遍——
            // 布局根本没动也摆，用户看到的就是"进度条和视频内容一直在更新"。这里改成只认 rect/holes：
            // 没动就不碰几何，画面/洞只跟随 CSS 布局真正变化的那一次上报。
            const bool geomOp = (op == L"rect" || op == L"holes");
            if (geomOp)
                setMediaRects(media, rect, barRect, clipRect);
            if (op == L"rect") notifyComposite(browser);
            if (op == L"holes") {
                // 页面覆盖物矩形列表（见 Media::holes/softHoles 的注释）。
                // 格式 <h|s>x,y,w,h;…，h=硬洞（整块让开），s=软洞（透明底文字/图标，抠像回贴）。
                // 解析保持宽松：坏段直接丢，不能让一个怪值挡住整条上报。
                std::vector<RectI> hard, soft;
                const wchar_t* cursor = value.c_str();
                while (*cursor && hard.size() + soft.size() < 128) {
                    const wchar_t kind = *cursor;
                    if (kind == L'h' || kind == L's') ++cursor;
                    long nums[4]{};
                    bool good = true;
                    for (int i = 0; i < 4; ++i) {
                        wchar_t* end = nullptr;
                        nums[i] = wcstol(cursor, &end, 10);
                        if (end == cursor) { good = false; break; }
                        cursor = end;
                        if (i < 3) {
                            if (*cursor != L',') { good = false; break; }
                            ++cursor;
                        }
                    }
                    if (good && nums[2] > 0 && nums[3] > 0) {
                        RectI rect0{static_cast<int>(nums[0]), static_cast<int>(nums[1]),
                                    static_cast<int>(nums[2]), static_cast<int>(nums[3])};
                        (kind == L's' ? soft : hard).push_back(rect0);
                    }
                    while (*cursor && *cursor != L';') ++cursor;
                    if (*cursor == L';') ++cursor;
                }
                media->holes = std::move(hard);
                media->softHoles = std::move(soft);
                // 覆盖物变化（弹窗弹出、控制栏淡入）要立刻让画面让开，不等下一次 rect 移动。
                notifyComposite(browser);
            }
            if (op == L"play") {
                std::lock_guard<std::mutex> commit(media->decodeMutex);
                // 用户按下播放**就是一次明确的重试**：把熔断标志一起清掉。
                // 不清的话现场是这样：源卡一下 → 15 秒判死置 error → 解码线程永久停在"不出帧"那一支，
                // 而且每 20ms 把 paused 按回 true，于是用户再点多少次播放都会被立刻抹掉——画面永远不动，
                // 可解复用线程照旧在读、缓冲还在长（用户原话："卡死之后再点也开不了，即使已经缓冲到更长的位置"）。
                // 过去唯一的解药是拖一下进度条（seekTo 里会清 error），这种"拖一下活了"的怪现象就是这个原因。
                media->error = false;
                media->stalled = false;
                // 播完之后再次播放：先回到开头，否则解码线程会立刻又读到文件末尾。
                if (media->ended.load() && mediaDuration(media) > 0 &&
                    mediaPosition(media) >= mediaDuration(media) - 0.3) {
                    // 定位只有一条通道（demuxLoop 在唯一那条连接上做），音视频必然同落点。
                    media->seeking = true;
                    media->seekSeconds.store(0.0);
                    media->seekPending.store(true);
                }
                media->paused = false;
                media->ended = false;
                media->audio.finished = false;
                media->audio.playing = true;
            }
            else if (op == L"pause") {
                media->paused = true;
                media->audio.playing = false;
                // 这里**不再**直接 waveOutReset。清声卡必须和"把音频时钟倒回用户听到的位置"
                // 一起做，而那件事只能由 audioLoop 完成（它才知道时钟和声卡缓冲的真实状态，
                // 见 nmb_audio.cpp 的暂停分支）。从查询线程抢先 reset 的话，声卡是清了，
                // 可时钟还记着那几块没播的样本——再点播放，画面直接跳到声音前面去，
                // 而且因为两边都以自己的基准往前跑，这个偏差再也纠不回来。
                // audioLoop 每 10ms 一轮，暂停后最多一个缓冲周期（46ms）内彻底安静。
            }
            else if (op == L"volume") {
                // 音量做成**逐流软件增益**（写卡前缩 PCM），不要用 waveOutSetVolume。
                // waveOutSetVolume 是**设备级**的：本桥所有流都开在 WAVE_MAPPER 上，页面上任何一个元素
                // 被静音（移动站 autoplay muted、被站点静音的预览/广告）都会把这条设备音量打到 0，
                // 连主视频一起哑掉（不少驱动还会顺手改系统波形音量）。而 waveOutWrite 照旧成功、
                // audio.position 照旧前进 —— 表现就是"画面在放、一点声音都没有"，且从页面侧看不出来。
                // 另外这里不再限定 audio.waveOut：站点可能在设备打开之前/之后设音量，值要先存下来。
                media->audio.volume.store(static_cast<int>(std::max(0.0, std::min(1.0, number(value, 1))) * 1000.0 + 0.5));
            }
            else if (op == L"seek") {
                std::lock_guard<std::mutex> commit(media->decodeMutex);
                media->seeking = true;
                const double seconds = std::max(0.0, number(value));
                // 定位统一交给 demuxLoop：它在那唯一一条连接上做一次 avformat_seek_file，
                // 视频与音频因此落在同一个点上（从前两条连接各定各的位，音画必然错开）。
                // 它会先置 rebuffering（先暂停），灌够水位再恢复播放——交给后台线程做，
                // 也就不会把 UI 线程堵在网络读取上。
                media->seekSeconds.store(seconds);
                media->seekPending.store(true);
                media->position = seconds;
            }
        }
        return mediaStateJson(media);
    }();
    respond(json);
}

} // namespace nmb
