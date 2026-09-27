# hook/css —— 桥控件条的样式真源

改控件条外观只改本目录，不要再往 `hook/js/injection.js` 里塞样式字符串。

## 构建流程（三步都不能省）

```
改 hook/css/*.css
  -> python _nmb_gen_hooks_inc.py        # 把 CSS 拼进 hook/inc/injection.inc
  -> 编译（build-zig.bat / build-cl.bat）
```

`page/nmb_inject.cpp` include 的是 `hook/inc/*.inc`，跳过第二步编译出来的 DLL 里还是旧样式。

## 文件与顺序

| 文件 | 内容 |
| --- | --- |
| `base.css` | 容器、按钮、播放/暂停记号、时间、滑块、压制内核原生控件 |
| `tier.css` | 窄条分档（`data-nmb-narrow` / `data-nmb-tiny`）收掉音量等 |
| `audio.css` | 音频条（更紧凑、四角圆角） |

生成器按**显式顺序表**（`_nmb_gen_hooks_inc.py` 里的 `CSS_ORDER`：base→tier→audio，
即原 `NN-` 编号顺序）拼接——同名特异度靠表序定输赢，**不能按文件名字母序**
（字母序会把 audio 排到 base 前，音频条的尺寸规则会被 base 盖回去）。新增文件默认
按字母序接在最后；要插进中间就改 `CSS_ORDER`。
`.css` 之外的文件（含本 README）不参与拼接。

## JS 里保留的内联样式（有意为之）

生成器只注入本目录的 CSS。下面这些仍在 JS 里，原因写在括号内：

- 控件条的位置与尺寸：**不在 JS 里**。条挂在桥自建的包装盒（`data-nmb-wrap`，与元素盒子重合）
  里，位置/宽度由 `base.css` 的 `absolute + inset` 相对布局给出（全屏态由 `-fsbar` 切 fixed），
  JS 只打 `data-nmb-off/-fill/-narrow/-tiny/-fsbar` 状态属性，挖洞矩形照实读条的
  `getBoundingClientRect()` 上报。
- 全屏 polyfill 给元素铺满视口的那串 `cssText`（要叠加在站点原有 style 之上，退出时逐字还原）。
- 进度条轨道的三段渐变串（每帧变）：由 JS 的 `paintTrack` 写进一条自建的
  `::-webkit-slider-runnable-track` 规则（内核丢弃 setProperty 写的自定义属性，`--nmb-track` 这条路走不通）。
- `el.style.minHeight` / `opacity` 这类"钉住元素占位"的兜底（见 `pinBox` / `adopt`）。

## 改样式时的两个坑

1. 特异度：本目录选择器都带 `[data-nmb-controls]` 前缀。写新规则时保持同样的前缀与位置，
   否则可能被前面的规则盖住（同名特异度按拼接顺序决胜负）。
2. 显隐口径（用户拍板）：元素只要有**任何一部分**被视口或容器裁掉，整条就隐藏
   （`placeBar` 里的 `clip` 判定，四边含顶边）。所以别指望用动画/位移让条"部分露出来"。

## 生成器替你挡住的一件事：行尾

CSS 是**逐行**塞进 `var NMB_CSS=[...]` 的 JS 单引号字符串数组的，而 JS 字符串里不允许出现
裸 `\r`/`\n`。本目录的 css 是 CRLF 行尾，如果原样带进去，数组每个元素都会以 `\r` 收尾——
**DLL 编得过（C++ 只当它是 raw string 文本），但注入到内核里整段脚本语法错误、页面直接不加载**。
所以：

- 生成器（`utility/_nmb_gen_hooks_inc.py`）读 css 时统一把 `\r\n`/`\r` 归一成 `\n`，并对每行
  再做一次"有没有裸 `\r`"的检查，有就报错停下；
- 生成后还会用 `node --check` 验一遍**注入完成的整段 JS**（没装 node 则跳过并提示）。

也就是说：改完 css 直接跑生成 + 编译即可，行尾不用手工处理；万一哪天又出现"编得过但页面空白"，
先跑 `python _nmb_check_inc.py` 看注入的脚本是不是语法错。
