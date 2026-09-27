#pragma once
// nmb_types.h —— 桥的核心运行时数据结构（FfmpegAudio/FfmpegVideo/Media/Browser/RectI）。
// 注意：本头只由 nmb_internal.h 包含（依赖其先行引入的 Windows/FFmpeg/标准库头与类型别名）。
namespace nmb {
// ── §2 数据结构 ───────────────────────────────────────────────────────────
// FfmpegAudio/FfmpegVideo：一条媒体在 native 侧的全部 ffmpeg 句柄与播放状态；
// Media：页面对应的一个 <audio>/<video>（可能同时含音轨+画面）；
// Browser：一个浏览器实例的内核 view、窗口句柄、媒体表与合成节流标记。
// 一条流（视频或音频）的"已解复用、待解码"包队列。
// 解复用只有一个线程（demuxLoop 在共享连接上读），它按流号把包分别投进两条队列，
// 视频/音频各自的解码线程只消费自己这条——谁也不碰网络，谁也不和谁抢连接。
// 过去是 video/audio 各开一条连接各自 av_read_frame：同一段字节下两遍，seek 时两边
// 各自定位、落点还不一致（视频落关键帧、音频落帧边界），音画就此错开。
struct PacketQueue {
    std::deque<AVPacket*> packets;
    // 源已读到末尾：队列排空之后就是真的播完了（要和"暂时没解出包"分开）。
    std::atomic<bool> eof{false};
    std::mutex mutex;
    ~PacketQueue() { clear(); }
    // 入队接管所有权。
    void push(AVPacket* packet) {
        std::lock_guard<std::mutex> lock(mutex);
        packets.push_back(packet);
    }
    // 出队，空则返回 nullptr（调用方拿不到包时本来就要退让一拍，不必在这里等）。
    AVPacket* pop() {
        std::lock_guard<std::mutex> lock(mutex);
        if (packets.empty()) return nullptr;
        AVPacket* packet = packets.front();
        packets.pop_front();
        return packet;
    }
    // 看一眼队首但**不取走**：解码线程在暂停时要用它判断"队首这个包是否已经越过当前位置"，
    // 越过了就说明再解下去就要跑到还没播到的地方，该停手把包留在队列里（见 decodeLoop）。
    bool peekTimestamp(int64_t& stamp) {
        std::lock_guard<std::mutex> lock(mutex);
        if (packets.empty()) return false;
        const AVPacket* packet = packets.front();
        stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        return true;
    }
    // 丢弃全部待解码包（seek 之后用：新位置的包才有效）。
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        for (AVPacket*& packet : packets) av_packet_free(&packet);
        packets.clear();
    }
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return packets.size();
    }
};

struct FfmpegAudio {
    AVCodecContext* codec{};
    AVStream* stream{};
    int streamIndex{-1};
    SwrContext* resampler{};
    WAVEFORMATEX waveFormat{};
    HWAVEOUT waveOut{};
    std::vector<WAVEHDR> headers;
    std::vector<int16_t> pcm;
    std::atomic<bool> stop{false};
    std::atomic<bool> playing{false};
    std::atomic<double> position{0.0};
    // ── 音频主时钟：整条链路上唯一有真实时间源的东西 ─────────────────────
    // 为什么必须由音频报时：视频那侧是"按帧率睡够时间就画"，只要系统调度抖一下、
    // 或某一帧没解出来，它的节拍就自己漂了——实测 seek 之后视频会跑到 1.4 倍速，
    // 而声卡还在一丝不苟地按 44100Hz 播。两个节拍各走各的，就是"音画不同步"。
    // 声卡有晶振，它才是权威；视频改成按这里的值对齐（见 decodeLoop 的 outputFrame）。
    //
    // clock 的定义：**用户此刻真正听到的那个媒体时间点**，等于
    //   writtenUntil（已经写进声卡的媒体时间前沿）− 声卡里还没播完的那几块。
    // 暂停/等缓冲时它必须冻住（那时候声音也停了），恢复时从冻住的地方接着走。
    std::atomic<double> clock{0.0};
    // 写进声卡的媒体时间前沿（秒）。audioLoop 每写完一块就推进它；seek 时重新锚定到落点。
    std::atomic<double> writtenUntil{0.0};
    // seek 代数：demuxLoop 每次定位都自增，audioLoop 发现变了就重建时钟锚点。
    // 不用"标志位"是因为两个线程都在跑，代数法天然幂等、不会丢事件。
    std::atomic<long long> clockEpoch{0};
    // 音频已经放到末尾（队列排空且源已 EOF）。注意它**不等于**整个媒体结束：
    // 有声有视的源上音轨包短、数量是视频的两三倍，必定先排空。只有纯音频源才由它宣布结束。
    std::atomic<bool> finished{false};
    double duration{};
    // 音频没起来的**原因**（ASCII 短语，见 openAudio 的各条失败路径）。
    // 视频分支里 openAudio 的返回值是故意丢掉的（视频没音轨是合法的），可 openAudio 的失败路径过去
    // 全是静默 return false —— 于是"画面在放、就是没声音"在页面侧一点痕迹都没有，只能靠猜。
    // 只由查询线程写/读（openAudio 与 mediaStateJson 都在查询线程上），不需要额外加锁。
    std::string fail;
    // 播放增益，千分比（1000 = 原样）。**不要**改回 waveOutSetVolume（见 volume op 处的注释）：
    // 那是设备级的，页面上任何一处静音都会把所有流一起打哑。这里是逐流软件增益。
    std::atomic<int> volume{1000};
};

