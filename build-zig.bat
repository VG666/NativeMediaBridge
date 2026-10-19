@echo off
rem ============================================================================
rem build-zig.bat -- preferred build: compile the bridge with zig c++ (bundled
rem mingw toolchain). Run from the project root. Sources live in api\ kernel\
rem core\ ... subdirs; output bin\NativeMediaBridge.dll.
rem
rem IMPORTANT: keep this file PURE ASCII. cmd.exe reads .bat bytes in the OEM
rem codepage (936 on zh-CN). UTF-8 Chinese comments get mis-decoded, and the
rem stray bytes can swallow the line-continuation caret (^) or a quote, which
rem splits the compiler command line and breaks the build with nonsense errors
rem like "'STATIC' is not recognized". Chinese docs live in README.md instead.
rem
rem Toolchain: zig 0.15.2, looked up in .toolchain\zig-x86_64-windows-0.15.2\
rem (download-toolchain.ps1 / build.ps1 put it there), then .toolchain-fixed,
rem .toolchain-fresh, .toolchain, then PATH.
rem
rem FFmpeg is linked STATICALLY from ffmpeg-static\ (lib*.a + include\), so the
rem runtime only needs NativeMediaBridge.dll + miniblink_x64.dll.
rem ============================================================================
setlocal
cd /d "%~dp0"
if not exist "bin" mkdir "bin"

rem ---- FFmpeg static libs -----------------------------------------------------
set "FSTATIC=ffmpeg-static"
if exist ".toolchain\ffmpeg-static\lib\libavcodec.a" set "FSTATIC=.toolchain\ffmpeg-static"
if not exist "%FSTATIC%\include\libavcodec\avcodec.h" (
    echo [build-zig] FFmpeg headers not found: %FSTATIC%\include\libavcodec\avcodec.h
    echo [build-zig] restore ffmpeg-static\ from the repo, or run build-ffmpeg-static.bat
    exit /b 1
)
if not exist "%FSTATIC%\lib\libavcodec.a" (
    echo [build-zig] FFmpeg static libs not found: %FSTATIC%\lib\libavcodec.a
    echo [build-zig] restore ffmpeg-static\ from the repo, or run build-ffmpeg-static.bat
    exit /b 1
)

rem ---- locate zig -------------------------------------------------------------
set "ZIG="
for %%D in ("zig-x86_64-windows-0.15.2" ".toolchain\zig-x86_64-windows-0.15.2" ".toolchain-fixed\zig-x86_64-windows-0.15.2" ".toolchain-fresh\zig-x86_64-windows-0.15.2") do (
    if not defined ZIG if exist "%%~D\zig.exe" set "ZIG=%%~D\zig.exe"
)
if not defined ZIG for /f "delims=" %%P in ('where zig 2^>nul') do if not defined ZIG set "ZIG=%%P"
if not defined ZIG (
    echo [build-zig] zig.exe not found.
    echo [build-zig] run: powershell -ExecutionPolicy Bypass -File .\download-toolchain.ps1
    exit /b 1
)
echo [build-zig] zig   : %ZIG%
echo [build-zig] ffmpeg: %FSTATIC%

rem ---- regenerate hook\inc\*.inc from hook\js\*.js ----------------------------
rem The generator locates hook\ by its own file path, so cwd does not matter.
where python >nul 2>nul && (
    python "utility\_nmb_gen_hooks_inc.py" || exit /b 1
) || echo [build-zig] python not found, keeping existing hook\inc\*.inc

rem ---- compile ----------------------------------------------------------------
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
    echo [build-zig] BUILD FAILED
    exit /b 1
)

rem Link artifacts (.exp/.lib/.pdb) are not needed: hosts use LoadLibrary/ctypes.
rem Keep bin\NativeMediaBridge.dll + the kernel dll only.
del /q "bin\*.exp" "bin\*.lib" "bin\*.pdb" 2>nul
echo [build-zig] BUILD-OK
dir /TW bin\NativeMediaBridge.dll | findstr /i NativeMediaBridge.dll
endlocal
