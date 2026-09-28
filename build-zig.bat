@echo off
rem ============================================================================
rem build-zig.bat —— 首选构建方式：用 zig c++（内置 mingw 工具链）直接编译本桥。
rem 本脚本在项目根目录运行；源码按功能域分在 api\ kernel\ core\ ... 子目录，产物 bin\NativeMediaBridge.dll。
rem 【注解版拷贝说明】为控制体积，随仓的 .toolchain-fixed 等 zig 工具链目录
rem   （约 760MB）没有随本注解版一起拷贝。在新机器上构建前，请自备 zig 0.15.2：
rem   要么把 .toolchain-fixed\zig-x86_64-windows-0.15.2\ 放到项目根下，
rem   要么把 zig.exe 加入 PATH（也可直接运行 build.ps1 让它自动下载到 .toolchain）。
rem 现在这台机器的 MSVC x64 CRT 库（MSVCRT.lib/vcruntime.lib/libvcruntime.lib/OLDNAMES.lib/msvcprt.lib）
rem 开头是 8 个 0 字节、已经是坏档，cl 链接必然失败，所以走 zig。
rem
rem ffmpeg 走「静态链接」：lib*.a 与头文件在 ffmpeg-static\（由 build-ffmpeg-static.bat 生成）。
rem 所以产物不再依赖 avcodec-63/avformat-63/avutil-61/swresample-7/swscale-10 五个 dll，
rem bin 运行时需要 NativeMediaBridge.dll + miniblink_x64.dll。
setlocal
cd /d "%~dp0"
if not exist "bin" mkdir "bin"

set "FSTATIC=ffmpeg-static"
if exist ".toolchain\ffmpeg-static\lib" set "FSTATIC=.toolchain\ffmpeg-static"

set "ZIG="
rem 工具链优先查项目根下（随仓方式 / build.ps1 自动下载的位置），再查相邻原项目的
rem （本注解版为控体积没随 zig 工具链，这台机器实际用的是它），最后查 PATH。
for %%D in (".toolchain\zig-x86_64-windows-0.15.2" ".toolchain-fixed" ".toolchain-fresh" ".toolchain") do (
    if not defined ZIG if exist "%%~D\zig-x86_64-windows-0.15.2\zig.exe" set "ZIG=%%~D\zig-x86_64-windows-0.15.2\zig.exe"
)
if not defined ZIG for /f "delims=" %%P in ('where zig 2^>nul') do if not defined ZIG set "ZIG=%%P"
if not defined ZIG (
    echo [build-zig] 找不到 zig.exe
    exit /b 1
)
echo [build-zig] 使用 %ZIG%

rem 注入脚本的真源是 hook\js\*.js：编译前重新生成 hook\inc\*.inc（没装 python 则沿用随仓 inc）。
where python >nul 2>nul && (
    python utility\_nmb_gen_hooks_inc.py || exit /b 1
) || echo [build-zig] 未找到 python，沿用现有 hook\inc\*.inc

"%ZIG%" c++ -target x86_64-windows-gnu -std=c++17 -O2 ^
    -DNMB_EXPORTS -DWIN32_LEAN_AND_MEAN -DNOMINMAX ^
    -I"." -I"utility" -I"%FSTATIC%\include" -shared ^
    api\native_media_bridge.cpp kernel\nmb_kernel.cpp core\nmb_util.cpp ^
    media\nmb_audio.cpp media\nmb_video.cpp media\nmb_media.cpp ^
    page\nmb_inject.cpp page\nmb_protocol.cpp render\nmb_composite.cpp platform\nmb_window.cpp ^
    api\native_media_bridge.def ^
    -L"%FSTATIC%\lib" ^
    -lavformat -lavcodec -lswscale -lswresample -lavutil ^
    -lws2_32 -lsecur32 -lcrypt32 -lncrypt -lbcrypt -lole32 -lm ^
    -luser32 -lgdi32 -lwinmm -lkernel32 -luuid ^
    -o bin\NativeMediaBridge.dll
if errorlevel 1 (
    echo [build-zig] 编译失败
    exit /b 1
)
rem bin\ 运行时需要本桥产物 NativeMediaBridge.dll + 浏览器内核 miniblink_x64.dll。
rem 导入库/导出表/pdb 都是链接中间物（宿主和测试都走 ctypes 动态加载，不需要它们），构建后清掉。
del /q "bin\*.exp" "bin\*.lib" "bin\*.pdb" 2>nul
echo [build-zig] BUILD-OK
dir /TW bin\NativeMediaBridge.dll | findstr /i NativeMediaBridge.dll