struct RectI { int x{}, y{}, width{}, height{}; };
struct Browser;
struct Media;
// 一次网络读的"时限守卫"：挂在 format context 的中断回调上（见 interruptIo）。
// until 是纳秒时间点，0 = 不限制；超过它就返回 1，让正在阻塞的读以 AVERROR_EXIT 返回。
// **绝不能两条连接共用一份**：视频、音频是各自线程在读，共用就会出现
// "视频那次读超时把音频正在进行的读一起打断"。所以 Media 里放两份。
struct IoDeadline {
    Media* media{};
    std::atomic<long long> until{0};
};

struct FfmpegVideo {
    // 全媒体唯一的解复用上下文：音频和视频共用这一条连接（见 Media::videoQueue/audioQueue）。
    AVFormatContext* format{};
    AVCodecContext* codec{};
    AVPacket* packet{};
    AVFrame* frame{};
    AVFrame* bgraFrame{};
    SwsContext* scaler{};
    int streamIndex{-1};
    double timeBase{};
    double fps{25.0};
    double duration{};
};

struct Media {
    Browser* browser{};
    std::wstring id;
    std::wstring kind;
    std::wstring source;
    // 页面请求上下文：FFmpeg 不会自动继承 miniblink 页面的 UA、来源页和 Cookie。
    // 这些值由 kInjection 的 send() 一并传入，用于防盗链媒体和 HLS 后续请求。
    std::string userAgent;
    std::string referer;
    std::string cookie;
    std::string origin;
    RectI rect;
    // 页面自绘控件条的位置：视频帧会让开这块区域，否则控件条会被画面盖住。
    // 直接用上报表里的绝对坐标挖洞——页面侧条的位置与这份上报是同一份数字，
    // 不再存"条相对元素的偏移"做二次推算（那个行为已回退）。
    RectI barRect;
    // 元素在页面里**真正可见**的区域（页面侧 clipBox 的结果＝元素盒 ∩ overflow 裁剪祖先
    // ∩ 视口，页面视口 CSS 像素）。画面只画它与 rect 的交集：站点容器（overflow:hidden
    // 的卡片/圆角播放器）把元素裁掉多少，画面就得裁掉多少，否则原生帧会溢出"组件"边界
    // 叠在页面上（用户报的"视频内容没锁死在组件内部"）。宽或高 <=0 表示元素完全不可见
    // （被容器完全藏住/整体出界），画面整块不画。协议自 clipRect 字段起每条消息都带，
    // open 起就有有效值，不存在"旧消息没带"的歧义。
    RectI clipRect;
    // 页面里"绘制在视频元素之上"的 DOM 覆盖物（站点自绘控制栏、弹幕、弹窗、广告、loading…）。
    // 合成时视频帧必须让开这些矩形：帧是 SRCCOPY 整块盖在页面像素上的，不挖洞就会把站点自己的
    // 播放器 UI 全部压在画面下——元素其实活着（脚本 .click() 仍能控播放），但用户看不见也点不着。
    // 由注入脚本用 document.elementsFromPoint 采集（栈中 video 之前的元素才是覆盖物），
    // 经 holes op 上报，坐标系与 rect/barRect 相同（页面视口 CSS 像素）。
    std::vector<RectI> holes;
    // 软洞：背景透明、只有文字/图标像素的覆盖物（弹幕、SVG 按钮）。这些不能整块挖空——
    // 元素盒内大部分像素是"透明露出视频"，整块让开只会挖出一个黑矩形（弹幕拖黑条）。
    // 画完视频帧后，从页面备份里按亮度把覆盖物像素抠回来（见 paintBrowser）。
    std::vector<RectI> softHoles;
    FfmpegAudio audio;
    FfmpegVideo video;
    // ── 解复用（只有一条连接，见 nmb_video.cpp 的 demuxLoop）──────────────
    // 一个媒体一个 AVFormatContext：demuxLoop 在上面不停地读，按流号把包分进两条队列；
    // 读到哪算到哪，**不管在不在播**（这就是"始终加载缓冲"）。解码线程只消费队列。
    PacketQueue videoQueue;
    PacketQueue audioQueue;
    std::thread demuxer;
    // 源已读到末尾（av_read_frame 返回 AVERROR_EOF）。队列排空 + 这个为真 = 真的播完了。
    std::atomic<bool> demuxEof{false};
    // 源里有视频流（openVideo 成功时置位）。音频线程靠它判断"音频播完算不算整个媒体播完"：
    // 有声有视的源上音轨必定先排空，若音频越过视频去宣布 ended/paused，视频当场被冻住。
    std::atomic<bool> hasVideo{false};
    // 正在等缓冲：播放位置追到了读前沿，先不出帧、不推音频，等水位重新拉开再继续。
    // 页面的 paused 也按"paused || rebuffering"报——用户看到的"暂停中"就是它。
    std::atomic<bool> rebuffering{false};
    std::thread decoder;
    std::thread audioDecoder;
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{true};
    std::atomic<bool> ended{false};
    // 致命错误：解码器坏、色彩转换失败、声卡写失败…。置上就停帧，而且**只有 seekTo 会清它**，
    // 所以它本质上是个熔断标志 —— 凡是"有可能自愈"的原因都不许往这里写（见 stalled）。
    std::atomic<bool> error{false};
    // 源暂时读不到数据（连接被掐 / 源站不响应 / 带宽掉底），但流本身没坏：解复用线程还在重试，
    // 源一恢复数据就接着进来。它**不参与熔断** —— 不锁解码线程、也不改用户的播放意图，
    // 只作为"此刻正卡在等源上"的提示（页面 state 里的 stalled），读到数据即自动清零。
    std::atomic<bool> stalled{false};
    std::atomic<bool> seekPending{false};
    std::atomic<double> seekSeconds{0.0};
    // 提交门：seek 代次/取包/帧与 PCM 提交共用；codec/resampler 仅各自消费线程操作。
    // 锁序 decodeMutex -> queue.mutex/frameMutex；禁止持锁网络定位或睡眠。
    std::mutex decodeMutex;
    std::atomic<bool> seeking{false};
    std::mutex frameMutex;
    std::vector<unsigned char> bgra;
    int frameWidth{};
    int frameHeight{};
    double fps{25.0};
    std::atomic<double> position{0.0};
    double duration{};
    // 读前缓冲的水位（秒）与已从源读到的字节数：页面和测试据此判断"源是不是比播放慢"。
    std::atomic<double> bufferedSeconds{0.0};
    // 已读到的最远媒体时间点（绝对秒）：控件条的"已缓冲"段画的是它。
    // 这里必须用绝对量——相对水位要加上播放位置才换算成时间点，而往回拖进度条时播放位置变小，
    // 换算出来的时间点就跟着往回缩、色带"退"回去；可那段数据明明早就读过了。
    std::atomic<double> bufferedUntil{0.0};
    // 定位之后的"位置下限"（绝对秒）。解复用线程 seekTo 时置为落点，解码线程在解出的帧
    // 时间戳追上它之前不改 position，追上后清零。
    // 为什么需要：定位为了能拿到落点之前的关键帧，会往前多读 kSeekBackSeconds（见 nmb_video.cpp），
    // 那些预滚帧的时间戳小于落点；若照常把 position 写回去，页面上就是"拖到 14 秒、进度条先跳回 13 秒
    // 再往前走"，看着像拖偏了。
    std::atomic<double> positionFloor{0.0};
    std::atomic<long long> readBytes{0};
    // 两条连接各自的中断/读超时守卫（见 IoDeadline 与 interruptIo）：
    // 没有它们，源站挂起时 av_read_frame 会无限期阻塞，线程只能靠杀进程收场。
    IoDeadline videoIo;
    IoDeadline audioIo;
    // 通用 MSE 接管：inject.js 拦截 MediaSource/SourceBuffer 把音视频字节喂到这里，
    // 不写站点特定代码。mseMode 时无 source URL，openInput 用自定义 AVIO 从 mseVideo 读。
    bool mseMode{false};
    std::mutex mseMtx;
    std::condition_variable mseCv;
    std::vector<uint8_t> mseVideo;
    std::vector<uint8_t> mseAudio;
    std::string mseVideoMime, mseAudioMime;
    std::atomic<bool> mseVideoEof{false};
    std::atomic<bool> mseAudioEof{false};
    int readFailures{};
    // open 的异步化状态（见 mediaBridge 的 open 分支）：查询线程登记条目时置 openRunning，
    // 后台 open 线程收尾时在 browser->mutex 之内清位并写下 openOk。openRunning 期间页面问到的
    // open 一律回 busy——绝不能把半开状态当成功交出去，否则页面会接管一个没有解码器的空壳。
    std::atomic<bool> openRunning{false};
    bool openOk{false};
    std::wstring openError;
};

