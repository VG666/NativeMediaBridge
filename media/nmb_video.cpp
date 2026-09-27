/**
 * @file nmb_video.cpp
 * @brief 视频与解复用：closeFfmpegVideo、publishFrame（BGRA 帧发布）、缓冲水位常量、
 *        demuxLoop（唯一连接上读包并分流）、decodeLoop（消费视频队列出帧）、openInput/openVideo 开流。
 *
 * 由 native_media_bridge.cpp 按职责拆分（P1-P5 重构）。
 * 内部实现一律在 namespace nmb，跨模块接口集中声明于 nmb_internal.h。
 */
#include "core/nmb_internal.h"

namespace nmb {
void closeFfmpegVideo(FfmpegVideo& video) {
    if (video.scaler) sws_freeContext(video.scaler);
    if (video.bgraFrame) {
        if (video.bgraFrame->data[0]) av_freep(&video.bgraFrame->data[0]);
        av_frame_free(&video.bgraFrame);
    }
    if (video.frame) av_frame_free(&video.frame);
    if (video.packet) av_packet_free(&video.packet);
    if (video.codec) avcodec_free_context(&video.codec);
    if (video.format) avformat_close_input(&video.format);
    video = {};
}
void publishFrame(Media* media, AVFrame* frame) {
    auto& video = media->video;
    if (!video.bgraFrame) return;
    // 解码后的尺寸/格式才是权威；码流可在中途改变分辨率或像素格式。
    const int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_BGRA, frame->width, frame->height, 1);
    if (bufferSize <= 0) {
        media->error = true;
        setError(L"FFmpeg 视频帧尺寸无效");
        return;
    }
    if (video.bgraFrame->width != frame->width || video.bgraFrame->height != frame->height) {
        av_freep(&video.bgraFrame->data[0]);
        video.bgraFrame->width = video.bgraFrame->height = 0;
        if (av_image_alloc(video.bgraFrame->data, video.bgraFrame->linesize,
                           frame->width, frame->height, AV_PIX_FMT_BGRA, 1) < 0) {
            media->error = true;
            setError(L"FFmpeg BGRA 缓冲区分配失败");
            return;
        }
        video.bgraFrame->width = frame->width;
        video.bgraFrame->height = frame->height;
    }
    video.scaler = sws_getCachedContext(video.scaler, frame->width, frame->height,
                                      static_cast<AVPixelFormat>(frame->format),
                                      frame->width, frame->height, AV_PIX_FMT_BGRA,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!video.scaler) {
        media->error = true;
        setError(L"FFmpeg 视频色彩转换初始化失败");
        return;
    }
    int scaled = sws_scale(video.scaler, frame->data, frame->linesize, 0, frame->height,
                            video.bgraFrame->data, video.bgraFrame->linesize);
    if (scaled != frame->height) {
        media->error = true;
        setError(L"FFmpeg 视频色彩转换失败");
        return;
    }
    const int sourceStride = video.bgraFrame->linesize[0];
    const int rowBytes = frame->width * 4;
    std::vector<unsigned char> pixels(static_cast<size_t>(rowBytes) * frame->height);
    for (int y = 0; y < frame->height; ++y)
        std::copy_n(video.bgraFrame->data[0] + static_cast<size_t>(y) * sourceStride,
                    rowBytes, pixels.data() + static_cast<size_t>(y) * rowBytes);
    {
        std::lock_guard<std::mutex> lock(media->frameMutex);
        media->bgra = std::move(pixels);
        media->frameWidth = frame->width;
        media->frameHeight = frame->height;
    }
    if (!media->stop) notifyComposite(media->browser);
}


