## NativeMediaBridge

- Windows x64 原生媒体桥。外部接口为 C ABI，内部动态加载 `miniblink_x64.dll`，使用 FFmpeg C API 解码音频与视频，音频通过 Windows `waveOut` 输出。DLL 当前会自行创建 miniblink 窗口，不会自动接管 MBPython 已创建的 WebView。

- 最早通过古法编写在一个单文件中，后面由ai进行拆分以及完善

- 该项目是给老款miniblink写的媒体桥，现在已经不维护了，目前会拿去给另外一个应用，但是正常情况下意义不大，但是可以作为参考，mb108这个新款内核当时 B 站无法正常启用，感觉是内核问题，拿来做测试的新款内核，理论可以使用，但没有开启 Nood，实际上有可能遇到很多其他的问题，请根据自身需求使用

- 第三方组件的引用署名、许可证与发行义务见 **[`开源声明.md`](开源声明.md)**（FFmpeg LGPL 静态链接、miniblink 内核等）。

## 许可证

本项目自身代码为 **LGPL-3.0-or-later**（全文见 [`LICENSE`](LICENSE)，LGPL-3.0 所补充的 GPL-3.0 见 [`COPYING`](COPYING)）。

- 第三方应用通过公开 C ABI（`LoadLibrary` / `ctypes`）动态加载本 DLL 调用时，**调用方无需开源**，但不得限制用户替换本 DLL，发行时需随附 LGPL 文本与本 DLL 的源码（Minimal Corresponding Source）。
- 桥内静态链接了 LGPL-2.1-or-later 的 FFmpeg，因此不要给 FFmpeg 加 `--enable-gpl` 或引入 GPL-only 组件，否则整个 DLL 将被迫转为 GPL。


## 当前能力

- DLL 自己创建 miniblink 窗口，可最大化/还原（`NMB_SetMaximized`/`NMB_IsMaximized`）。
- 每次 Document Ready 都重新注入脚本（页面跳转后媒体桥依然可用），并在文档切换时释放旧文档的媒体实例。
- 自动扫描当前和动态新增的 `<audio>`、`<video>`。
- 转发 `load()`、`play()`、`pause()`、音量和 DOM 位置。
- `<audio>` 和视频音轨统一由 FFmpeg 解码，PCM 通过 Windows `waveOut` 输出。
- 音频输出使用 8 个缓冲排队播放（不是"解一块播一块"），避免解码间隙造成声音断续。
- `<video>` 由 FFmpeg 解码 BGRA 帧并覆盖绘制到元素区域。
- 元素位置由注入脚本在每帧（`requestAnimationFrame`）同步给桥，页面滚动时画面与元素保持贴合，滚出视口后不再绘制。
- 声明了 `controls` 的媒体元素：隐藏内核自带控件，改由本桥绘制控件条，进度条拖动直接驱动原生解码 seek（内核控件读不到解码进度，所以不能直接用）。
- 控件条是悬浮在元素底部的固定层（近似 Chrome 的样式）；因为原生帧绘制在网页之上，绘制视频帧时会挖掉控件条所在的矩形，让它不被画面盖住。
- 网页可调用 `__nmbMedia('title','page','page','',文本,...)` 把文本回写为宿主窗口标题，宿主据此读取页面状态。

## 依赖

