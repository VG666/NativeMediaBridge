@echo off
rem ============================================================================
rem build-test.bat —— 旁路构建：与 build-zig.bat 完全同参数，仅产物名不同
rem   （bin\NativeMediaBridge-test.dll）。正式 DLL 常被运行中的宿主占用，
rem   验证改动时先编旁路名，用宿主 --dll 指定它跑测试，验证通过再覆盖安装。
rem zig 工具链位置说明见 build-zig.bat 头部注释。
setlocal
cd /d "%~dp0"
if not exist "bin" mkdir "bin"

set "FSTATIC=ffmpeg-static"
if exist ".toolchain\ffmpeg-static\lib" set "FSTATIC=.toolchain\ffmpeg-static"

set "ZIG="
rem 本工作区没随 zig 工具链（约 760MB 没拷），实际用相邻原项目的：见下第 4 个候选。
for %%D in (".toolchain\zig-x86_64-windows-0.15.2" ".toolchain-fixed" ".toolchain-fresh" ".toolchain") do (
    if not defined ZIG if exist "%%~D\zig-x86_64-windows-0.15.2\zig.exe" set "ZIG=%%~D\zig-x86_64-windows-0.15.2\zig.exe"
)
if not defined ZIG for /f "delims=" %%P in ('where zig 2^>nul') do if not defined ZIG set "ZIG=%%P"
if not defined ZIG (
    echo [build-test] 找不到 zig.exe
    exit /b 1
)
echo [build-test] 使用 %ZIG%

where python >nul 2>nul && (
    python _nmb_gen_hooks_inc.py || exit /b 1
) || echo [build-test] 未找到 python，沿用现有 hook\inc\*.inc

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
    -o bin\NativeMediaBridge-test.dll
if errorlevel 1 (
    echo [build-test] 编译失败
    exit /b 1
)
echo [build-test] BUILD-OK
dir /TW bin\NativeMediaBridge-test.dll | findstr /i NativeMediaBridge-test.dll