// ── 缓冲模型（秒）────────────────────────────────────────────────────
// 出发点：**把"读数据"和"解码播放"彻底解耦**。解复用线程（demuxLoop）永远在往缓冲里读，
// 读不读与"在不在播"无关；解码线程只从缓冲取包，取不到就等，绝不自己去读网络。
// 什么时候读、读多少，全由下面这几个水位决定。
//
// 读前缓冲的窗口：读到"当前播放位置 + 这个秒数"就停手。暂停时播放位置不动，
// 所以暂停期间也会一直灌到同一个窗口——这就是"无论播放与否，都始终加载缓冲"。
// 取 30 秒：这是"缓冲积极程度"的总闸门。窗口越小，一点点网络抖动就会把播放位置顶到前沿、
// 触发等缓冲；窗口越大，能扛住的抖动越久。代价只有内存（两条队列合计约 2MB 量级）。
constexpr double kBufferSeconds = 30.0;
// 每条队列的包数上限：暂停时解码几乎不动，靠它兜住内存。
// 注意它是**兜底**而不是主闸门：主闸门是上面的时间窗口。之所以给到 2000，
// 是因为音频包（每包约 23ms）在同样秒数下的包数是视频包（每帧约 40ms）的两倍左右，
// 上限给小了就会出现"视频还没读完、音频队列已经顶到上限"的假满。
constexpr size_t kQueuePackets = 2000;
// 一次进入灌数据状态，连续读这么多包再回头做一次水位判定。
// 每读一个包都要看水位、比 rebuffer 阈值、查两条队列长度（加锁）——一轮一个包的话，
// 这些判定开销会占掉相当一部分吞吐。批量读把这份开销摊薄，源站快时能明显更快填满窗口。
constexpr int kReadBurstPackets = 64;
// 播放位置追到读前沿这么近，就认定"进度与缓冲量不匹配"：暂停播放等缓冲（rebuffer）。
// 取 0.35 秒：这是"能继续播就继续播"的底线。它越小，已经读到手的数据被白白锁住的时间越短。
// 从前取 1.0——明明手上还有整整一秒可以放的内容，却先停下来等；在只能勉强跟住 1 倍速的源上，
// 这一停一等等于把"能播"变成"卡顿"：实测慢源上每次掉水位要白冻 1.5 秒以上才恢复。
constexpr double kRebufferLowSeconds = 0.35;
// 恢复水位（稳态：播着播着掉下去之后的重新起步）：刻意比上面那个大，留出迟滞——
// 否则会在阈值附近"恢复一帧→立刻又断"地抖，表现出来就是持续的、按秒一跳的卡顿。
// 取 1.2 秒：够防抖，又不像从前的 2.5 秒那样，每掉一次水位都要盯着不动等一两个网络往返。
constexpr double kRebufferHighSeconds = 1.2;
// 首轮填充门槛（开播时、以及每一次拖完进度条之后）：只要求"够把声卡喂起来"就恢复播放。
// 为什么必须和稳态恢复分成两个值：定位之后缓冲是**从落点重新长出来**的（见 seekTo），
// ahead 必然从 0 开始，此时套用稳态那套迟滞没有任何防抖价值——迟滞防的是"播着播着掉水位"，
// 而刚定位完压根还没在播。实测慢源上 2.5 秒门槛让定位后白白冻住 2.6 秒：日志里那 3 秒是一片空白，
// 恢复的瞬间队列里已经有 117 个包——数据早就在手上了，只是被水位门槛锁着不许用。
constexpr double kStartupFillSeconds = 0.6;
// seek 落点往左（往回）留的余量：从落点前面这么多秒开始读。解码器需要落点之前的关键帧
// 才能准确落位，音频也需要一点前置数据才能对齐；但也不能多，否则用户拖完进度会先看到（听到）
// 一段他并没有要看的内容。
constexpr double kSeekBackSeconds = 1.5;
// 视频帧允许落后音频时钟这么多（秒），超过就不再按节拍等、而是立刻连出几帧追上去。
// 为什么需要：音画对齐靠"帧还没到点就睡到那个点"，可帧一旦已经晚于音频（丢帧、
// 系统调度被抢、色彩转换慢），再睡就等于把偏差永久留在那里，越拖越远。
// 0.15 秒是"看着还同步"的经验容差；超过它就放弃等待、让画面尽快追上声音。
constexpr double kFrameLateSeconds = 0.15;
// 落后的上限：超过这么多就认定这一帧已经过期，直接丢掉不出——追上来的过程里
// 把积压的旧帧一帧帧画出来同样是慢动作，丢掉才是真正的"追上"。
constexpr double kFrameDropSeconds = 0.5;
// 单次网络读的时限（毫秒）：**必须给足**，绝不能是"几百毫秒"这种短值。
// 因为中断回调是在读的中途把这次读掐掉（av_read_frame 以 AVERROR_EXIT 返回），而 HTTP 协议
// 往往已经把一部分字节读进了自己的缓冲——被掐掉的这次读，这些字节既没交出来也没归还，
// 整条字节流就此错位，后面的包解出来就是 "Invalid data found when processing input"。
// 实测复现：补缓冲时限设 200ms、门槛设 1000ms，播到 2.8 秒就报"送入视频包失败: Invalid data"。
// 所以这里统一给 20 秒，它只作兜底——协议层 rw_timeout（10 秒，见 applyIoOptions）会先返回；
// "慢但活着"的源一次都不会触发它，真正卡死的源由 kIoStallMs 收口。
constexpr int kIoReadLimitMs = 20000;
// 源站连续多久没给出任何数据就判定读失败（毫秒）。中断回调把"无限阻塞"切成"可中断"之后，
// 还需要这个时间尺度来收口——否则"能中断"只意味着能退出，而不是能报错。
constexpr int kIoStallMs = 15000;

// 连续多少个视频包"送不进解码器"才认定字节流整体错位（判熔断）。
// 为什么会有零星坏包：一次被掐断的读，恰好正在组装的**那一个**包会拿到错位字节；再往后
// FFmpeg 会用 Range 重新定位，字节流自己就正回来了。所以坏包通常是**一个**，不是一片。
// 从前一个坏包就直接熔断，等于让一次网络抖动判死整条播放；这里先丢包，连续 kBadPacketLimit
// 个都送不进去才认账（一串全坏＝真的错位，这时候再救也没意义）。
constexpr int kBadPacketLimit = 8;

// 音频时钟"停摆"多久就不再把它当主时钟（毫秒）。见 outputFrame：时钟停住时那条
// "睡到声音播到这里"会变成**永久睡眠**，画面跟着一起冻死——哪怕是手上还有几十秒的画面。
// 判据放在这里而不是别处：只有"时钟自己在走"才能当节拍源。
constexpr int kAudioClockStallMs = 500;

// 诊断开关：设了 NMB_NO_PRELOAD=1 就退回"读到哪播到哪"的老行为，用来对照缓冲的效果。
bool preloadDisabled() {
    static const bool disabled = [] {
        char buffer[8]{};
        return GetEnvironmentVariableA("NMB_NO_PRELOAD", buffer, sizeof(buffer)) > 0;
    }();
    return disabled;
}

// 缓冲窗口（秒）：NMB_NO_PRELOAD=1 时返回 0，退回"读到哪播到哪"，用来对照缓冲的效果。
double bufferWindowSeconds() { return preloadDisabled() ? 0.0 : kBufferSeconds; }

// 诊断开关：把每一帧的"帧时间戳 / 音频时钟 / 两者之差"追加写进一个文件（默认关闭，零行为变化）。
// 为什么要单独有这么一条日志："音画不同步"在屏幕上只是一个**感觉**——声音好像快一点？
// 还是画面慢一点？看屏幕看不出来，也没有任何现成数字能读（页面只能拿到 position，
// 而 position 现在正是由视频帧驱动的，拿它跟它自己比没有意义）。
// 只有把这两个量并排记下来，才能分清三种完全不同的毛病：
//   ① drift（pts − clock）**持续变大**：节拍没对齐，视频在按自己的速度跑；
//   ② drift 稳定但**整体偏移**：seek/暂停恢复时锚点没接上；
//   ③ drift 正常而 clock 自己的斜率 ≠ 1：声卡侧时钟推进算错了。
// 每一行都带 GetTickCount，所以 clock 的斜率可以直接用相邻两行的差值算出来。
const char* syncLogPath() {
    static const std::string path = [] {
        char buffer[512]{};
        const DWORD n = GetEnvironmentVariableA("NMB_SYNC_LOG", buffer, sizeof(buffer));
        return (n > 0 && n < sizeof(buffer)) ? std::string(buffer, n) : std::string();
    }();
    return path.empty() ? nullptr : path.c_str();
}
// 记一行同步数据。整帧只开一次文件、写一行就关：正常播放每秒 25 行上下，
// 这点开销在诊断时才付（默认关闭时连字符串比较都不会发生）。
void noteSyncFrame(const Media* media, double pts, bool stamped, double clock) {
    const char* const path = syncLogPath();
    if (!path) return;
    if (FILE* f = fopen(path, "a")) {
        fprintf(f, "%lu pts=%.3f clock=%.3f drift=%.3f pos=%.3f buffered=%.3f "
                   "audio=%d rebuf=%d paused=%d ended=%d\n",
                static_cast<unsigned long>(GetTickCount()), pts, clock,
                stamped ? pts - clock : 0.0, media->position.load(),
                media->bufferedUntil.load(), media->audio.stream ? 1 : 0,
                media->rebuffering ? 1 : 0, media->paused ? 1 : 0, media->ended ? 1 : 0);
        fclose(f);
    }
}
// 音频侧的一行诊断（见 nmb_internal.h 的声明）。event 用短标签，便于事后 grep：
//   write / starve / rebuf-on / rebuf-off / seek / seek-reuse
// w 是"写入前沿"，clk 是用户听到的位置，org 是本轮锚点，pend 是声卡里排队的块数，
// q 是音频包队列深度。时钟冻住时，看这四个值就能立刻分辨是哪一种：q=0（没包）、
// pend 顶满（声卡没消费）、w 不涨（压根没写出去）。
void noteSyncAudio(const char* event, double writtenUntil, double clock, double origin,
                   int pending, size_t queueDepth, int playing) {
    const char* const path = syncLogPath();
    if (!path) return;
    if (FILE* f = fopen(path, "a")) {
        fprintf(f, "%lu aud %s w=%.3f clk=%.3f org=%.3f pend=%d q=%zu play=%d\n",
                static_cast<unsigned long>(GetTickCount()), event, writtenUntil, clock,
                origin, pending, queueDepth, playing);
        fclose(f);
    }
}

