# NativeMediaBridge-注解版 · 长期记忆

## 工程与构建
- 本工作区**不含** zig 工具链；用相邻项目那份：`..\NativeMediaBridge\.toolchain-fixed\zig-x86_64-windows-0.15.2\zig.exe`，参数照 `build-zig.bat`（MSVC 的 `build-cl.bat` 也在）。shell 是 PowerShell，别用 `cd /d ... && ...` 那套 cmd 写法，直接写全路径调用。
- 真正主项目是 `F:\编程\nw_mb\NativeMediaBridge`，不是注解版；构建后运行时 DLL 还需同步到 `F:\编程\nw_mb\nw\bin\NativeMediaBridge.dll`（必要时同步到应用目录），否则宿主会继续加载旧副本，导致悬浮 polyfill 修复看似未生效。
- `bin/NativeMediaBridge.dll` 常被用户正在跑的 `tests/test_bindwebview.py` 占用：先编到旁路名（`bin/NativeMediaBridge-test.dll`），宿主用 `--dll <路径>` 指定。宿主参数：`--page <路径>`（相对工作区根）、`--auto`、`--duration N`、`--dll`。
- **改 `hook/js/*.js` 后必须先 `python _nmb_gen_hooks_inc.py` 再编译**：`page/nmb_inject.cpp` include 的是 `hook/inc/*.inc`（生成物）。生成器 2026-09-18 重建（原脚本丢失），要点：MSVC 字面量 ≤16380B → 多段 `R"NMBHOOK(...)NMBHOOK"` 拼接（段界在 `\n`、≤15999B、UTF-8）；验证用"各段拼回内容等价"而非逐字节。`--check` / `--diff` / 默认生成（.bak 备份）。
- FFmpeg 是 **libavformat 63.1.100（FFmpeg 8.1）**，`ffmpeg-static/` 只有头与静态库、**无源码**。要判 ffmpeg 行为只能把上游源码拉到 `%TEMP%` 再本地 grep（`Invoke-WebRequest raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavformat/mov.c`）。**别用 web_fetch 抓**——会中途截断，得到假的"未出现该字符串"结论。

## 控件条 CSS（2026-09-18）
- **样式真源仍是 `hook/css/`**：用户撤销了"迁出到 nw_mb/css"的移动，但保留了数字前缀消除——文件＝`base.css`/`tier.css`/`audio.css`（无 01/02/03 前缀），另有 preview.html 与 README.md。生成器（根 + `utility/` 两份 `_nmb_gen_hooks_inc.py`）的 `CSS_DIR` 指回 `hook/css`。
- **级联顺序＝生成器 `CSS_ORDER` 表（base→tier→audio），不能按文件名字母序**：字母序会把 audio 排到 base 前，audio.css 的 4 条同特异度尺寸规则（min-width 36/90、width 52）会被 base 盖回去。新增 css 文件默认按字母序接尾；要插中间就改 `CSS_ORDER`。

## 内核（miniblink）事实
- 现役内核 **Chromium 60**（miniblink 2023 版 `miniblink_x64.dll`）。**CSS 新特性会静默失效**：`linear-gradient` 的"颜色 起 止"双位置断点（CSS Images 4，要 Chrome 71+）整条渐变判非法 → `background` 计算值 `none`（"进度条没色带"的根因之一，已改六断点写法，新旧内核通吃）。改 CSS 先查这类新语法。
- **内核丢弃一切经 `setProperty` 写的自定义属性**：内联的丢（`el.style.setProperty('--x',…)`）、CSSOM 规则上的也丢（`rule.style.setProperty('--x',…)`）；只有**样式表里的声明**认（跨表 `var()` 也能解析到）。`getPropertyValue('--x')` 与伪元素 `getComputedStyle(el,'::before')` 读回一律失效——**判"内核认不认"只能看算出的真实颜色**。"进度条没色带"的另一个根因就是它（`--nmb-track` 从没送到轨道）。可用替代：`insertRule`/`deleteRule` 重写规则，或直接写伪元素规则的 `rule.style.background`（现用后者：`hook/js/injection.js` 的 `trackRule`/`paintTrack`，基 CSS 只留几何与回退色）。
- **mb108 内核没有"native→页面同步取值"通道**：mbRunJs 结果回调给 `es=nullptr` + 小整数句柄，`mbJsToString` 用不了；title 通道会污染窗口标题。取页面结论走"页面自己 POST 到 `smoke_test/_nmb_dump_server.py`，落 JSON"这条。

