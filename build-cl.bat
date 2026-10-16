@echo off
rem build-cl.bat -- compile with MSVC directly (no CMake; the VS generator fails on non-ASCII paths).
rem Run from the project root. Sources are grouped under api\ kernel\ core\ ... subdirs.
rem Output: bin\NativeMediaBridge.dll + .pdb + import lib NativeMediaBridge.lib; objects in build-cl\.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [build-cl] MSVC environment not found (vcvars64.bat)
    exit /b 1
)
cd /d "%~dp0"
rem This script needs the shared FFmpeg SDK (avcodec.lib / avformat.lib ... MSVC
rem COFF import libs). The repo ships ffmpeg-static\ instead, whose lib*.a are GNU
rem ar archives that MSVC cannot link, so bail out with a clear reason instead of
rem spraying C1083 lines that hide the real cause.
rem NOTE: keep echo text pure ASCII -- cmd.exe reads .bat bytes in the OEM codepage
rem (936 here), so non-ASCII text can swallow following characters and break parsing.
set "FFSDK=.toolchain\ffmpeg-sdk\ffmpeg-n9.0-latest-win64-lgpl-shared-9.0"
if not exist "%FFSDK%\include\libavcodec\avcodec.h" (
    echo [build-cl] shared FFmpeg SDK not found: %FFSDK%
    echo [build-cl] the repo only has ffmpeg-static\lib\*.a (MinGW static libs, MSVC cannot link them)
    echo [build-cl] use build-zig.bat instead
    exit /b 1
)
rem Injected scripts live in hook\js\*.js: regenerate hook\inc\*.inc before compiling
where python >nul 2>nul && (
    python utility\_nmb_gen_hooks_inc.py || exit /b 1
) || echo [build-cl] python not found, keeping existing hook\inc\*.inc
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
    echo [build-cl] compile failed
    exit /b 1
)
rem At runtime bin\ only needs two dlls: NativeMediaBridge.dll + the kernel mb108_x64.dll.
rem Import lib / export table / pdb are link artifacts (hosts load the dll dynamically),
rem so they are deleted after the build.
del /q "bin\*.exp" "bin\*.lib" "bin\*.pdb" 2>nul
echo [build-cl] BUILD-OK
dir /TW bin\NativeMediaBridge.dll | findstr /i NativeMediaBridge.dll
