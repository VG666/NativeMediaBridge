/**
 * @file nmb_media.cpp
 * @brief 媒体生命周期与状态：releaseMedia/silenceBrowser/notifyComposite、releaseAllMedia、回传页面的播放状态 JSON。
 *
 * 由 native_media_bridge.cpp 按职责拆分（P1-P5 重构）；逻辑未改。
 * 内部实现一律在 namespace nmb，跨模块接口集中声明于 nmb_internal.h。
 */
#include "core/nmb_internal.h"

namespace nmb {
// ── 网络读的可中断与时限（声明见 nmb_internal.h）────────────────────────
// 当前 steady_clock 的纳秒读数。守卫里存的是"纳秒时间点"，不用 time_point 是因为
// 它要放进 atomic，而 MSVC 的 steady_clock 周期（100ns）在别的平台不一样，
// 显式换算成纳秒才能跨平台比较。
long long ioNowNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
// FFmpeg 的中断回调：返回非 0 就中止当前阻塞操作。两把尺子——
// ① media->stop：关闭/换源时立刻中断，线程能及时退出（这是"关页面卡住"的解法）；
// ② guard.until：单次读的时限，源站既不断开也不给数据时也能返回（这是"卡死后还能报错"的解法）。
int interruptIo(void* opaque) {
    auto* guard = static_cast<IoDeadline*>(opaque);
    if (!guard || !guard->media) return 0;
    if (guard->media->stop.load()) return 1;
    const long long until = guard->until.load();
    return until != 0 && ioNowNanoseconds() > until ? 1 : 0;
}
void armIo(IoDeadline& guard, Media* media, int milliseconds) {
    guard.media = media;
    guard.until = ioNowNanoseconds() + static_cast<long long>(milliseconds) * 1000000LL;
}
void disarmIo(IoDeadline& guard) { guard.until = 0; }
// 协议层读超时（微秒）。中断回调负责"能取消"，它负责"协议自己也会超时"：
// 两条路都留着——有的协议不查中断回调，有的在连接/握手阶段就卡住（那时回调还没轮询到）。
void applyIoOptions(AVDictionary** options) {
    av_dict_set(options, "rw_timeout", "10000000", 0);
    av_dict_set(options, "timeout", "10000000", 0);
}
void attachInterruptCallback(AVFormatContext* context, IoDeadline* guard, Media* media) {
    if (!context || !guard) return;
    guard->media = media;
    context->interrupt_callback.callback = &interruptIo;
    context->interrupt_callback.opaque = guard;
}
// 中断回调触发（AVERROR_EXIT）或协议层读超时（ETIMEDOUT）：都不是"解码失败"，
// 而是"这次没拿到数据"。调用方该重试就重试，该放弃本轮就放弃本轮，绝不能判死。
bool isIoInterrupted(int result) {
    return result == AVERROR_EXIT || result == AVERROR(ETIMEDOUT);
}
void releaseMedia(Media* media) {
    if (!media) return;
    media->stop = true;
    media->paused = false;
    media->audio.stop = true;
    media->audio.playing = false;
    // 先收解复用线程：它负责那条连接（阻塞在读上，靠中断回调退场），
    // 连接归它用，必须等它停了再关闭输入。两个解码线程随后收——它们等的是队列，stop 一到就走。
    if (media->demuxer.joinable()) media->demuxer.join();
    if (media->decoder.joinable()) media->decoder.join();
    if (media->audioDecoder.joinable()) media->audioDecoder.join();
    // 媒体已从显示表摘除，重绘由调用方负责；后台回收不再访问窗口。
    closeFfmpegAudio(media->audio);
    closeFfmpegVideo(media->video);
    delete media;
}
void releaseMediaAsync(Media* media) {
    if (!media) return;
    // UI 回调只发停止信号；网络读取和解码可能阻塞，join 必须留给后台线程。
    media->stop = true;
    media->paused = false;
    media->audio.stop = true;
    media->audio.playing = false;
    // 解码线程也会读取 media->browser，不能在它退出前改写该指针。
    // 持有 Browser 保证回收期间对象仍存在。
    auto owner = media->browser ? media->browser->self : std::shared_ptr<Browser>{};
    std::thread([media, owner] { releaseMedia(media); }).detach();
}
// 用户点窗口的关闭按钮时，宿主（MBPython 的 message_loop）并不会走 DestroyBrowser：
// 窗口没了、消息循环还在转，音频线程和 waveOut 缓冲都还活着——听起来就是"关不干净，后台还有声音"。
// 这里只负责"立刻静音"，不做 join：解码线程可能正卡在读网络上，在窗口过程里等它会冻住 UI。
// 真正的释放留给宿主的 NMB_DestroyBrowser / NMB_Shutdown（那时线程已退出，join 立刻返回）。
void silenceBrowser(Browser* browser) {
    if (!browser) return;
    std::lock_guard<std::mutex> lock(browser->mutex);
    for (auto& item : browser->media) {
        Media* media = item.second;
        if (!media) continue;
        media->paused = true;
        media->stop = true;
        media->audio.stop = true;
        media->audio.playing = false;
        // 已经把 PCM 排进声卡但还没播完的那部分，必须 reset 掉才会马上安静。
        if (!media->openRunning && media->audio.waveOut) waveOutReset(media->audio.waveOut);
    }
}
void notifyComposite(Browser* browser) {
    if (!browser || !browser->hwnd) return;
    // 每一帧、每个视频都会调到这里。之前每次都 PostMessage，多路视频时消息队列会被灌满，
    // 每个消息又触发一次整窗重绘，界面直接卡死。
    // 这里合并：上一次通知还没被处理完（WM_APP+2 里清标志）之前，后续请求全部丢弃——
    // 反正最终只会有一次整窗合成，画面不会丢帧。
    if (browser->paintPosted.exchange(true)) return;
    if (!PostMessageW(browser->hwnd, WM_APP + 2, 0, 0)) browser->paintPosted.store(false);
}
// ── §7 页面回调、状态 JSON、环境开关、mbQuery 协议 ──────────────────────
// 释放本浏览器上的全部媒体：解码线程、音频设备、表格条目一起清，并重绘一次擦掉残留的原生画面。
// 调用方不必持锁。
void releaseAllMedia(Browser* browser) {
    if (!browser) return;
    {
        std::lock_guard<std::mutex> lock(browser->mutex);
        for (auto& item : browser->media) {
            // open 还在后台线程里跑的条目不能在这里 delete——后台线程正往里写。
            // 只随 clear() 从表里摘掉，所有权归 open 线程的收尾：它发现表里已经没有自己时自行释放。
            if (item.second && item.second->openRunning) {
                item.second->stop = true;
                continue;
            }
            releaseMediaAsync(item.second);
        }
        browser->media.clear();
    }
    if (browser->hwnd) InvalidateRect(browser->hwnd, nullptr, FALSE);
}
std::wstring numberToWide(double value) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.3f", value);
    return buffer;
}