// 一个包的时间戳换算成绝对秒（按它所在流的时间基）。容器里有些包没有 pts，退回 dts。
bool packetSeconds(const AVPacket* packet, double timeBase, double& seconds) {
    const int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    if (stamp == AV_NOPTS_VALUE || timeBase <= 0.0) return false;
    seconds = stamp * timeBase;
    return true;
}

// ── seek 的"就地复用"：落点还在已缓冲区间内时不碰网络 ──────────────────────
// 已缓冲区间是 [队首包的时间戳, bufferedUntil]。队首≈播放位置（decodeLoop 按播放节拍逐包消费，
// 暂停时还用 peek 把包留在队里），所以这条区间永远是从"用户此刻看/听到的地方"往后伸到读前沿。
// 一次真实定位（clear + avformat_seek_file + 回退 bufferedUntil）会把落点之后那半段一起丢掉，
// 可它们是**已经下过、还没交给解码器**的包；丢掉的代价是：这一段要重新走网络、水位从 0 重新长，
// 于是"往后拖一段"必然白等一次缓冲。
//
// 下面两个函数只做一件事：把队列砍到"真实定位也会从这个点起步"的位置上，其余环节
// （flush 解码器、重锚时钟、positionFloor）全部照旧。判据是**时间戳**，不是"队列里有没有东西"：
//   · 视频：解码器必须从一个关键帧起步，取 from 之前**最近**的那个关键帧（与
//     AVSEEK_FLAG_BACKWARD 落在同一帧）；它到 from 之间的包是关键帧的预滚，必须留着；
//   · 音频：没有关键帧概念，第一个时间戳 ≥ from 的包就是起步点。
// 找不到起步点（落点跑到队首之前了）返回 false，调用方退回真实定位。失败时**一个包都不动**，
// 免得把队列改坏了还要靠调用方的全清来兜底。
void dropQueueFront(PacketQueue& queue, size_t count) {
    // 调用方必须已持 queue.mutex（两个 trim 都在锁内调它）。
    for (size_t i = 0; i < count; ++i) {
        AVPacket* packet = queue.packets.front();
        queue.packets.pop_front();
        av_packet_free(&packet);
    }
}

bool trimVideoBeforeKeyframe(PacketQueue& queue, double timeBase, double from) {
    std::lock_guard<std::mutex> lock(queue.mutex);
    size_t cut = queue.packets.size();
    for (size_t i = 0; i < queue.packets.size(); ++i) {
        double seconds = 0.0;
        if (!packetSeconds(queue.packets[i], timeBase, seconds) || seconds > from) break;
        if (queue.packets[i]->flags & AV_PKT_FLAG_KEY) cut = i;
    }
    if (cut == queue.packets.size()) return false;
    dropQueueFront(queue, cut);
    return true;
}

bool trimAudioBefore(PacketQueue& queue, double timeBase, double from) {
    std::lock_guard<std::mutex> lock(queue.mutex);
    size_t cut = 0;
    while (cut < queue.packets.size()) {
        double seconds = 0.0;
        if (!packetSeconds(queue.packets[cut], timeBase, seconds) || seconds >= from) break;
        ++cut;
    }
    if (cut == queue.packets.size()) return false;
    dropQueueFront(queue, cut);
    return true;
}

