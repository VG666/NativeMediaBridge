# build.ps1 -- PowerShell build: find zig (PATH or project .toolchain\), otherwise
# download zig 0.15.2 from ziglang.org into .toolchain\, then compile the sources
# under api\ kernel\ core\ ... into bin\NativeMediaBridge.dll.
#
# FFmpeg is linked STATICALLY from ffmpeg-static\ (same as build-zig.bat), so the
# runtime only needs NativeMediaBridge.dll + the kernel dll.
#
# Keep this file ASCII-only: Windows PowerShell 5.1 decodes BOM-less scripts with
# the ANSI/OEM codepage (936 on zh-CN), so non-ASCII comments become mojibake.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = $root
$bin = Join-Path $src 'bin'
$fstatic = Join-Path $src 'ffmpeg-static'
if (Test-Path (Join-Path $src '.toolchain\ffmpeg-static\lib\libavcodec.a')) {
    $fstatic = Join-Path $src '.toolchain\ffmpeg-static'
}

# ---- sanity: FFmpeg static libs ------------------------------------------------
$ffHeader = Join-Path $fstatic 'include\libavcodec\avcodec.h'
$ffLib = Join-Path $fstatic 'lib\libavcodec.a'
if (-not (Test-Path $ffHeader) -or -not (Test-Path $ffLib)) {
    throw "FFmpeg static libs missing under $fstatic (restore ffmpeg-static\ from the repo, or run build-ffmpeg-static.bat)"
}

# ---- locate zig ---------------------------------------------------------------
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) {
    $zig = @('.toolchain\zig-x86_64-windows-0.15.2', '.toolchain-fixed\zig-x86_64-windows-0.15.2',
             '.toolchain-fresh\zig-x86_64-windows-0.15.2', 'zig-x86_64-windows-0.15.2') |
        ForEach-Object { Join-Path $root $_ } |
        ForEach-Object { Join-Path $_ 'zig.exe' } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
}

$temp = Join-Path $root '.toolchain'
$archive = Join-Path $root 'zig.zip'
if (-not $zig) {
    $url = 'https://ziglang.org/download/0.15.2/zig-x86_64-windows-0.15.2.zip'
    $sha256 = '3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c'
    try {
        Write-Host "[build] downloading zig 0.15.2 ..."
        Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
        if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLower() -ne $sha256) {
            throw 'Zig SHA-256 check failed.'
        }
        New-Item -ItemType Directory -Force $temp | Out-Null
        Expand-Archive -LiteralPath $archive -DestinationPath $temp -Force
        $zig = Join-Path $temp 'zig-x86_64-windows-0.15.2\zig.exe'
        if (-not (Test-Path $zig)) { throw "zig.exe not found after extraction under $temp" }
    } finally {
        Remove-Item $archive -Force -ErrorAction SilentlyContinue
    }
}
if (-not $zig) { throw 'zig.exe not found. Run .\download-toolchain.ps1 first.' }
Write-Host "[build] zig   : $zig"
Write-Host "[build] ffmpeg: $fstatic"

# ---- regenerate hook\inc\*.inc from hook\js\*.js ------------------------------
# The generator locates hook\ by its own file path, so cwd does not matter.
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if ($python) {
    & $python (Join-Path $src 'utility\_nmb_gen_hooks_inc.py')
    if ($LASTEXITCODE -ne 0) { throw 'generating hook\inc\*.inc failed' }
} else {
    Write-Warning 'python not found, keeping existing hook\inc\*.inc'
}

# ---- compile ------------------------------------------------------------------
New-Item -ItemType Directory -Force $bin | Out-Null
$dllArgs = @(
    'c++', '-target', 'x86_64-windows-gnu', '-std=c++17', '-O2',
    '-DNMB_EXPORTS', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
    ('-I' + $src),
    ('-I' + (Join-Path $src 'utility')),
    ('-I' + (Join-Path $fstatic 'include')), '-shared',
    (Join-Path $src 'api\native_media_bridge.cpp'),
    (Join-Path $src 'kernel\nmb_kernel.cpp'),
    (Join-Path $src 'core\nmb_util.cpp'),
    (Join-Path $src 'media\nmb_audio.cpp'),
    (Join-Path $src 'media\nmb_video.cpp'),
    (Join-Path $src 'media\nmb_media.cpp'),
    (Join-Path $src 'page\nmb_inject.cpp'),
    (Join-Path $src 'page\nmb_protocol.cpp'),
    (Join-Path $src 'render\nmb_composite.cpp'),
    (Join-Path $src 'platform\nmb_window.cpp'),
    (Join-Path $src 'api\native_media_bridge.def'),
    ('-L' + (Join-Path $fstatic 'lib')),
    '-lavformat', '-lavcodec', '-lswscale', '-lswresample', '-lavutil',
    '-lws2_32', '-lsecur32', '-lcrypt32', '-lncrypt', '-lbcrypt', '-lole32', '-lm',
    '-luser32', '-lgdi32', '-lwinmm', '-lkernel32', '-luuid',
    '-o', (Join-Path $bin 'NativeMediaBridge.dll')
)
& $zig $dllArgs
if ($LASTEXITCODE -ne 0) { throw 'NativeMediaBridge.dll build failed.' }

# Link artifacts (.exp/.lib/.pdb) are not needed: hosts use LoadLibrary/ctypes.
Get-ChildItem $bin -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.exp', '.lib', '.pdb' } |
    Remove-Item -Force

Write-Host '[build] BUILD-OK'
Write-Host ('DLL: ' + (Join-Path $bin 'NativeMediaBridge.dll'))
