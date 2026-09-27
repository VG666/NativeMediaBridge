/**
 * @file nmb_audio.cpp
 * @brief 音频：closeFfmpegAudio 回收、waveOut 多缓冲播放线程 audioLoop（消费 demuxLoop 分来的音频包）、
 *        openAudio 开流（复用共享连接 + 重采样器 + 8 缓冲）。
 *
 * 由 native_media_bridge.cpp 按职责拆分（P1-P5 重构）；逻辑未改。
 * 内部实现一律在 namespace nmb，跨模块接口集中声明于 nmb_internal.h。
 */
#include "core/nmb_internal.h"

namespace nmb {
// 音频输出用多个缓冲排队播放：如果只用一个缓冲并在写完后等它播完，
// 解码下一帧的间隙就会出现在两次播放之间，听感上就是断断续续。
constexpr int kAudioBufferCount = 8;
constexpr int kAudioSamplesPerBuffer = 2048;
// 注：音频这里不再有"单次网络读时限"这回事——它已经不读网络了。
// 读、超时判定、有数据没数据的收口全在 demuxLoop（见 nmb_video.cpp 的 kIoReadLimitMs/kIoStallMs），
// 两个流的超时口径因此完全一致：一条连接，一套规则。
void closeFfmpegAudio(FfmpegAudio& audio) {
    if (audio.waveOut) {
        waveOutReset(audio.waveOut);
        for (auto& header : audio.headers) {
            if (header.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(audio.waveOut, &header, sizeof(WAVEHDR));
        }
        waveOutClose(audio.waveOut);
        audio.waveOut = nullptr;
    }
    audio.headers.clear();
    audio.pcm.clear();
    if (audio.resampler) swr_free(&audio.resampler);
    if (audio.codec) avcodec_free_context(&audio.codec);
    // 连接不在这里关：音频与视频共用 openInput() 开的那一条（归 FfmpegVideo::format 所有）。
    audio.stream = nullptr;
    audio.streamIndex = -1;
    audio.duration = 0;
    audio.position = 0;
}
// 把"媒体时间点"换算成"自本次锚定以来写进声卡的样本数"。
// 用途见 audioLoop 的暂停/等缓冲收口：声卡里被 reset 掉的那部分要当作没播过，
// 于是必须把写入前沿从"已经写到哪儿"倒回"用户听到哪儿"——这两个量都以样本计，
// 换算过来才能继续保持 writtenUntil = originPts + writtenSamples / 44100 这个等式成立。
long long clockToSamples(double seconds, double originPts) {
    const double delta = seconds - originPts;
    if (delta <= 0.0) return 0;
    // 44100 是写卡采样率，和 openAudio 里给的 waveFormat 一致（每通道样本数）。
    return static_cast<long long>(delta * 44100.0);
}
// 音频播放线程（每条音频媒体一条，openAudio 末尾创建）。
// 职责：按包解码 → swresample 重采样成 S16 立体声 44.1kHz → 8 个 WAVEHDR 排队喂给 waveOut，
// 靠缓冲回收驱动节拍；处理定位/暂停/等缓冲标记，并把**音频主时钟**（audio.clock，
// 也就是用户此刻真正听到的那个位置）持续写回 media —— 视频线程按它对齐画面
// （见 nmb_video.cpp 的 outputFrame），页面进度条也以它为准。
// 线程退出条件：closeFfmpegAudio 置 stop 或解码到结尾。waveOut 句柄由
// closeFfmpegAudio 重置事件并 waveOutReset 解除阻塞后安全关闭。
void audioLoop(Media* media) {
    ensureTimerResolution();
    auto& audio = media->audio;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        media->error = true;
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        return;
    }
    const size_t samplesPerBuffer = static_cast<size_t>(kAudioSamplesPerBuffer);
    const size_t framesPerBuffer = samplesPerBuffer * 2;
    // 本线程不读网络、也不定位：包由 demuxLoop 从唯一那条连接上分过来（media->audioQueue），
    // 定位同样由它统一执行（同一条连接、和视频落在同一点）——所以这里不需要任何网络守卫。
    // 解码状态：packetReady 表示 packet 里有一个还没被解码器接收的包，
    // frameReady 表示 frame 里有一帧还没送去重采样。
    bool packetReady = false;
    bool frameReady = false;
    bool drained = false;
    bool eof = false;
    // 刚解码出来的那一帧的原始时间戳（流时间基）。只在两个地方用：
    //   ① 每次锚定后写出的第一块，用它把时钟锚点接到真实音频帧上（比容器落点准）；
    //   ② 没有主时钟（老逻辑）时的位置兜底。
    int64_t lastTimestamp = AV_NOPTS_VALUE;
    // ── 音频主时钟的本地状态（只有本线程读写，不必放进 Media）──────────────
    // originPts：本次锚定对应的媒体时间点。writtenSamples：自锚定以来累计写进声卡的
    // 样本数（每通道）。两者合起来就是"已经写进声卡的媒体时间前沿"：
    //   writtenUntil = originPts + writtenSamples / 44100
    // 再减去声卡里还没播完的那几块，就得到用户此刻真正听到的位置（audio.clock）。
    double originPts = audio.clock.load();
    long long writtenSamples = 0;
    long long epoch = 0;
    // 上一轮是不是在播：只在"从播到停"的那一刻做一次收口（时钟倒回、清声卡），
    // 否则每 10ms 一轮都会重复 reset，白费而且没有任何意义。
    bool wasPlaying = false;
    // 诊断用：队列空造成的"一块都没写出去"每 5ms 就会撞上一次，全记下来会刷爆日志，
    // 所以按 200ms 节流（见 noteSyncAudio）。
    auto lastStarve = std::chrono::steady_clock::now();
    while (!media->stop && !audio.stop) {
        std::unique_lock<std::mutex> commit(media->decodeMutex);
        auto wait = [&](int ms) {
            commit.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            commit.lock();
        };
        if (media->seeking) {
            if (audio.waveOut) waveOutReset(audio.waveOut);
            wait(5);
            continue;
        }
        // 定位代数变了（demuxLoop 刚做过一次 avformat_seek_file）：时钟锚点重建。
        // 锚点必须跟着落点走，否则视频那边已经按新位置对齐，音频时钟还停在上一次的位置。
        // 解码状态一并清掉：packet/frame 里装的是定位之前的数据，拿去重采样就是旧声音。
        if (audio.clockEpoch.load() != epoch) {
            epoch = audio.clockEpoch.load();
            avcodec_flush_buffers(audio.codec);
            av_packet_unref(packet);
            av_frame_unref(frame);
            if (audio.waveOut) waveOutReset(audio.waveOut);
            swr_close(audio.resampler);
            if (swr_init(audio.resampler) < 0) {
                media->error = true;
                audio.playing = false;
                setError(L"FFmpeg 定位后音频重采样器重置失败");
            }
            originPts = audio.clock.load();
            writtenSamples = 0;
            audio.writtenUntil = originPts;
            packetReady = false;
            frameReady = false;
            drained = false;
            eof = false;
            wasPlaying = false;
            // 时间戳也要清：它记的是定位之前那条流上的一帧，留着它会把新锚点接到旧位置上。
            lastTimestamp = AV_NOPTS_VALUE;
        }
        // 等缓冲：播放位置追到读前沿时，视频不出帧、这里也不推音频——音画一起等、一起恢复，
        // 不会出现"画面停了声音还在跑"。demuxLoop 灌够水位后会清掉这个标记。
        if (media->rebuffering) {
            // 把已经排在声卡里的旧声音丢掉，否则恢复后会先播出一段落点之前的声音。
            if (audio.waveOut) waveOutReset(audio.waveOut);
            // 被 reset 掉的那几块等于"没有播出来"：把写入前沿倒回用户真正听到的位置，
            // 否则恢复播放时时钟会凭空前跳一截，画面跟着跳、音画也错开。
            writtenSamples = clockToSamples(audio.clock.load(), originPts);
            audio.writtenUntil = audio.clock.load();
            packetReady = false;
            frameReady = false;
            drained = false;
            eof = false;
            wasPlaying = false;
            wait(10);
            continue;
        }
        if (!audio.playing) {
            // 暂停要真的安静下来：把已经排进声卡、还没播完的那部分丢掉。
            // 这一下由本线程做（而不是查询线程的 pause op）是为了让"丢弃"和"时钟倒回"
            // 发生在同一个地方——分开做的话，时钟会在声卡清空之后还记着那些没播的样本，
            // 表现就是"暂停一下再播，声音和画面错开半秒，而且再也回不来"。
            if (wasPlaying && audio.waveOut) waveOutReset(audio.waveOut);
            if (wasPlaying) {
                writtenSamples = clockToSamples(audio.clock.load(), originPts);
                audio.writtenUntil = audio.clock.load();
                audio.position = audio.clock.load();
            }
            wasPlaying = false;
            // 暂停时不动解码状态：恢复播放能接着刚才那一帧继续。
            wait(10);
            continue;
        }
        // 暂停（media->paused，含熔断期间被按回 true）时同样一块都不该解。
        // 从前这里放行到"攒好一整块、临写之前再看一眼能不能写"，然后丢掉那块 continue——
        // 下一轮立刻再解一块，于是**暂停期间音频线程会连着把整条队列吃空**。
        // 实测现场：946 个音频包在不到一秒里被解出来又全丢掉（日志 audio-drop #1…#451，qa 946→0）。
        // 音频数据一旦被丢就回不来了（除了重新读源）：队列空 → writtenUntil 永远停在原处 →
        // 时钟永久冻住 → 视频线程等一个不会走的主时钟 → 画面冻死，而视频队列里还有 23.6 秒的内容。
        // 这就是"卡死之后点播放也没用"的真正收尾。写不了就别解，等能写的时候再解。
        if (media->paused) {
            wait(10);
            continue;
        }
        // 找一块已经播完的缓冲；全部都在队列里就稍等，保证是"排队播放"而不是"播一块解一块"。
        int index = -1;
        for (int i = 0; i < kAudioBufferCount; ++i) {
            if ((audio.headers[i].dwFlags & WHDR_INQUEUE) == 0) { index = i; break; }
        }
        if (index < 0) {
            wait(3);
            continue;
        }
        int16_t* out = audio.pcm.data() + static_cast<size_t>(index) * framesPerBuffer;
        int filled = 0;
        while (filled < kAudioSamplesPerBuffer && !media->stop && !audio.stop && audio.playing) {
            commit.unlock();
            commit.lock();
            if (media->seeking || epoch != audio.clockEpoch.load()) break;
            // 这一轮之前的样本是上一轮写进来的，已经按当时的增益缩放过，所以每次只缩"本轮新写的"那段
            // （[before*2, filled*2) 个 int16，立体声交错）。重复缩放会让音量随解码节奏漂移。
            const int before = filled;
            if (!frameReady) {
                if (!packetReady) {
                    // 包由 demuxLoop 从那唯一一条连接上分过来，这里**绝不读网络**。
                    // 队列空就是"缓冲还没到位"：源已读完就冲解码器收尾，否则退让一拍再试。
                    AVPacket* next = media->audioQueue.pop();
                    if (!next) {
                        if (media->audioQueue.eof) {
                            if (!drained) {
                                const int sent = avcodec_send_packet(audio.codec, nullptr);
                                drained = sent >= 0 || sent == AVERROR_EOF;
                            }
                            const int received = avcodec_receive_frame(audio.codec, frame);
                            if (received >= 0) {
                                frameReady = true;
                            } else {
                                eof = received == AVERROR_EOF;
                                if (eof) {
                                    uint8_t* tail[] = { reinterpret_cast<uint8_t*>(out + filled * 2) };
                                    const int got = swr_convert(audio.resampler, tail,
                                        kAudioSamplesPerBuffer - filled, nullptr, 0);
                                    if (got > 0) {
                                        const int gain = audio.volume.load();
                                        for (int i = filled * 2; i < (filled + got) * 2; ++i)
                                            out[i] = static_cast<int16_t>(out[i] * gain / 1000);
                                        filled += got;
                                    }
                                }
                                break;
                            }
                        } else {
                            // 队列空 = 这一轮一块都写不出去，时钟就此冻住，视频也跟着停。
                            // 这正是"seek 完卡住"最可疑的一条路，必须留痕（按 200ms 节流）。
                            const auto now = std::chrono::steady_clock::now();
                            if (now - lastStarve > std::chrono::milliseconds(200)) {
                                lastStarve = now;
                                noteSyncAudio("starve",
                                              originPts + static_cast<double>(writtenSamples) / 44100.0,
                                              audio.clock.load(), originPts, 0,
                                              media->audioQueue.size(), audio.playing ? 1 : 0);
                            }
                            wait(5);
                            break;
                        }
                    } else {
                        // 队列里的包必定是音频流（demuxLoop 已按流号分流），直接接管所有权。
                        av_packet_unref(packet);
                        av_packet_move_ref(packet, next);
                        av_packet_free(&next);
                        packetReady = true;
                    }
                }
                if (!frameReady) {
                    const int sent = avcodec_send_packet(audio.codec, packet);
                    if (sent == AVERROR(EAGAIN)) {
                        // 解码器缓冲满了：先取一帧出来再重发同一个包。
                        // 这里绝不能把包丢掉，否则音频会出现可听见的卡顿。
                        if (avcodec_receive_frame(audio.codec, frame) >= 0) {
                            frameReady = true;
                        } else {
                            av_packet_unref(packet);
                            packetReady = false;
                            eof = true;
                            break;
                        }
                    } else if (sent < 0) {
                        av_packet_unref(packet);
                        packetReady = false;
                        continue;
                    } else {
                        av_packet_unref(packet);
                        packetReady = false;
                        const int received = avcodec_receive_frame(audio.codec, frame);
                        if (received == AVERROR(EAGAIN)) continue;
                        if (received < 0) { eof = true; break; }
                        frameReady = true;
                    }
                }
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE) lastTimestamp = frame->best_effort_timestamp;
                // 落点**之前**的预滚帧只用来喂解码器，不能放出来、更不能拿它锚定时钟。
                // 为什么会有这种帧：定位落在"落点之前最近的关键帧"上（见 kSeekBackSeconds），
                // 本地 20 秒的源只在 0 秒有一个关键帧，于是 seek 到 2 秒时音频是从 0 秒开始
                // 解出来的。照放的话用户拖到 2 秒却先听到开头那两秒；更糟的是下面那句
                // "用真实帧时间戳锚定时钟"会把锚点从 2.0 改回 0.023——整条时间轴退回落点之前，
                // 画面重放一遍、进度条却卡在落点不动（实测卡 2.7 秒）。
                if (frameReady && audio.stream && audio.stream->time_base.den &&
                    lastTimestamp != AV_NOPTS_VALUE &&
                    lastTimestamp * av_q2d(audio.stream->time_base) < originPts - 0.001) {
                    frameReady = false;
                    continue;
                }
            }
            uint8_t* output[] = { reinterpret_cast<uint8_t*>(out + static_cast<size_t>(filled) * 2) };
            const int got = swr_convert(audio.resampler, output, kAudioSamplesPerBuffer - filled,
                                        const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            // 整帧喂给重采样器，装不下的输出留在它自己的缓冲里，下一次继续取。
            frameReady = false;
            if (got <= 0) continue;
            filled += got;
            const int gain = audio.volume.load();
            if (gain < 1000) {
                for (int i = before * 2; i < filled * 2; ++i)
                    out[i] = static_cast<int16_t>(out[i] * gain / 1000);
            }
        }
        if (media->seeking || epoch != audio.clockEpoch.load()) continue;
        if (filled <= 0) {
            if (eof) {
                bool pending = false;
                for (const auto& header : audio.headers)
                    if (header.dwFlags & WHDR_INQUEUE) pending = true;
                if (pending) { wait(3); continue; }
                audio.clock = audio.writtenUntil.load();
                audio.position = audio.clock.load();
                // 不用退出线程：重新播放或拖动进度条时还要用它。
                audio.playing = false;
                audio.finished = true;
                // 音轨排空**不等于**整个媒体播完。音轨包短、数量是视频的两三倍，必定先排空；
                // 从前这里无条件写 ended/paused，于是音频一结束就把视频全局暂停——
                // 现场就是"声音放完了，画面才动（或者干脆冻住不动）"。
                // 只有纯音频源（根本没有视频流）才由音频宣布结束。
                if (!media->hasVideo) {
                    media->ended = true;
                    media->paused = true;
                    audio.position = audio.clock.load();
                }
            }
            continue;
        }
        // 攒到一半时用户按了暂停、或者播放位置追上了读前沿（等缓冲）：
        // 这一块绝不能写进声卡。写了就是"暂停之后还多响一块（46ms）"，
        // 更要命的是写入前沿会往前走、时钟跟着跳，恢复播放时音画就错开了。
        if (!audio.playing || media->paused || media->rebuffering) {
            // 这一块只能扔。注意这里是**兜底**：正常路径在循环开头就把"不能写"的情况挡掉了
            // （见上面 media->paused 那一段），不会走到这里——否则暂停期间会一边解一边扔，
            // 把整条音频队列吃空，队列一空时钟就永远停在那里，视频跟着一起冻死。
            continue;
        }
        // 本次锚定写出的第一块：把时钟锚点接到真实的音频帧时间戳上。
        // 落点（seek）给的是容器层面的时间，而音频流自己的首帧时间戳可能与它差几十毫秒
        // （AAC 的 priming delay、编辑列表），以真实帧时间戳为锚，时钟才和听到的内容严格对齐。
        if (writtenSamples == 0 && lastTimestamp != AV_NOPTS_VALUE &&
            audio.stream && audio.stream->time_base.den)
            // 只许往前微调（抵消 AAC priming 之类几十毫秒的偏差），绝不许把锚点拉回落点之前：
            // 第一块对应的时间戳若仍早于锚点，说明预滚还没走完，此时以锚点为准。
            originPts = std::max(originPts, lastTimestamp * av_q2d(audio.stream->time_base));
        if (filled < kAudioSamplesPerBuffer)
            std::fill(out + static_cast<size_t>(filled) * 2, out + framesPerBuffer, static_cast<int16_t>(0));
        WAVEHDR& header = audio.headers[index];
        if (!(header.dwFlags & WHDR_PREPARED) &&
            waveOutPrepareHeader(audio.waveOut, &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            media->error = true;
            audio.playing = false;
            setError(L"waveOut 缓冲准备失败");
            continue;
        }
        if (waveOutWrite(audio.waveOut, &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            media->error = true;
            audio.playing = false;
            setError(L"waveOut 写入失败");
            continue;
        }
        wasPlaying = true;
        // 这一块真的进了声卡：推进"写入前沿"。块长度固定按 2048 个样本算（不足的尾部补了零，
        // 时间照样流逝），所以前沿 = 锚点 + 累计样本 / 44100。
        writtenSamples += kAudioSamplesPerBuffer;
        const double writtenUntil = originPts + static_cast<double>(writtenSamples) / 44100.0;
        audio.writtenUntil = writtenUntil;
        // 时钟 = 写入前沿 − 声卡里还没播完的那几块。
        // 误差只来自"正在播的那一块已经播了多少无从得知"，也就是不到一块（46ms），
        // 而一帧画面本身就 40ms，这个精度对音画对齐完全够用。
        int pendingBlocks = 0;
        for (int i = 0; i < kAudioBufferCount; ++i)
            if (audio.headers[i].dwFlags & WHDR_INQUEUE) ++pendingBlocks;
        const double pending = static_cast<double>(pendingBlocks) * kAudioSamplesPerBuffer / 44100.0;
        audio.clock = std::max(originPts, writtenUntil - pending);
        // 诊断：写出一块 PCM 就是时钟前进一次，这里能看到时钟的推进节奏与锚点。
        noteSyncAudio("write", writtenUntil, audio.clock.load(), originPts, pendingBlocks,
                      media->audioQueue.size(), audio.playing ? 1 : 0);
        // 进度条也走这个时钟：它比"最后写进去的那一帧的时间戳"准得多——
        // 后者天然领先用户听到的声音最多 8 块（371ms），控件条会一直跑在声音前面。
        audio.position = audio.clock.load();
    }
    if (audio.waveOut) waveOutReset(audio.waveOut);
    av_frame_free(&frame); av_packet_free(&packet);
}
// 打开一路音频：连接部分复用 openInput()（与视频共用同一条连接），其余是音频自己的事——
// 解码器、S16 立体声重采样器、waveOut 设备、8 个播放缓冲，最后启动 audioLoop。
// 纯音频源（没有视频流的那种，比如音乐站点）同样走这里：openInput 是幂等的，
// 谁先来谁把连接开好，另一方直接用。
bool openAudio(Media* media) {
    auto& audio = media->audio;
    // 每次开都从干净状态开始：失败原因只在本次尝试里有效（closeFfmpegAudio 不动 fail，
    // 所以失败原因必须在 close 之前写完）。
    audio.fail.clear();
    if (!openInput(media)) { audio.fail = media->video.format ? "audio-open" : "open"; return false; }
    // 源里根本没有音轨：这是合法情况（不少测试源就是纯视频），但要和"音轨在、打不开"分开报。
    if (audio.streamIndex < 0 || !audio.stream) {
        audio.fail = "no-stream";
        closeFfmpegAudio(audio);
        return false;
    }
    const AVCodec* decoder = avcodec_find_decoder(audio.stream->codecpar->codec_id);
    if (!decoder) { audio.fail = "decoder"; closeFfmpegAudio(audio); return false; }
    audio.codec = avcodec_alloc_context3(decoder);
    if (!audio.codec || avcodec_parameters_to_context(audio.codec, audio.stream->codecpar) < 0 || avcodec_open2(audio.codec, decoder, nullptr) < 0) {
        audio.fail = "codec-open"; closeFfmpegAudio(audio); return false;
    }
    const int inputRate = audio.codec->sample_rate > 0 ? audio.codec->sample_rate : 44100;
    if (audio.codec->ch_layout.nb_channels <= 0) av_channel_layout_default(&audio.codec->ch_layout, 2);
    AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&audio.resampler, &outputLayout, AV_SAMPLE_FMT_S16, 44100,
                            &audio.codec->ch_layout, audio.codec->sample_fmt, inputRate, 0, nullptr) < 0 ||
        swr_init(audio.resampler) < 0) {
        audio.fail = "resampler"; closeFfmpegAudio(audio); return false;
    }
    audio.waveFormat.wFormatTag = WAVE_FORMAT_PCM;
    audio.waveFormat.nChannels = 2;
    audio.waveFormat.nSamplesPerSec = 44100;
    audio.waveFormat.wBitsPerSample = 16;
    audio.waveFormat.nBlockAlign = audio.waveFormat.nChannels * audio.waveFormat.wBitsPerSample / 8;
    audio.waveFormat.nAvgBytesPerSec = audio.waveFormat.nSamplesPerSec * audio.waveFormat.nBlockAlign;
    if (waveOutOpen(&audio.waveOut, WAVE_MAPPER, &audio.waveFormat, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        // 这条过去是静默失败：机器没有可用音频设备 / 独占模式下都会走到这里，
        // 表现就是"视频画面正常但一点声音都没有"，而日志与页面都看不出任何异常。
        audio.fail = "waveout-open"; closeFfmpegAudio(audio); return false;
    }
    // 一次性把缓冲池准备好，之后只是往空闲缓冲里写数据，避免播放中途反复准备/释放缓冲。
    audio.headers.resize(kAudioBufferCount);
    audio.pcm.assign(static_cast<size_t>(kAudioBufferCount) * kAudioSamplesPerBuffer * 2, 0);
    for (int i = 0; i < kAudioBufferCount; ++i) {
        WAVEHDR& header = audio.headers[i];
        header = {};
        header.lpData = reinterpret_cast<LPSTR>(audio.pcm.data() + static_cast<size_t>(i) * kAudioSamplesPerBuffer * 2);
        header.dwBufferLength = static_cast<DWORD>(kAudioSamplesPerBuffer * 2 * sizeof(int16_t));
        if (waveOutPrepareHeader(audio.waveOut, &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            audio.fail = "waveout-prepare"; closeFfmpegAudio(audio); return false;
        }
    }
    audio.duration = audio.stream->duration > 0 ? audio.stream->duration * av_q2d(audio.stream->time_base) : 0.0;
    // 流级时长普遍缺失（HLS、不少 mp3/wav、以及只在容器层给时长的 mp4）：回落到容器总时长。
    // 视频那条早就有这个回落，音频这条漏了——缺了它 `duration` 恒为 0，控件条总时长显示 --:--、
    // 进度条更是彻底拖不动（页面侧 input 处理里 `if(s.duration>0)` 整段被跳过），
    // 观感就是"音频根本没加载"。注意 AVFormatContext::duration 的单位是微秒（AV_TIME_BASE）。
    if (audio.duration <= 0 && media->video.format && media->video.format->duration != AV_NOPTS_VALUE)
        audio.duration = static_cast<double>(media->video.format->duration) / AV_TIME_BASE;
    audio.playing = false;
    return true;
}

} // namespace nmb