// ── 解复用线程：一个媒体只有这一条连接 ─────────────────────────────────
// 过去 video 与 audio 各开一条连接、各自 av_read_frame：同一段字节要下两遍，带宽紧的源上
// 等于自己跟自己抢。更要命的是 seek——两条连接各自定位，视频落关键帧、音频落帧边界，
// 两边落点差出几百毫秒到几秒，音画就此错开，而且谁也纠正不了谁。
// 现在读包集中在这一条连接上：读到的包按流号投进 videoQueue / audioQueue。
// 顺带把"缓冲"也收到这一处（水位、已读字节、是否读到末尾、要不要等缓冲），
// 于是页面上看到的缓冲量，和两条解码线程真正拿到的包，天然是同一个口径。
void demuxLoop(Media* media) {
    ensureTimerResolution();
    auto& video = media->video;
    auto& audio = media->audio;
    AVFormatContext* input = video.format;
    if (!input) return;
    const double window = bufferWindowSeconds();
    auto lastData = std::chrono::steady_clock::now();
    bool startupFill = true;
    auto seekTo = [&](double seconds) {
        const double target = std::max(0.0, seconds);
        const double from = std::max(0.0, target - kSeekBackSeconds);
        bool reuse = false;
        {
            std::lock_guard<std::mutex> commit(media->decodeMutex);
            media->seeking = true;
            media->rebuffering = true;
            startupFill = true;
            media->ended = false;
            media->error = false;
            media->stalled = false;
            reuse = (media->hasVideo || audio.stream) && target <= media->bufferedUntil.load();
            if (reuse && media->hasVideo)
                reuse = trimVideoBeforeKeyframe(media->videoQueue, video.timeBase, from);
            if (reuse && audio.stream)
                reuse = trimAudioBefore(media->audioQueue, av_q2d(audio.stream->time_base), from);
            if (!reuse) {
                media->videoQueue.clear();
                media->audioQueue.clear();
                media->videoQueue.eof = false;
                media->audioQueue.eof = false;
                media->demuxEof = false;
                media->bufferedUntil = target;
                media->bufferedSeconds = 0.0;
            }
        }
        int result = 0;
        if (!reuse) {
            armIo(media->videoIo, media, kIoReadLimitMs);
            result = avformat_seek_file(input, -1, INT64_MIN,
                static_cast<int64_t>(from * AV_TIME_BASE), INT64_MAX, AVSEEK_FLAG_BACKWARD);
            disarmIo(media->videoIo);
        }
        // Keep the actual FFmpeg result visible even if a later state update clears the error.
        char seekError[AV_ERROR_MAX_STRING_SIZE]{};
        if (result < 0) {
            av_strerror(result, seekError, sizeof(seekError));
            fprintf(stderr, "NMB seek FAILED target=%.3f from=%.3f result=%d error=%s\n",
                    target, from, result, seekError);
            fflush(stderr);
        }
        if (const char* path = syncLogPath()) {
            if (FILE* f = fopen(path, "a")) {
                fprintf(f, "%lu seek target=%.3f from=%.3f reuse=%d result=%d error=%s\n",
                        static_cast<unsigned long>(GetTickCount()), target, from,
                        reuse ? 1 : 0, result, seekError);
                fclose(f);
            }
        }
        {
            std::lock_guard<std::mutex> commit(media->decodeMutex);
            media->position = target;
            media->positionFloor = target;
            audio.position = target;
            audio.clock = target;
            audio.writtenUntil = target;
            audio.finished = false;
            // 两个消费者各自观察代次并 flush，解复用线程绝不操作 codec。
            ++audio.clockEpoch;
            media->seeking = media->seekPending.load();
            if (result < 0) {
                media->error = true;
                setError(L"FFmpeg 定位失败: " + ffmpegError(result));
            }
        }
        lastData = std::chrono::steady_clock::now();
    };
    while (!media->stop) {
        bool requested = false;
        double target = 0.0;
        {
            std::lock_guard<std::mutex> commit(media->decodeMutex);
            requested = media->seekPending.exchange(false);
            if (requested) {
                target = media->seekSeconds.load();
                media->seeking = true;
            }
        }
        if (requested) seekTo(target);
        if (media->seekPending) continue;
        const double until = media->bufferedUntil.load();
        // 水位基准是"播放真正推进到了哪儿"。两个候选量各有各失效的时候，必须分开对待，
        // 绝不能一律取 min 再把缺席的一方当成 0：
        //   · media->position 由**视频**解码线程写 —— 纯音频源根本没有这条线程，它恒为 0；
        //   · audio.clock 只在音频真的在播时有效 —— 暂停/等缓冲时它冻住，纯视频源里它不存在。
        // 从前一律写 min(position, audioClock或+∞)，纯音频源上 consumed 就恒等于 0，
        // ahead 于是等于"已读到的绝对时间点"：涨到 30 秒窗口后永不回落，full 永久为真，
        // 解复用线程从此一个字节都不再读。表现就是纯音频播到 30 秒处永久卡死、缓冲能力为零，
        // 页面上的 buffered 数字还停在那里不动（这正是"缓冲能力太差"最硬的一条）。
        const bool audioAlive = audio.stream && audio.playing && !media->rebuffering;
        double consumed = media->position;
        if (audioAlive && media->hasVideo) {
            // 有声有视：取两者里**较慢**的那个。视频为了对齐音频会连出几帧追赶（见 kFrameLateSeconds），
            // 也会为等音频而停住，位置会短暂跳变；单看它算出的水位忽高忽低——
            // 高时误判"已经灌够"而停读，低时又疯狂读。音频时钟才是"用户已经消费到哪儿"的权威。
            consumed = std::min(media->position.load(), audio.clock.load());
        } else if (audioAlive) {
            // 纯音频源且在播：位置只能问音频时钟（media->position 在这类源上永远是 0，见上）。
            consumed = audio.clock.load();
        } else if (audio.stream && !media->hasVideo) {
            // 纯音频源此刻没在播（暂停 / 等缓冲 / 已播完）：用音频自己记录的位置当基准。
            // 这样"暂停期间也继续加载缓冲"依然成立：ahead 随已读前沿一起长，不会凭空涨满。
            consumed = audio.position.load();
        }
        const double ahead = until - consumed;
        // ① 等缓冲的进出。两个门槛分两套（见常量注释）：首轮填充只要求"够开声"，
        //    稳态恢复才要求攒到防抖水位。读到末尾就没有等的必要：剩下的都能立刻给出去。
        if (media->demuxEof) {
            media->rebuffering = false;
        } else if (media->rebuffering) {
            if (ahead >= (startupFill ? kStartupFillSeconds : kRebufferHighSeconds)) {
                media->rebuffering = false;
                startupFill = false;
            }
        } else if (ahead < kRebufferLowSeconds) {
            media->rebuffering = true;
        } else if (ahead >= kRebufferHighSeconds) {
            // 已经稳稳跑在稳态水位之上：首轮填充的宽松门槛就此作废，此后按迟滞走。
            startupFill = false;
        }
        media->bufferedSeconds = media->demuxEof ? 0.0 : std::max(0.0, ahead);
        // ② 灌到窗口就歇着（暂停时也一样灌到同一个窗口，这就是"始终加载缓冲"）。
        const bool full = window > 0.0 && ahead >= window;
        const bool queued = media->videoQueue.size() >= kQueuePackets ||
                            (audio.stream && media->audioQueue.size() >= kQueuePackets);
        if (media->demuxEof || full || queued) {
            // 歇一小会儿就回头再看水位。这里取 5ms 而不是更长：播放位置一前进就该立刻接着灌，
            // 等得越久"缓冲追上播放"越慢，用户感觉到的就是"卡一下、动一下"。
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        // ③ 连续读一批（最多 kReadBurstPackets 个包）：把每轮的水位判定、阈值比较、
        //    两次加锁查队列长度这些固定开销摊薄，源站快时窗口能明显更快填满。
        //    批内每一轮都重新查一次定位请求——用户拖进度条时必须尽快把连接让出去，
        //    不能傻等这一批读完。
        for (int burst = 0; burst < kReadBurstPackets && !media->stop; ++burst) {
            if (media->seekPending) break;
            AVPacket* packet = av_packet_alloc();
            if (!packet) break;
            armIo(media->videoIo, media, kIoReadLimitMs);
            const int read = av_read_frame(input, packet);
            disarmIo(media->videoIo);
            if (read == AVERROR_EOF) {
                av_packet_free(&packet);
                // 注意：中途断掉的源也可能以 EOF 收场（mov.c 里尾部 seek 失败就是返回 EOF），
                // 于是"源没读完"会被当成"源读完了"。真要区分得拿 avio_tell 跟 duration 比。
                // 当前不区分：这种情况下队列里剩下的都能照常播完（ended 只影响"要不要报播完"）。
                media->demuxEof = true;
                media->videoQueue.eof = true;
                media->audioQueue.eof = true;
                // 源全部到手：整条都算"已缓冲"，页面上的色带直接铺满。
                const double total = media->video.duration > 0.0 ? media->video.duration : media->audio.duration;
                if (total > 0.0) media->bufferedUntil = std::max(media->bufferedUntil.load(), total);
                break;
            }
            if (read < 0) {
                av_packet_free(&packet);
                // 读错误 ≠ 这条流废了：中断回调（AVERROR_EXIT）、协议层超时（ETIMEDOUT）、
                // 源站掐连接（EIO/ECONNRESET）都只是"这次没拿到数据"，退让一拍重试；
                // 只有连续 kIoStallMs 毫秒什么都没读到才判死，不再一次抖动就整条判死。
                const auto silent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - lastData).count();
                if (silent >= kIoStallMs) {
                    // 这是"暂时取不到"，不是"这条流废了"：源站掐连接、限速抖动、连接被中间设备
                    // 断掉，过后恢复的比比皆是，而下面每次循环都还在重试读取。
                    // 所以这里**绝不能写 media->error** —— 那是个只写不读的熔断标志（整份代码里
                    // 只有 seekTo 会清），一旦置上，解码线程就永久停在"不出帧"那一支，还每 20ms
                    // 把 paused 按回 true：用户再点多少次播放都会被立刻抹掉，画面永远不动，可
                    // 解复用线程照旧在读、缓冲还在长。现场原话就是"视频卡死之后再点也开不了，
                    // 即使已经缓冲到更长的位置"。现在只标记 stalled（可自愈）。
                    if (!media->stalled.exchange(true)) {
                        setError(L"FFmpeg 数据源 " + std::to_wstring(silent / 1000) +
                                 L" 秒没有数据: " + ffmpegError(read));
                    }
                    // 判死之后仍要退让：否则"读失败→立即再读"变成忙等，还每轮抢一次全局锁重写错误文本。
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                } else {
                    // 退让一拍再试。少了这一下，批量读会变成"失败→立刻再读"的忙等，
                    // 几十次空转一秒就过去了，白烧 CPU。
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                break;
            }
            lastData = std::chrono::steady_clock::now();
            // 源恢复：stalled 只表示"此刻取不到"，数据一到就自动清零，不需要用户点任何东西
            // （水位自己涨回来、rebuffering 自己解除，播放从卡住的地方接着走）。
            if (media->stalled) media->stalled = false;
            // ④ 按流号分流，并用水位记下"已经读到哪儿"。
            double seconds = 0.0;
            const bool isVideo = packet->stream_index == video.streamIndex;
            const bool isAudio = !isVideo && audio.stream && packet->stream_index == audio.streamIndex;
            const double timeBase = isVideo ? video.timeBase
                                            : (isAudio ? av_q2d(audio.stream->time_base) : 0.0);
            const bool stamped = packetSeconds(packet, timeBase, seconds);
            if (isVideo) media->videoQueue.push(packet);
            else if (isAudio) media->audioQueue.push(packet);
            else av_packet_free(&packet);
            if (stamped) media->bufferedUntil = std::max(media->bufferedUntil.load(), seconds);
            if (input->pb) media->readBytes = avio_tell(input->pb);
        }
    }
}

// ── §5 视频解码线程与开流（openVideo/openAudio 在本段后半）──────────────
// 视频解码线程（每条视频媒体一条，openVideo 成功后创建）。
// 职责：从 videoQueue 取**已经解复用好的**视频包 → 送解码器 → 取 AVFrame → 按帧节拍
// publishFrame 给 UI 线程。它不再碰网络（读包是 demuxLoop 的事），取不到包就等——
// "读"与"播"就此彻底分开，缓冲量成了唯一决定"还能不能继续播"的东西。
// 等缓冲（media->rebuffering）期间不出帧：画面停在上一帧；音频线程同样停下，
// 于是音画一起等、恢复时也一起走，不会出现"声音跑在画面前面"。
// 后两个参数（WebView/HWND）保留旧签名，当前不直接使用。
void decodeLoop(Media* media, WebView, HWND) {
    ensureTimerResolution();
    auto& video = media->video;
    auto deadline = std::chrono::steady_clock::now();
    long long epoch = 0;
    auto current = [&]() {
        return !media->stop && !media->seeking && epoch == media->audio.clockEpoch.load();
    };
    // 锁仅覆盖解码/提交；节拍等待释放提交门，每 5ms 检查取消和代次。
    std::unique_lock<std::mutex> commit(media->decodeMutex, std::defer_lock);
    auto waitUntil = [&](std::chrono::steady_clock::time_point until) {
        commit.unlock();
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (current() && std::chrono::steady_clock::now() < until);
        commit.lock();
    };
    // 出一帧：交给 UI 线程，然后对齐节拍。
    // 暂停（含拖进度条后的立即刷新）时只交画面，不推进播放位置、不按节拍睡——
    // 否则暂停中拖进度条会让位置被"落点之前"的帧反推回去。
    //
    // ── 节拍以音频时钟为准（音画同步的落点就在这里）────────────────────────
    // 有音轨时，"这一帧该什么时候显示"由 audio.clock（用户此刻听到的位置）决定；
    // 没有音轨（纯视频源）才退回"按帧率累加"的老办法。
    // 为什么必须这样：按帧率累加的节拍，只要系统调度抖一下、或某一帧没解出来，
    // 就会永久性地偏掉——而声卡那边一丝不苟地按 44100Hz 走着，两边越拉越远。
    // 实测现场就是"声音播到 12 秒，画面还在 8 秒"这种谁也纠正不了的不同步。
    // 音频时钟的"上一次采样"：用来判断它到底有没有在走（见 outputFrame 里的 audioRunning）。
    double clockSample = -1.0;
    auto clockSampleAt = std::chrono::steady_clock::now();
    // 连续送不进解码器的视频包数（见 kBadPacketLimit）。
    int badPackets = 0;
    auto outputFrame = [&]() {
        if (!current()) return;
        double seconds = 0.0;
        bool stamped = false;
        if (video.frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            seconds = video.frame->best_effort_timestamp * video.timeBase;
            stamped = true;
        }
        const double audioClock = media->audio.clock.load();
        // 音频时钟还得**真的在走**：它停住的时候，下面那条"睡到声音播到这里"会变成永久睡眠。
        // 什么时候会停：音轨排空（源断在音频那一段、队列被吃空、解码器出错……）。
        // 实测现场：clock 冻在 36.34 不动，而视频队列里还躺着 523 个包（23.6 秒的画面）；
        // 桥这边 paused=false、error 也没有，用户点播放自然毫无反应——因为"没在等什么"。
        // 所以主时钟资格多一条：500 毫秒内必须前进过。停摆期间先按帧率走，
        // 音频一恢复推进就立刻交还主时钟（落后于声音的帧照旧按 kFrameDropSeconds 丢掉，不会越错越远）。
        // 只看"变没变"，不看"往哪个方向变"：定位会把时钟整个挪走（原点换了），
        // 那时候它是在动，不该算停摆。
        if (std::abs(audioClock - clockSample) > 0.001) {
            clockSample = audioClock;
            clockSampleAt = std::chrono::steady_clock::now();
        }
        const bool audioRunning =
            audioClock > 0.0 && std::chrono::steady_clock::now() - clockSampleAt <
                                    std::chrono::milliseconds(kAudioClockStallMs);
        // 音频能不能当主时钟：有声卡、在放、不在等缓冲、时钟已经跑起来、而且还在走。
        const bool audioMaster = stamped && media->audio.stream && media->audio.playing &&
                                 !media->rebuffering && audioRunning;
        // 落点**之前**的预滚帧直接丢掉：不发画面、不写进度、也不进日志。
        // 它为什么存在：定位落在"落点之前最近的关键帧"上（见上面的定位分支），关键帧间隔一大，
        // 从关键帧到落点这一整段都会被重新解出来。光靠下面的 positionFloor 只拦得住进度条，
        // 拦不住画面——帧照样会发出去。实测拖到 2 秒时，30 毫秒里连发了 11 帧旧内容
        // （0.000→1.875），观感就是"闪一下定位前的画面"。
        // 放在 noteSyncFrame 之前：这些帧是按设计丢的，进日志只会把 drift 极值之类的指标拖脏；
        // 而"因迟到被丢"的帧（下面 audioMaster 那条）仍然照常留痕。
        if (stamped && seconds < media->positionFloor.load()) return;
        // 诊断（见 syncLogPath）：把这一帧的对齐情况记下来。放在丢弃判断**之前**，
        // 这样被丢掉的过期帧也会留痕——否则"日志里没问题"可能只是把问题的帧全丢了。
        noteSyncFrame(media, seconds, stamped, audioClock);
        // 已经落后声音太多的帧是"过期帧"：留着它只会让画面继续慢动作，
        // 一帧一帧补画同样追不上，直接丢掉不出才是真正的追上。
        if (audioMaster && seconds < audioClock - kFrameDropSeconds) return;
        publishFrame(media, video.frame);
        if (media->paused) return;
        if (stamped) {
            // 定位后的预滚帧（时间戳小于落点）不写 position：写了进度条就会往回跳，
            // 看着像"拖偏了"。等帧追上落点（见 positionFloor）之后照常推进。
            const double floor = media->positionFloor.load();
            if (seconds >= floor) {
                media->position = seconds;
                if (floor > 0.0) media->positionFloor = 0.0;
            }
        }
        if (audioMaster) {
            // 帧还没到该显示的那一刻：睡到声音播到它为止。
            // 只看"还早多少"，绝不看"晚了多少"——已经晚了就不睡，
            // 因为再睡一下就是把偏差永久留在那儿（这就是从前越播越偏的成因）。
            const double drift = seconds - audioClock;
            if (drift > 0.005) {
                deadline = std::chrono::steady_clock::now() +
                           std::chrono::microseconds(static_cast<int64_t>(drift * 1000000.0));
                waitUntil(deadline);
            }
            return;
        }
        const double delay = video.fps > 0 ? 1.0 / video.fps : 0.04;
        deadline += std::chrono::microseconds(static_cast<int64_t>(delay * 1000000.0));
        if (deadline < std::chrono::steady_clock::now()) deadline = std::chrono::steady_clock::now();
        waitUntil(deadline);
    };
    while (!media->stop) {
        if (commit.owns_lock()) commit.unlock();
        commit.lock();
        if (media->seeking) {
            waitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(5));
            continue;
        }
        if (epoch != media->audio.clockEpoch.load()) {
            epoch = media->audio.clockEpoch.load();
            avcodec_flush_buffers(video.codec);
            av_frame_unref(video.frame);
            av_packet_unref(video.packet);
            deadline = std::chrono::steady_clock::now();
            clockSample = -1.0;
            clockSampleAt = deadline;
            badPackets = 0;
        }
        if (media->error) {
            media->paused = true;
            waitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(20));
            continue;
        }
        // 位置调整由 demuxLoop 统一处理（同一条连接、同一个落点），这里只等它做完：
        // 落点和缓冲水位都没确定之前不出帧，免得先闪一下定位前的旧画面。
        if (media->rebuffering) {
            deadline = std::chrono::steady_clock::now();
            waitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(10));
            continue;
        }
        // 暂停时只"解到当前这一帧"，后面的包原样留在队列里。
        // 为什么必须停手：暂停期间继续往下解码，就是拿队列当缓冲池烧——本地小文件上解复用
        // 几毫秒就把整段读完、解码线程跟着把几百帧一瞬间"过"完（不按节拍），等页面真的调
        // play() 时队列已空、位置还停在 0，于是进度恒 0、ended 恒 1，怎么点播放都不动。
        // 而"暂停中拖进度条要立刻刷新画面"这件事现在由解复用线程统一负责（seekTo 清队列 +
        // flush 解码器 + 置 position），这里只要保证画面走到 position 那一帧即可。
        if (media->paused) {
            int64_t stamp = AV_NOPTS_VALUE;
            const bool front = media->videoQueue.peekTimestamp(stamp);
            const double guard = video.fps > 0 ? 1.0 / video.fps : 0.04;
            if (!front || (stamp != AV_NOPTS_VALUE && video.timeBase > 0 &&
                           stamp * video.timeBase > media->position + guard)) {
                deadline = std::chrono::steady_clock::now();
                waitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(10));
                continue;
            }
        }
        AVPacket* packet = media->videoQueue.pop();
        if (!packet) {
            // 队列空：源已读到末尾就是真的播完了；否则是解复用还没喂上来，退让一拍再取。
            if (media->demuxEof) {
                // 但"源读完了"不等于"播完了"：页面还没点播放（或用户刚按下暂停）时队列空是正常的，
                // 这时候报 ended 会让页面直接认为"已播完"，进度条归零、播不动（实测现场）。
                // 只有确实在播（没暂停）时把队列连同解码器一起排空，才算真的播到结尾。
                if (media->paused) {
                    waitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(10));
                    continue;
                }
                int sent = avcodec_send_packet(video.codec, nullptr);
                while (current() && (sent >= 0 || sent == AVERROR(EAGAIN))) {
                    const int received = avcodec_receive_frame(video.codec, video.frame);
                    if (received < 0) break;
                    outputFrame();
                    if (current() && sent == AVERROR(EAGAIN))
                        sent = avcodec_send_packet(video.codec, nullptr);
                }
                if (!current()) continue;
                media->ended = true;
                media->paused = true;
                continue;
            }
            waitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(5));
            continue;
        }
        // 暂停（含暂停中拖进度条）时也照常解码：画面要立刻跟到新位置，
        // 只是不推进播放进度、不按节拍等待（见 outputFrame）。
        // 送包并出帧。EAGAIN 表示解码器缓冲满：先取一帧腾出位置，再重发同一个包（绝不能丢包）。
        int sent = avcodec_send_packet(video.codec, packet);
        while (current() && sent == AVERROR(EAGAIN)) {
            const int received = avcodec_receive_frame(video.codec, video.frame);
            if (received < 0) break;
            outputFrame();
            if (!current()) break;
            sent = avcodec_send_packet(video.codec, packet);
        }
        av_packet_free(&packet);
        if (sent == AVERROR(EAGAIN) || !current()) continue;
        if (sent < 0) {
            // 零星坏包不判死：丢掉它、继续下一个（丢掉的那一帧按"缺一帧"过去，看不出接缝）。
            // 坏包从哪来：一次被掐断的读，正组装的那个包会拿到错位字节（见 kIoReadLimitMs 的注释）。
            // 实测现场：源站停顿时读回 EIO，队列里混进一个 pts≈36.4 的坏包，解码器报
            // "Invalid data found when processing input"（AVERROR_INVALIDDATA = -1094995529）——
            // 就这一个包，把整条播放判了死刑：error 熔断 → paused 常驻 → 点播放也回不来。
            // 连续 kBadPacketLimit 个都送不进去才是真错位，那时候再熔断。
            ++badPackets;
            if (badPackets < kBadPacketLimit) continue;
            media->error = true;
            media->paused = true;
            setError(L"FFmpeg 送入视频包失败: " + ffmpegError(sent));
            continue;
        }
        badPackets = 0;
        // 把这个包能解出的帧全部取出来交给 UI。
        while (current()) {
            const int received = avcodec_receive_frame(video.codec, video.frame);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
            if (received < 0) {
                media->error = true;
                media->paused = true;
                setError(L"FFmpeg 视频解码失败: " + ffmpegError(received));
                break;
            }
            outputFrame();
        }
    }
}
// ── 通用 MSE 接管：页面经 __nmbServeMse 把 SourceBuffer.appendBuffer 的字节喂进来 ──
// mseMode 时无 source URL，openInput 用自定义 AVIO 从 media->mseVideo 顺序读，demux 出画面。
// 读回调在数据不足时短等（40ms 轮询），被 stop 唤醒即中断；页面持续 append 即连续喂流。
static int mseReadVideo(void* opaque, uint8_t* buf, int size) {
    auto* media = reinterpret_cast<Media*>(opaque);
    std::unique_lock<std::mutex> lk(media->mseMtx);
    while (media->mseVideo.size() < (size_t)size && !media->mseVideoEof.load() && !media->stop.load()) {
        if (media->mseCv.wait_for(lk, std::chrono::milliseconds(40)) == std::cv_status::timeout) {
            if (media->stop.load()) return AVERROR_EXIT;
        }
    }
    if (media->stop.load()) return AVERROR_EXIT;
    size_t n = std::min((size_t)size, media->mseVideo.size());
    if (n == 0) return AVERROR_EOF;
    memcpy(buf, media->mseVideo.data(), n);
    media->mseVideo.erase(media->mseVideo.begin(), media->mseVideo.begin() + n);
    media->readBytes.fetch_add((long long)n);
    return (int)n;
}