double mediaDuration(Media* media) {
    if (!media) return 0.0;
    if (media->video.duration > 0) return media->video.duration;
    return media->audio.duration;
}

double mediaPosition(Media* media) {
    if (!media) return 0.0;
    if (media->kind == L"video") return media->position;
    return media->audio.position.load();
}

// 播放状态应答的字段集合。默认构造出来就是"媒体不存在"的状态（missing=1，其余全 0/假）；
// 拿到真实 Media* 后在这份默认值上逐项覆盖即可，序列化逻辑不用再区分两种分支。
struct MediaState {
    bool missing = true;
    double duration = 0.0;
    double position = 0.0;
    // 读前缓冲的水位（秒）和已读字节：页面靠它显示缓冲，测试靠它判断源站够不够快。
    double buffered = 0.0;
    // 已缓冲到的绝对时间点：控件条的"已缓冲"段用它，往回拖进度条时它不该跟着往回缩。
    double bufferedUntil = 0.0;
    long long readBytes = 0;
    bool paused = false;
    bool ended = false;
    bool error = false;
    // 源暂时取不到数据（可自愈，见 Media::stalled 的注释）：页面据此可以把"卡住"显示成
    // "正在等数据/网络"而不是"出错"，而 error 仍是真正的致命失败。
    bool stalled = false;
    // "audio" 的真值条件比字面意思强得多：waveOutOpen/waveOutPrepareHeader 任何一步失败都会
    // closeFfmpegAudio，把 codec 一起置空，所以 audioReady 的含义是"解码器+声卡+缓冲池全都就绪"。
    bool audioReady = false;
    bool videoReady = false;
    // 源视频的真实像素尺寸：页面用 videoWidth/videoHeight 做画面适配、弹幕排版、全屏切换。
    // 内核自己解不了码，这两个值恒为 0，站点播放器会据此走错分支（实测 B站：0x0）。
    // open 成功前帧尺寸还是 0，页面侧按"0=尚不可知"处理，与规范一致。
    int frameWidth = 0;
    int frameHeight = 0;
    // 音轨的独立进度与失败原因：视频元素的 position 走的是视频线程（mediaPosition 对 kind==video
    // 直接返回 media->position），音频写到哪一步、有没有失败，过去在应答里完全不存在——
    // "画面正常但没声音"因此无法从页面侧判定，只能靠肉眼。audioFail 是 ASCII 短语，空串不输出：
    // no-stream（源里本来就没音轨，合法）/ waveout-open / waveout-prepare / codec-open ...
    double audioPosition = 0.0;
    std::string audioFail;
};