## 控件条 / 几何（用户的硬口径）
- **布局全交 CSS，JS 一个坐标都不写**（用户 2026-09-18 两次明确：原话"通过 css 更新好就行了，不要乱搞""我不希望使用代码来控制控件的绝对布局的定位……而是使用 css 的相对布局来将他们控制在内部"）。`rAF + style.left/top/width` 会被视作离谱方案，即便加了"值变了才写"的缓存。要的形态：事件驱动（`ResizeObserver` / `input` / `change`）或纯 CSS（子元素 absolute + inset、data-* 切状态）。
- **ui9 定稿 = 包装盒 + 纯 CSS 相对布局**：桥自建 `<div data-nmb-wrap>` 包住媒体元素（占元素原布局位，`[data-nmb-wrap]{position:relative;line-height:0}`），条作为其子元素由 `01-base.css` 的 `absolute;left:0;right:0;bottom:0` 贴盒底；JS 只打 `-off/-fill/-narrow/-tiny/-fsbar` 状态属性，挖洞矩形**照实读条 DOM**。**全屏态（2026-09-18 改定）fixed 的也只是包装盒**：`fsEnter` 把 `data-nmb-fs="1"` 打在壳上，壳铺满视口、元素仍是壳的流内子元素（`elPos=static`，元素版 `[data-native-media][data-nmb-fs="1"]` 规则已删）；选择器必须写 `[data-nmb-wrap][data-nmb-fs="1"]`——条上的全屏按钮也带 `data-nmb-fs`（buildBar 记号），裸选会把按钮拉成 100vw×100vh。条全程留在壳内（壳=视口 → absolute 贴壳底=贴视口底），placeBar"挪文档根"只剩站点把元素挪出壳的兜底。ui8 的"元素 fixed + placeBar 写视口坐标"与 ui7 的"宿主盒"都被用户否掉（原话"不要随便脱离流"）。
- **`wrapEl` 的逐像素保险会让"带 margin 的元素"拆包 → 干脆不建条**（包装前后各量一次元素盒，布局被改写就退回）。包装盒把元素 margin 搬到 wrap 上、`[data-nmb-wrap]>[data-native-media]{margin:0!important}` 再把元素自己清零；清不干净就退化成无条。**诊断页/取证页给媒体元素别写 margin**，留白交给容器 padding。
- **显隐口径（ui10）＝只有元素完全看不见才收条**（完全出界或被 overflow 祖先吃掉＝`clipBox` null；`<16px` 高的盒子不画）。部分越界照常显示——条锁在包装盒里跟元素一起被真实裁剪，与浏览器原生控件行为一致。旧口径"部分越界就整条隐藏"已废。
- **挖洞位置＝上报的 `barRect` 绝对坐标，直接用，不做加减**。**画面裁剪 `clipRect` 的权威写入点在 `syncAll`（算 ckey 处）**，绝不能放 `placeBar`（那里开头 `if(!bar)return`，拆包无条的元素会被误判"完全不可见"连画面一起砍）。排障 `NMB_PAINT_LOG`；取证页 `tests/testjs/_nmb_ui_clip.html`。
- **几何的权威只有 `rect` 与 `holes` 两个 op**：`send()` 每条消息顺手带的 rect/barRect 不能照单全收，否则 200ms 轮询会把画面和洞按绝对坐标重摆（症状＝"进度条和视频内容一直在更新"）。没动就不碰几何。
- **`syncAll` 的 rect key 必须含"条自己的状态"**（`__nmbOff`、`__nmbBarRect`）：条的几何由 CSS 给出、不等于脚本常量，显隐/fill 翻转/全屏搬移要靠 key 变化把条的实测矩形过桥给 native 挖洞。
- **音频控件条样式用户已满意**（四角圆角 8px、撑满原生 54px 盒），要动只动视频条（视频条是直角，圆角会露画面尖角）。`paintBrowser` 的 `drawn rect` 是 buffer 坐标（已减 target），别当窗口坐标。
- **video/audio 元素默认样式一律去掉**（接管后 `opacity:0!important`，unhook 还原）；画面由 native 画在合成层。**画面缩放＝等比 contain，留黑边可接受**，别改 fill。
- **滚动瞬态是架构极限**：rect 走 mbQuery 异步，快速滚动时画面追 1-2 帧，滚停即恢复。"速度上报+native 外推"试过并移除。用户报"滚动后位置不正常"先分：letterbox 黑边 ≠ 瞬态错位 ≠ 稳态错位。