// 后台打开（open 与 mse 共用）：分配/找流/起 demux+解码线程，收尾写 openOk。
void openMediaAsync(Media* media, WebView view, HWND hwnd) {
    auto owner = media->browser->self;
    std::thread([owner, media, view, hwnd]() {
        bool ok = false;
        if (media->kind == L"video") {
            ok = openVideo(media, view, hwnd);
            if (ok) openAudio(media);
        } else {
            ok = openAudio(media);
        }
        if (ok && !media->stop) {
            try {
                media->demuxer = std::thread(demuxLoop, media);
                if (media->video.codec)
                    media->decoder = std::thread(decodeLoop, media, view, hwnd);
                if (media->audio.codec)
                    media->audioDecoder = std::thread(audioLoop, media);
            } catch (...) { ok = false; }
        } else { ok = false; }
        if (!ok) {
            media->stop = true; media->audio.stop = true;
            if (media->demuxer.joinable()) media->demuxer.join();
            if (media->decoder.joinable()) media->decoder.join();
            if (media->audioDecoder.joinable()) media->audioDecoder.join();
            closeFfmpegAudio(media->audio);
            closeFfmpegVideo(media->video);
            media->videoQueue.clear();
            media->audioQueue.clear();
        }
        std::lock_guard<std::mutex> lock(owner->mutex);
        media->openOk = ok;
        media->openError = ok ? std::wstring() : L"open-failed";
        media->openRunning = false;
        auto it = owner->media.find(media->id);
        const bool listed = it != owner->media.end() && it->second == media;
        if (!listed) releaseMediaAsync(media);
        else if (!ok) media->openError = L"open-failed";
    }).detach();
}

