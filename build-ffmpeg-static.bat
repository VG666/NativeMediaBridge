@echo off
rem ============================================================================
rem 一次性构建「静态 ffmpeg 库」供 build-zig.bat 静态链接用（LGPL，只留解码/解复用
rem 实际用得到的组件）。本脚本在项目根目录运行，产物拷到 ffmpeg-static\（lib*.a + include\）。
rem
rem 为什么需要它：NativeMediaBridge.dll 的 PE 导入表原本写死 avcodec-63 / avformat-63 /
rem avutil-61 / swresample-7 / swscale-10 五个 dll，bin 目录不可能只留一个文件。
rem 把 ffmpeg 静态链进去之后，bin 里只需要「NativeMediaBridge.dll + mb132_x64.dll」。
rem
rem 依赖（本机已有，路径不同就改下面的变量）：
rem   zig    下载到 .toolchain\zig-x86_64-windows-0.15.2\zig.exe（download-toolchain.ps1）
rem   sh     Git for Windows 自带的 usr\bin\sh.exe（config.h 生成脚本要用）
rem   sed    同上，usr\bin\sed.exe（给 configure 打补丁，见下面「大坑 1」）
rem   make   GNU make（mingw32-make.exe）
rem   nasm   x86 SIMD 汇编器，必须 2.13 以上（老版本编不了 ffmpeg 的 x86inc.asm）
rem   源码   ffmpeg 源码树，默认 .toolchain\ffmpeg-src（download-toolchain.ps1 -FfmpegSource；或第一个参数覆盖）
rem
rem 两个已经踩过、必须保留的坑：
rem   [大坑 1] ffmpeg 的 configure 在 --disable-debug 时会给 mingw 目标加
rem            -Wl,--pic-executable。zig 用的 lld 不认这个参数，于是 configure 里
rem            所有「链接测试」全部失败，连 _aligned_malloc 都测不出来，最后报
rem            "Building with assembly enabled is not supported on platforms without
rem             aligned memory allocations!" 直接中断。所以下面先用 sed 把它换成
rem            lld 认识的 -Wl,--dynamicbase（我们只出静态库，不需要可执行文件入口）。
rem   [大坑 2] 多个 zig cc 并发编译同一个全局缓存会随机炸 "error: Unexpected"
rem            （8/12 并发时几秒钟内必挂，单文件手编却没事）。必须给本次构建一个
rem            独立的 ZIG_GLOBAL_CACHE_DIR，并把并发压到 4。全量约 2 分钟。
rem
rem 用法：build-ffmpeg-static.bat [源码目录]
rem ============================================================================
setlocal
set "ROOT=%~dp0"
set "TC=%ROOT%.toolchain"
rem ffmpeg 源码：优先 .toolchain\ffmpeg-src 下递归找 configure；否则用第一个参数；再回退旧默认
if not "%~1"=="" (set "SRC=%~1") else (
    for /f "delims=" %%C in ('dir /b /s /a:-d "%TC%\configure" 2^>nul') do set "SRC=%%~dpC"
)
if not defined SRC set "SRC=F:\ffbuild\FFmpeg-n9.0"
rem 产物装到 .toolchain\ffmpeg-static（构建脚本优先读这里；根目录 ffmpeg-static 为随仓兜底）
set "PREFIX=%TC:\=/%ffmpeg-prefix"
set "OUT=%TC%ffmpeg-static"
rem nasm / make：优先 .toolchain 下递归找，其次 PATH，再回退旧机器路径
for /f "delims=" %%N in ('dir /b /s /a:-d "%TC%\nasm.exe" 2^>nul') do set "NASM=%%N"
if not defined NASM set "NASM=F:\ffbuild\tools\nasm-2.16.03\nasm.exe"
for /f "delims=" %%M in ('dir /b /s /a:-d "%TC%\mingw32-make.exe" 2^>nul') do set "MAKE=%%M"
if not defined MAKE set "MAKE=F:\编程\Dev-Cpp\MinGW64\bin\mingw32-make.exe"
if not defined SH  set "SH=C:\Program Files\Git\usr\bin\sh.exe"
if not defined SED set "SED=C:\Program Files\Git\usr\bin\sed.exe"
rem 并发别调大：见大坑 2，4 是本机验证过不会炸的值
set "JOB=4"