**运行时**（交付只需 `bin\` 两个 dll）：

- Windows x64
- `bin\miniblink_x64.dll` —— miniblink 浏览器内核（随仓，由桥动态加载）
- `bin\NativeMediaBridge.dll` —— 本桥；FFmpeg 已静态链入，不需要额外的 avcodec/avformat 等 dll
- Windows Multimedia `winmm`（系统组件）

**构建时**：

- zig 0.15.2（首选，自带 mingw 工具链）：由 `download-toolchain.ps1` 下载到 `.toolchain\zig-x86_64-windows-0.15.2\`（不进版本库；MSVC 仅备用）
- FFmpeg 9 静态库（`ffmpeg-static\`，随仓；也可用 `build-ffmpeg-static.bat` 自造，产物落在 `.toolchain\ffmpeg-static\`）
- Python 3（可选；构建时自动重新生成 `hook\inc`，没有则沿用随仓生成物）
- 其余工具（nasm / FFmpeg 源码 / 共享 SDK）：均经 `download-toolchain.ps1` 落到 `.toolchain\`

## 构建脚本怎么用

四个脚本都**在项目根目录直接运行**，不要 cd 到子目录：

| 脚本 | 用途 | 何时用 |
|---|---|---|
| `download-toolchain.ps1` | 把构建工具链下载到 `.toolchain\`：zig（首选构建硬依赖）、nasm、FFmpeg 源码、共享 SDK | 新机器 / CI 首次构建前跑一次；`build-zig` 只需 zig，ffmpeg-static 随仓已在根目录 |
| `build-zig.bat` | **首选**。zig c++ 静态链接 FFmpeg，产 `bin\NativeMediaBridge.dll`，构建后自动清掉 .lib/.exp/.pdb | 日常开发、发布。需要 zig 0.15.2（由 `download-toolchain.ps1` 下到 `.toolchain\`，或加入 PATH） |
| `build.ps1` | 同 build-zig，但本机找不到 zig 时会**自动从 ziglang.org 下载** 0.15.2 到根目录 `.toolchain\` | 新机器首次构建、联网环境 |
| `build-cl.bat` | 用 MSVC `cl` 链接 ffmpeg-sdk 的 shared 版 FFmpeg（产物运行时需自带 5 个 av*.dll） | zig 不可用、且已自行放回 `.toolchain\ffmpeg-sdk\` 时的备用方案 |
| `build-ffmpeg-static.bat` | **一次性**。从 `.toolchain\ffmpeg-src` 的 FFmpeg 源码树自编译精简静态库到 `.toolchain\ffmpeg-static\`（lib*.a + include\） | 只有随仓的 `ffmpeg-static\` 丢失或要升级 FFmpeg 版本时才需要；依赖 sh/sed(Git)/make/nasm（均经 `download-toolchain.ps1` 备齐） |

```powershell
.\build-zig.bat        # 绝大多数情况跑这个就行
```

成功标志：打印 `[build-zig] BUILD-OK`。每次构建都会先重新生成 `hook\inc\*.inc`（注入脚本有改动时），
并在结束时把 `bin\` 里的导入库/导出表/pdb 清掉——**bin\ 永远只剩 `NativeMediaBridge.dll` 和 `mb108_x64.dll` 两个文件**，
拷贝这两个文件即可分发运行。

## 项目结构

本 README 在项目根；源码已打平到根下的功能子目录（不再有 src\ 一层）：

```text
NativeMediaBridge-注解版\
├─ api\                                  【对外契约】宿主唯一可见的 C ABI 边界
│  ├─ native_media_bridge.h              对外 C ABI 接口声明（NMB_* 函数契约，宿主唯一需要包含的头）
│  ├─ native_media_bridge.cpp            导出层：仅 NMB_* 薄封装与 DllMain（原 §9）
│  └─ native_media_bridge.def            DLL 导出符号表
├─ kernel\                               【内核适配】miniblink 动态加载与函数指针绑定
│  └─ nmb_kernel.cpp                     mb108/mb132 动态加载、全部 mb* 函数指针绑定（initializeKernel）
├─ core\                                 【基础设施】跨模块共享头、核心数据结构、通用工具
│  ├─ nmb_internal.h                     内部共享头：公共头/类型别名/MbApi/跨模块全局与函数声明
│  ├─ nmb_types.h                        核心运行时结构：FfmpegAudio/FfmpegVideo/Media/Browser/RectI
│  └─ nmb_util.cpp                       全局对象、错误文本、路径/DLL 加载、UTF-8、字符串切分等工具
├─ media\                                【媒体管道】ffmpeg 音视频解码与媒体生命周期
│  ├─ nmb_audio.cpp                      ffmpeg 音频解码 + waveOut 8 缓冲播放线程 audioLoop + openAudio
│  ├─ nmb_video.cpp                      ffmpeg 视频解码线程 decodeLoop + 预读缓冲 + openVideo
│  └─ nmb_media.cpp                      媒体生命周期（release/silence/notify）与回传页面的状态 JSON
├─ page\                                 【页面交互】JS 注入与 mbQuery 协议
│  ├─ nmb_inject.cpp                     4 段注入脚本常量 + onReady/onScriptContext + 注入环境开关
│  └─ nmb_protocol.cpp                   mbQuery 协议 mediaBridge 与内核媒体下载拦截 mediaLoadUrlBegin
├─ render\                               【渲染合成】页面像素与原生视频帧离屏合成
│  └─ nmb_composite.cpp                  页面+原生视频帧离屏合成 paintBrowser（挖洞/软洞抠像）
├─ platform\                             【平台窗口】Win32 窗口、消息过程与输入转发
│  └─ nmb_window.cpp                     宿主窗口过程、输入转发、窗口类/工具窗、destroyBrowser
├─ utility\                              【工具与垫片】构建期/自检工具和编译兼容头（已加入 -I）
│  ├─ mm_malloc.h / stdbool.h / x86intrin.h   zig(mingw) 编译时给 FFmpeg 头补的兼容垫片
│  ├─ _nmb_gen_hooks_inc.py              代码生成器：hook/js 的 JS → hook/inc 的 C++ 分段字符串
│  └─ check_inject_syntax.py             node --check 四个注入 JS，并校验 inc 是否过期
├─ CMakeLists.txt                        CMake 备用构建入口（内部相对路径以项目根为基准；日常用 build-zig.bat）
├─ hook\
│  ├─ js\                                注入页面脚本的【真源】，改注入逻辑只改这里
│  │  ├─ injection.js                    主脚本：媒体收养、原生控件条、属性劫持、全屏 polyfill
│  │  ├─ media_api_shim.js               补 MediaError 等被内核砍掉的接口名
│  │  ├─ compat_shim.js                  chrome/ios/mse 能力伪装（NMB_COMPAT_SHIM 控制）
│  │  └─ crypto_shim.js                  SubtleCrypto 未实现分支的兜底垫片
│  └─ inc\                               生成器产出的 R"NMBHOOK(…)" 片段（勿手改，随仓提交）
├─ ffmpeg-static\                        静态链接的 FFmpeg 9：include\ 头 + lib*.a
├─ bin\                                  运行时目录：只放两个 dll（构建脚本会自动清掉 .lib/.exp/.pdb）
│  ├─ NativeMediaBridge.dll              本桥 DLL（构建产物）
│  └─ mb108_x64.dll                      miniblink 浏览器内核（运行时必需，由桥动态加载）
├─ tests\                                【测试】用例回归（Python + ctypes，不参与构建；与 smoke_test 区分）
│  ├─ test_bindwebview.py                媒体桥交互/自动回归主入口（--auto 跑脚本化用例）
│  ├─ test_bindwindow.py / test_mbpython.py   窗口绑定、MBPython 绑定用例
│  ├─ nmb_probe_common.py / config.py / callbackfunc.py   探针公共库、配置、JS 回调
│  ├─ check_kernel_exports.py            核对内核 DLL 是否提供桥所需的全部导出
│  ├─ probe_*.py / _probe_*.py / _make_bad_png.py          各类专项探针与造数小脚本
│  ├─ MBPython\                          随仓快照【旧版】Python 绑定包（仅测试用，见下文“测试依赖”）
│  ├─ testjs\                            测试页面与前端资产（media_suite.html、video.js、图片等）
│  └─ testexe\                           测试宿主相关的页面/图标素材
├─ smoke_test\                           【测试】查 bug 的冒烟/诊断资产（一次性工具，不参与构建）
│  ├─ MBPython\                          随仓快照【新版】Python 绑定包（仅测试用，见下文“测试依赖”）
│  └─ _nmb_find_zig.py / _nmb_libcheck.py /
│     _nmb_libfind.py / _nmb_env.bat /
│     _nmb_inject_size.py                zig/MSVC 坏库环境诊断、注入脚本字节与切分统计
├─ tools\
│  └─ png_iccp_check.py                  扫描/就地清理 PNG 的 iCCP 块（消 libpng 警告）
├─ download-toolchain.ps1                【工具链】把 zig/nasm/ffmpeg 源码/共享 SDK 下载到 .toolchain\
├─ build-zig.bat                         【首选构建】zig c++ 静态链接，输出 bin\
├─ build.ps1                             PowerShell 构建（本机无 zig 时自动下载 0.15.2）
├─ build-cl.bat                          MSVC 构建（需自备 ffmpeg-sdk shared 版 SDK）
└─ build-ffmpeg-static.bat               【一次性】自编译 ffmpeg-static 静态库
```

说明三类容易混淆的目录：

- `hook\js` 是**真源**，`hook\inc` 是**生成物**；改注入只改 js，构建脚本会自动重新生成 inc（详见 `utility\_nmb_gen_hooks_inc.py`）。
- `tests` 与 `smoke_test` 都是**纯测试资产**：构建脚本完全不引用它们，删掉也不影响 DLL 编译；它们只通过 ctypes 加载 `bin\NativeMediaBridge.dll` 做验证。其中各有一份随仓的 MBPython 绑定包快照（旧版在 `tests\MBPython`，新版在 `smoke_test\MBPython`），不是项目本体。
- `ffmpeg-static`（静态库，首选构建用）与备用的 `ffmpeg-sdk`（shared 版，体积大、默认未随本注解版拷贝）二选一。

## 验证（用例测试在 `tests\`，从项目根目录运行）

### 测试依赖怎么装

自动回归只需要 Python 3 + pywin32，全部通过 ctypes 直接加载 `bin\` 里的两个 dll，**不需要安装 MBPython**：

```powershell
pip install pywin32
```

`tests\MBPython\` 和 `smoke_test\MBPython\` 是 miniblink 的第三方 Python 绑定包
[MBPython](https://github.com/lochen88/MBPython)（作者 lochen，MIT）的**随仓快照**，
仅供 `test_mbpython.py`、`j.py` 等手动用例 import，与本桥的构建没有任何关系：

- 随仓快照已可直接用（脚本从自己所在目录解析 `MBPython` 包），无需下载；
- 快照丢失或要更新时，从上游取即可，二选一：
  ```powershell
  git clone https://github.com/lochen88/MBPython.git   # 取其中的 MBPython\ 目录放回 tests\ 或 smoke_test\
  # 或直接以 PEP517 方式安装到当前 Python 环境：
  pip install "git+https://github.com/lochen88/MBPython.git"
  ```
  注意它自身依赖 pywin32（`pip install pywin32`）。

另外，`python utility\check_inject_syntax.py` 做注入脚本语法自检时需要 node（`node --check`），
不跑该自检则不需要。

### 跑测试

`tests\` 与 `smoke_test\` 都是**只读型测试资产**：不修改源码、不参与构建、不被构建脚本引用，
删除这两个目录不影响 DLL 编译。默认是**全手动**模式：脚本只负责开窗并回读窗口标题，页面不会自动跳转、也不会自动播放。

```powershell
python tests\test_bindwebview.py
```

窗口打开后按脚本打印的清单逐项试：播放/拖动进度条、滚动页面、听声音是否连续、
多视频并发、点链接手动跳转、最大化与拉伸窗口。

需要脚本化回归时加 `--auto`（页面会带 `?auto=1`）：

```powershell
python tests\test_bindwebview.py --auto --duration 24
```

此时脚本自动跑窗口事件（最大化/还原/改尺寸），测试页 `tests\testjs\media_suite.html`
自检控件条数量、模拟拖动进度条、确认多路媒体时长与播放状态，并把结果回写成窗口标题，
脚本读窗口标题即可判定结果。

## 常见提示

`libpng warning: iCCP: known incorrect sRGB profile` 由内核 miniblink 里的 libpng 打印，触发条件是页面里的 PNG 带了一个内容与标准 sRGB 不一致的 iCCP 块（Photoshop 导出的图很常见）。空白页不会出现，加载这类 PNG 时才会出现，与本桥无关，也不影响功能。

要消掉它，把页面资源里的这类 PNG 清一下 iCCP 块即可（只删颜色配置元数据，像素数据不变）：

```powershell
python tools\png_iccp_check.py 目录          # 只扫描并列出
python tools\png_iccp_check.py 目录 --fix    # 就地删除 iCCP
```

## 限制

FFmpeg 负责音频和视频解码，音频输出依赖 Windows `waveOut`；网络媒体必须能被 FFmpeg 直接访问，页面登录态、Cookie 和防盗链不会自动复用。当前脚本覆盖常用调用，但旧版 miniblink 的原生媒体属性可能不可配置，因此 `currentTime`、`duration`、`paused` 等完整只读属性仍按目标网页补充。
