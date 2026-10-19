@echo off
rem ============================================================================
rem build-test.bat -- side build: identical to build-zig.bat but writes
rem bin\NativeMediaBridge-test.dll. The production DLL is often locked by a
rem running host, so verify changes with this one first, then rebuild normally.
rem
rem IMPORTANT: keep this file PURE ASCII (see build-zig.bat header): cmd.exe reads
rem .bat bytes in the OEM codepage, UTF-8 Chinese comments break ^ continuations.
rem ============================================================================
setlocal
cd /d "%~dp0"
if not exist "bin" mkdir "bin"

set "FSTATIC=ffmpeg-static"
if exist ".toolchain\ffmpeg-static\lib\libavcodec.a" set "FSTATIC=.toolchain\ffmpeg-static"
if not exist "%FSTATIC%\include\libavcodec\avcodec.h" (
    echo [build-test] FFmpeg headers not found: %FSTATIC%\include\libavcodec\avcodec.h
    exit /b 1
)
if not exist "%FSTATIC%\lib\libavcodec.a" (
    echo [build-test] FFmpeg static libs not found: %FSTATIC%\lib\libavcodec.a
    exit /b 1
)

set "ZIG="
for %%D in ("zig-x86_64-windows-0.15.2" ".toolchain\zig-x86_64-windows-0.15.2" ".toolchain-fixed\zig-x86_64-windows-0.15.2" ".toolchain-fresh\zig-x86_64-windows-0.15.2") do (
    if not defined ZIG if exist "%%~D\zig.exe" set "ZIG=%%~D\zig.exe"
)
if not defined ZIG for /f "delims=" %%P in ('where zig 2^>nul') do if not defined ZIG set "ZIG=%%P"
if not defined ZIG (
    echo [build-test] zig.exe not found.
    echo [build-test] run: powershell -ExecutionPolicy Bypass -File .\download-toolchain.ps1
    exit /b 1
)
echo [build-test] zig   : %ZIG%
echo [build-test] ffmpeg: %FSTATIC%

where python >nul 2>nul && (
    python "utility\_nmb_gen_hooks_inc.py" || exit /b 1
) || echo [build-test] python not found, keeping existing hook\inc\*.inc

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
    echo [build-test] BUILD FAILED
    exit /b 1
)
del /q "bin\*.exp" "bin\*.lib" "bin\*.pdb" 2>nul
echo [build-test] BUILD-OK
dir /TW bin\NativeMediaBridge-test.dll | findstr /i NativeMediaBridge-test.dll
endlocal