struct Browser {
    // 共享所有权：open 已异步化（见 mediaBridge 的 open 分支），后台 open 线程收尾时要摸
    // mutex/media，而 NMB_DestroyBrowser 可能趁它还在天上就执行。open 线程抓一份 self，
    // destroyBrowser 把 teardown 做完后只放掉自己这一份——结构活到最后一个持有者放手为止。
    std::shared_ptr<Browser> self;
    WebView view{};
    HWND hwnd{};
    // mb108 不能在"完全没有 HWND"的模式下稳定运行，但把可见 hwnd 交给它又会让它直接上屏，
    // 与本模块的合成争抢同一批像素而闪烁。给内核一个同尺寸、永不显示的工具窗：既满足其 HWND
    // 前提，又把直绘隔离到屏幕之外；用户可见 hwnd 仍只有 paintBrowser 一个绘制者。
    HWND kernelHwnd{};
    // mbSetFocus 会把 Win32 焦点转给 kernelHwnd；同步期间屏蔽这次内部跳转产生的 WM_KILLFOCUS，
    // 并把系统焦点送回可见窗，保证后续键鼠消息仍落在 browserWindowProc。
    bool syncingKernelFocus{};
    std::mutex mutex;
    std::unordered_map<std::wstring, Media*> media;
    // 合成通知的合并开关：见 notifyComposite。
    std::atomic<bool> paintPosted{false};
    // 合成缓冲：paintBrowser 先把整窗合成进这块离屏位图，最后一次性上屏（单一绘制者，没有中间态）。
    // 每帧新建/删除一个整窗大小的兼容位图（900x620 就是 2.2MB，最大化后 3.7MB+）本身的开销和视频帧
    // 一个量级，尺寸不变就一直复用。paintOriginal 是 DC 自带的 1x1 单色位图，销毁前必须换回去。
    HDC paintDC{};
    HBITMAP paintBitmap{};
    HGDIOBJ paintOriginal{};
    void* paintBits{};
    int paintWidth{};
    int paintHeight{};
    // 页面像素备份：视频帧画上去**之前**的整页内容。软洞（透明背景上的弹幕文字/图标，
    // 见 Media::softHoles 与 paintBrowser 的抠像回贴）要从这里把覆盖物像素取回，
    // 所以必须与 paintDC 同尺寸、同周期刷新。两块都是 32 位 top-down DIB，可直接按 BGRA 读写。
    HDC pageDC{};
    HBITMAP pageBitmap{};
    HGDIOBJ pageOriginal{};
    void* pageBits{};
    // 鼠标移动合流：mb108 的 mbFireMouseEvent 对"无按键移动"不派发任何 DOM 事件、也不更新
    // :hover（实测点击/滚轮正常、悬浮全失效）。WM_MOUSEMOVE 只记录最新坐标并 PostMessage
    // 合流（WM_APP+3），每轮消息循环至多调一次 JS 喂给注入脚本的 hover polyfill（__nmbHoverMove），
    // 由页面用 elementsFromPoint 自做命中并合成 mousemove/mouseover/enter/leave/pointer 事件。
    std::atomic<int> mouseX{-1};
    std::atomic<int> mouseY{-1};
    std::atomic<bool> mousePosted{false};
    // 档3：JS polyfill 写入当前 hover 元素的 cursor 类型，WM_SETCURSOR 据此设系统光标。
    std::atomic<int> cursorType{0};  // 0=default, 1=pointer, 2=text
};

} // namespace nmb