// 把真实播放状态回传给页面，页面据此驱动原生播放控件的进度条。
std::wstring mediaStateJson(Media* media) {
    // 1) 先有一份默认状态；2) 有媒体时只覆盖下列字段；3) 最后统一序列化。
    MediaState state;
    if (media) {
        state.missing = false;
        state.duration = mediaDuration(media);
        state.position = mediaPosition(media);
        state.buffered = media->bufferedSeconds.load();
        state.bufferedUntil = media->bufferedUntil.load();
        state.readBytes = media->readBytes.load();
        // 报给页面的 paused 是"用户看到的暂停"：手动暂停 或 正在等数据（rebuffer）。
        // 为什么要合并：rebuffer 期间解码线程刻意不再出帧（否则画面会追上并超过数据供给，越播越空），
        // 声音那侧也刚被 waveOutReset 清掉。若这里只报 media->paused，页面会认为"还在播放"，
        // 于是进度条继续走、外部的"加载中"指示不出现，用户看到的是"画面冻住但没提示"。
        // 合并之后页面拿到的语义是"没在前进"，与它自己的 paused 处理天然对齐：
        // 暂停时不该有帧推进，rebuffer 时同样不该——两者对页面是同一件事。
        state.paused = media->paused.load() || media->rebuffering.load();
        state.ended = media->ended.load();
        state.error = media->error.load();
        state.stalled = media->stalled.load();
        state.audioReady = media->audio.codec != nullptr;
        state.videoReady = media->video.codec != nullptr;
        {
            std::lock_guard<std::mutex> frameLock(media->frameMutex);
            state.frameWidth = media->frameWidth;
            state.frameHeight = media->frameHeight;
        }
        state.audioPosition = media->audio.position.load();
        state.audioFail = media->audio.fail;
    }

    // 名值分离的有序字段表：字段行不再各自手拼 ",，逗号只在最后拼装时统一插入，
    // 漏逗号/多逗号都不可能再发生。数值格式保持历史约定：秒类三位小数，字节/尺寸整数。
    std::vector<std::pair<std::wstring, std::wstring>> fields;
    auto addNumber = [&](const wchar_t* key, double value) {
        fields.emplace_back(key, numberToWide(value));
    };
    auto addInteger = [&](const wchar_t* key, long long value) {
        fields.emplace_back(key, std::to_wstring(value));
    };
    auto addBoolean = [&](const wchar_t* key, bool value) {
        fields.emplace_back(key, value ? L"1" : L"0");
    };

    if (state.missing) {
        addBoolean(L"missing", true);
    } else {
        addNumber(L"duration", state.duration);
        addNumber(L"position", state.position);
        addNumber(L"buffered", state.buffered);
        addNumber(L"bufferedUntil", state.bufferedUntil);
        addInteger(L"read", state.readBytes);
        addBoolean(L"paused", state.paused);
        addBoolean(L"ended", state.ended);
        addBoolean(L"error", state.error);
        // 源暂时取不到数据：页面可以把"卡住"显示成"正在等数据"而不是"出错"（error 仍是真致命）。
        addBoolean(L"stalled", state.stalled);
        addBoolean(L"audio", state.audioReady);
        addBoolean(L"video", state.videoReady);
        addInteger(L"vw", state.frameWidth);
        addInteger(L"vh", state.frameHeight);
        addNumber(L"apos", state.audioPosition);
        if (!state.audioFail.empty()) {
            // fail 短语只允许 ASCII（openAudio 各失败路径写死），直接加宽即可，无需 JSON 转义。
            fields.emplace_back(L"aerr",
                                L"\"" + std::wstring(state.audioFail.begin(), state.audioFail.end()) + L"\"");
        }
    }

    std::wstring json = L"{\"ok\":true";
    for (const auto& field : fields) {
        json += L",\"" + field.first + L"\":" + field.second;
    }
    json += L"}";
    return json;
}

} // namespace nmb