## 验证手段
- UI 取证：`_nmb_ui_shot.py --dll=<产物> --out=<目录> --page=<页面> --duration N` 截图 + 抓窗口标题。配套页 `tests/testjs/_nmb_ui_lock.html`（分档滚动、rect/barRect 写进标题）、`_nmb_ui_lag.html` + `_nmb_lag_measure.py`（量稳态滚动滞后，看中位数）、`_nmb_bar_diag.html`（进度条诊断：原生字段 + `trackRule` 那条规则的真实内容 + 渐变解析能力 + 渲染参照物，结论 POST 落盘）。**当前环境读不了图片**，判色带用 PIL 采样统计像素（`smoke_test/_tmp_px.py`，`路径@x,y,w,h` 形式传区域，别用 `#`——PS 会当注释）。注意：截图是**整窗含标题栏/边框**，与页面 CSS 坐标恒差 **(+8,+31)**，拿页面坐标直接去采样会得到一片白；取证页在 (4,4) 放 6×6 洋红基准点，先按它反推偏移。`_tmp_px.py` 除颜色计数外还给色块**包围盒**（判"色带在不在、多宽"靠它，只看计数不够）。
- `ffprobe -v error -show_entries packet=stream_index,pos,dts_time -of csv=p=0 <file>` 反查日志里的 `offset 0x…`。**列序是 `stream_index,dts_time,pos`，pos 在第 3 列**。
- 判"桥的锅还是源的锅"：先 `ffmpeg -v error -i <url> -f null -` 复现（远程源被掐断很常见，别先怀疑自己的缓冲逻辑）。
- 同步/写卡：`NMB_SYNC_LOG=<路径>` → `_nmb_bufstat.py "_nmb_log_*.log"`（量"定位→首次写卡"、声卡放空、>0.4s 写卡间隔）；`_nmb_sync_grab.py`（拖进度条整轮探针，`--url/--seek/--dll/--audio`）。
- 限速源：`_nmb_slow_http.py --port 8100 --rate <B/s> [--stall 停:停]`，**不传 `--stall` 默认每 8 秒停 3 秒**，要无停顿显式 `--stall 0:0`。样片 `tests/testjs/_nmb_press.mp4`（120s/5.34MB，45500 B/s ≈ 1 倍速）。
- 判据：**写卡间隔大 ≠ 断音**，只有"写完 `pend==1`"才等价于写前声卡是空的。
- 临时诊断打 **stderr**（宿主会带出来，探针读 `proc.stderr`），别写 `%TEMP%` 文件；用完 grep 关键字确认零残留再出新 DLL。`_nmb_*.py` / `_nmb_log_*.log` 都是用完可删的临时件，不是项目代码。

## 缓冲/定位设计口径
- 解复用**单连接单线程**（`demuxLoop`）按流号分流到 `videoQueue`/`audioQueue`；两个解码线程只消费队列。
- 水位＝`bufferedUntil − consumed`；`consumed` 三支（有声有视取 `min(position, clock)`、纯音频在播取 `clock`、纯音频不在播取 `audio.position`）。
- 定位有"首轮填充"档（`startupFill` + `kStartupFillSeconds`）与稳态迟滞（`kRebufferLow/HighSeconds`），别混用。
- 已缓冲区间是 `[队列首包, bufferedUntil]`，队首≈播放位置 —— 只有**往前**的落点可能就地复用，倒退一定救不回来。

## 播放状态机的坑（2026-09-18 已修："卡死之后点播放没用"）
- **音频是主时钟，音频一死画面必死**：`audio.clock`＝写入前沿 − 声卡未播完块数，不再写卡就永远停住。任何"暂停/等缓冲期间仍解码"的路径会先掏空 `audioQueue` → 时钟冻住 → 视频线程永久等不到拍子。判据：心跳里 `qa=0` 而 `qv` 很大。
- 三个缺陷：①音频循环把"能不能写"的判断放在攒完一块之后（暂停期间不到 1 秒吃掉 946 包）；②一个坏包就写 `error` 熔断 → `paused` 常驻；③停摆的音频时钟仍被当主时钟 → `outputFrame` 变永久睡眠。对应常量 `kBadPacketLimit=8`、`kAudioClockStallMs=500`、音频循环开头的 `if (media->paused) sleep+continue`。
- **排除过的假设（勿重排）**：`play`/re-open 不清 `error`；`mediaStateJson` 缺 `"ok":true`（带了的）；HTTP 超时后未重建（mov 会 Range 自救）。
- 验证组合：`_nmb_stall_probe.py --dll=<产物>` + `_nmb_slow_http.py --rate 300000 --stall 8:22`（样片 `_nmb_stall_src.mp4`）。修复前 36.3s 冻死变 P；修复后 4.9→37.8 连续推进、停顿自愈。

## 协作偏好
- 用户在意 token/成本：分层选模型、探索交 code-explorer 子代理、长任务靠 memory 承接、口径固化成脚本、长内容落文件而非贴聊天。
- **"安装类"写盘操作（装 skill/插件、改别的软件目录）交脚本让用户自己跑，不要代他执行**（2026-09-17 装 Caveman skill 时明确要求）。脚本内建 `-DryRun`/`-List`/`-Force` 并自带回读校验。
- 删除**被其他软件占用**的文件：删除工具走"移到回收站"、占用时静默失败（返回 success 却没删）。必须：停进程 → 删 → `Test-Path` 回读。
- C 盘常年吃紧（约 6.6GB 可用），E 盘宽裕（约 480GB）；AI 相关大体积数据放 `E:\AI\`（实际大写 `AI`）。
