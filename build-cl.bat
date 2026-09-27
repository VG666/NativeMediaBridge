@echo off
rem build-cl.bat —— 用 MSVC 直接编译（不需要 CMake；CMake 的 VS 生成器在中文路径下会配置失败）。
rem 本脚本在项目根目录运行；源码按功能域分在 api\ kernel\ core\ ... 子目录，用 ffmpeg-sdk 里的 shared 版 FFmpeg SDK。
rem 产物：bin\NativeMediaBridge.dll + .pdb + 导入库 NativeMediaBridge.lib；中间目标文件在 build-cl\。
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [build-cl] 找不到 MSVC 环境（vcvars64.bat）
    exit /b 1
)
cd /d "%~dp0"
rem 注入脚本的真源是 hook\js\*.js：编译前重新生成 hook\inc\*.inc（没装 python 则沿用随仓 inc）。
where python >nul 2>nul && (
    python utility\_nmb_gen_hooks_inc.py || exit /b 1
) || echo [build-cl] 未找到 python，沿用现有 hook\inc\*.inc
if not exist build-cl mkdir build-cl
if not exist "bin" mkdir "bin"
cl /nologo /LD /MD /O2 /std:c++17 /EHsc /utf-8 /DNMB_EXPORTS /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I"." ^
   /I"utility" ^
   /I".toolchain\ffmpeg-sdk\ffmpeg-n9.0-latest-win64-lgpl-shared-9.0\include" ^
   /Fo:build-cl\ /Fd:bin\NativeMediaBridge.pdb ^
   api\native_media_bridge.cpp kernel\nmb_kernel.cpp core\nmb_util.cpp ^
   media\nmb_audio.cpp media\nmb_video.cpp media\nmb_media.cpp ^
   page\nmb_inject.cpp page\nmb_protocol.cpp render\nmb_composite.cpp platform\nmb_window.cpp ^
   api\native_media_bridge.def ^
   /link /LIBPATH:".toolchain\ffmpeg-sdk\ffmpeg-n9.0-latest-win64-lgpl-shared-9.0\lib" ^
   avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib user32.lib gdi32.lib winmm.lib ole32.lib uuid.lib ^
   /IMPLIB:bin\NativeMediaBridge.lib /OUT:bin\NativeMediaBridge.dll
if errorlevel 1 (
    echo [build-cl] 编译失败
    exit /b 1
)
rem bin\ 运行时只需要两个 dll：本桥产物 NativeMediaBridge.dll + 浏览器内核 mb108_x64.dll。
rem 导入库/导出表/pdb 都是链接中间物（宿主和测试都走 ctypes 动态加载，不需要它们），构建后清掉。
del /q "bin\*.exp" "bin\*.lib" "bin\*.pdb" 2>nul
echo [build-cl] BUILD-OK
dir /TW bin\NativeMediaBridge.dll | findstr /i NativeMediaBridge.dll