set "ZIG="
if not defined ZIG if exist "%ROOT%.toolchain\zig-x86_64-windows-0.15.2\zig.exe" set "ZIG=%ROOT%.toolchain\zig-x86_64-windows-0.15.2\zig.exe"
if not defined ZIG if exist "%ROOT%.toolchain-fixed\zig-x86_64-windows-0.15.2\zig.exe" set "ZIG=%ROOT%.toolchain-fixed\zig-x86_64-windows-0.15.2\zig.exe"
if not defined ZIG if exist "%ROOT%.toolchain-fresh\zig-x86_64-windows-0.15.2\zig.exe" set "ZIG=%ROOT%.toolchain-fresh\zig-x86_64-windows-0.15.2\zig.exe"

for %%F in ("%SRC%\configure" "%ZIG%" "%SH%" "%SED%" "%MAKE%" "%NASM%") do if not exist %%F (
    echo [ffmpeg-static] 缺文件: %%F
    exit /b 1
)

set "PATH=C:\Program Files\Git\usr\bin;%PATH%"
set "MSYSTEM=MINGW64"
rem 给 zig 一个独立缓存目录（大坑 2）
set "ZIG_GLOBAL_CACHE_DIR=%SRC%\..\zigcache"
cd /d "%SRC%"

echo [ffmpeg-static] 给 configure 打 lld 兼容补丁（大坑 1）...
"%SED%" -i "s/-Wl,--pic-executable,-e,_mainCRTStartup/-Wl,--dynamicbase/; s/-Wl,--pic-executable,-e,mainCRTStartup/-Wl,--dynamicbase/" configure
if errorlevel 1 ( echo [ffmpeg-static] sed 打补丁失败 & exit /b 1 )

echo [ffmpeg-static] configure ...
"%SH%" ./configure --prefix=%PREFIX% ^
    --target-os=mingw32 --arch=x86_64 --enable-cross-compile ^
    "--cc=%ZIG% cc -target x86_64-windows-gnu" ^
    "--cxx=%ZIG% c++ -target x86_64-windows-gnu" ^
    "--host-cc=%ZIG% cc" ^
    "--ar=%ZIG% ar" "--ranlib=%ZIG% ranlib" "--nm=%ZIG% nm" "--strip=%ZIG% strip" ^
    "--x86asmexe=%NASM%" ^
    --enable-static --disable-shared --disable-programs --disable-doc --disable-debug ^
    --disable-avdevice --disable-avfilter --disable-hwaccels ^
    --disable-autodetect --disable-everything ^
    --enable-network --enable-schannel ^
    --enable-protocol=file,pipe,http,https,tcp,tls,crypto,data ^
    --enable-demuxer=mov,matroska,mpegts,mpegtsraw,flv,avi,asf,mp3,aac,ogg,wav,flac,hls ^
    --enable-decoder=h264,hevc,mpeg4,mpeg2video,mpeg1video,vp8,vp9,av1,aac,mp3,mp2,ac3,eac3,flac,vorbis,opus,wmav2,wmapro,pcm_s16le,pcm_s16be,pcm_s24le,pcm_u8,pcm_f32le,pcm_alaw,pcm_mulaw,mjpeg,gif,bmp ^
    --enable-parser=h264,hevc,mpeg4video,mpegvideo,aac,aac_latm,mp3,opus,vorbis,flac,av1,vp9,ac3 ^
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata,vp9_superframe ^
    --enable-swscale --enable-swresample --enable-x86asm
if errorlevel 1 ( echo [ffmpeg-static] configure 失败 & exit /b 1 )

echo [ffmpeg-static] make -j%JOB% ...
"%MAKE%" -j%JOB%
if errorlevel 1 ( echo [ffmpeg-static] 编译失败 & exit /b 1 )

echo [ffmpeg-static] make install 到 %PREFIX% ...
"%MAKE%" install
if errorlevel 1 ( echo [ffmpeg-static] install 失败 & exit /b 1 )

echo [ffmpeg-static] 拷贝产物到 %OUT% ...
if exist "%OUT%\include" rd /s /q "%OUT%\include"
if not exist "%OUT%\lib" mkdir "%OUT%\lib"
xcopy /e /i /y /q "%PREFIX%\include" "%OUT%\include" >nul || exit /b 1
for %%L in (avcodec avformat avutil swscale swresample) do (
    copy /y "%PREFIX%\lib\lib%%L.a" "%OUT%\lib\" >nul || exit /b 1
)
if exist "%PREFIX%\lib\pkgconfig" xcopy /e /i /y /q "%PREFIX%\lib\pkgconfig" "%OUT%\lib\pkgconfig" >nul

echo [ffmpeg-static] BUILD-OK
dir /b "%OUT%\lib"