// ── 打开唯一的一条连接 ────────────────────────────────────────────────
// 分配上下文 → 挂中断回调 → avformat_open_input → find_stream_info → 把视频流和音频流的
// 索引**一次找齐** → 启动 demuxLoop。openVideo 与 openAudio 都先调它，谁先来谁负责开，
// 后来者直接用现成的连接（幂等）——这就是"一个媒体只开一条连接"的落地处。
// 从前两边各开一条：同一段字节下两遍，seek 还各定各的位。
// mseMode：无 source URL，改用自定义 AVIO 从页面喂入的 mseVideo 字节流读（通用 MSE 接管）。
bool openInput(Media* media) {
    auto& video = media->video;
    auto& audio = media->audio;
    if (video.format) return true;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "buffer_size", "1048576", 0);
    if (!media->userAgent.empty()) av_dict_set(&options, "user_agent", media->userAgent.c_str(), 0);
    if (!media->referer.empty()) av_dict_set(&options, "referer", media->referer.c_str(), 0);
    if (!media->cookie.empty()) av_dict_set(&options, "cookies", media->cookie.c_str(), 0);
    if (!media->origin.empty()) {
        const std::string headers = "Origin: " + media->origin + "\r\n";
        av_dict_set(&options, "headers", headers.c_str(), 0);
    }
    video.format = avformat_alloc_context();
    if (!video.format) { av_dict_free(&options); setError(L"FFmpeg 媒体上下文分配失败"); return false; }
    attachInterruptCallback(video.format, &media->videoIo, media);
    if (media->mseMode) {
        uint8_t* iobuf = (uint8_t*)av_malloc(1048576);
        video.format->pb = avio_alloc_context(iobuf, 1048576, 0, media, mseReadVideo, nullptr, nullptr);
    }
    applyIoOptions(&options);
    const std::string urlStr = media->mseMode ? std::string() : ffmpegSource(media->source);
    const char* url = media->mseMode ? nullptr : urlStr.c_str();
    int result = avformat_open_input(&video.format, url, nullptr, &options);
    av_dict_free(&options);
    if (result < 0) { setError(L"FFmpeg 无法打开媒体: " + ffmpegError(result)); return false; }
    result = avformat_find_stream_info(video.format, nullptr);
    if (result < 0) { setError(L"FFmpeg 无法读取媒体信息: " + ffmpegError(result)); closeFfmpegVideo(video); return false; }
    // 两条流都在这儿找齐，各模块此后只认索引，谁也不再自己去 open/找流。
    video.streamIndex = av_find_best_stream(video.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioIndex = av_find_best_stream(video.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    audio.streamIndex = audioIndex;
    audio.stream = audioIndex >= 0 ? video.format->streams[audioIndex] : nullptr;
    // 容器级总时长先兜一份：流级时长缺失时（HLS、不少 mp3）页面就靠它。
    if (video.format->duration > 0) {
        const double total = static_cast<double>(video.format->duration) / AV_TIME_BASE;
        if (video.duration <= 0.0) video.duration = total;
        if (audio.duration <= 0.0) audio.duration = total;
    }
    return true;
}

// 打开一路视频：openInput()（唯一连接）→ avcodec_open2 → 准备 BGRA 转换用的
// sws 上下文与帧缓冲 → 启动 decodeLoop。任何一步失败都通过 closeFfmpegVideo
// 回收半成品并把原因写进 video.fail（页面经 state JSON 的 error 字段读到）。
bool openVideo(Media* media, WebView, HWND hwnd) {
    auto& video = media->video;
    if (!openInput(media)) return false;
    if (video.streamIndex < 0) { setError(L"FFmpeg 找不到视频流"); closeFfmpegVideo(video); return false; }
    int result = 0;
    AVStream* stream = video.format->streams[video.streamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) { setError(L"FFmpeg 找不到视频解码器"); closeFfmpegVideo(video); return false; }
    video.codec = avcodec_alloc_context3(decoder);
    if (!video.codec || avcodec_parameters_to_context(video.codec, stream->codecpar) < 0 || avcodec_open2(video.codec, decoder, nullptr) < 0) {
        setError(L"FFmpeg 无法打开视频解码器"); closeFfmpegVideo(video); return false;
    }
    AVRational rate = av_guess_frame_rate(video.format, stream, nullptr);
    video.fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 25.0;
    video.timeBase = av_q2d(stream->time_base);
    video.duration = stream->duration > 0 ? stream->duration * video.timeBase : 0.0;
    // HLS（.m3u8）这类源常常只在容器层给总时长（AVFormatContext::duration，单位 AV_TIME_BASE），
    // 单条流的 duration 是 0/未定。只认流时长的话，站点从 mp4 切到 HLS 后原生这边一直报 0，
    // 页面就继续挂着换源前的旧时长（实测：位置已到 28s，控件条还显示 /15）。
    if (video.duration <= 0.0 && video.format && video.format->duration > 0)
        video.duration = static_cast<double>(video.format->duration) / AV_TIME_BASE;
    video.packet = av_packet_alloc();
    video.frame = av_frame_alloc();
    video.bgraFrame = av_frame_alloc();
    video.scaler = sws_getContext(video.codec->width, video.codec->height, video.codec->pix_fmt,
                                  video.codec->width, video.codec->height, AV_PIX_FMT_BGRA,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!video.packet || !video.frame || !video.bgraFrame || !video.scaler) {
        setError(L"FFmpeg 视频帧缓冲初始化失败"); closeFfmpegVideo(video); return false;
    }
    int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_BGRA, video.codec->width, video.codec->height, 1);
    media->video.duration = video.duration;
    if (av_image_alloc(video.bgraFrame->data, video.bgraFrame->linesize, video.codec->width, video.codec->height, AV_PIX_FMT_BGRA, 1) < 0) {
        setError(L"FFmpeg BGRA 缓冲区分配失败");
        closeFfmpegVideo(video);
        return false;
    }
    if (bufferSize <= 0) {
        setError(L"FFmpeg 视频缓冲区尺寸无效");
        closeFfmpegVideo(video);
        return false;
    }
    media->paused = true;
    // 告诉音频那一侧"这个源有画面"：音轨包短、数量是视频的两三倍，必定先排空，
    // 它绝不能越过视频去宣布"整个媒体播完了"（见 audioLoop 的收尾分支）。
    media->hasVideo = true;
    return true;
}

} // namespace nmb
