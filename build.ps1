# build.ps1 —— PowerShell 版构建：先查 PATH/项目根 .toolchain 里的 zig，
# 没有就从 ziglang.org 下载 0.15.2 到根目录 .toolchain，再编译各功能子目录下的源码。
# 源码直接在项目根的 api\ kernel\ core\ ... 子目录，产物 bin\。需要联网；离线环境请把 zig 放 .toolchain-fixed 后用 build-zig.bat。
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = $root
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) {
    $zig = @('.toolchain-fixed', '.toolchain-fresh', '.toolchain') |
        ForEach-Object { Join-Path $root $_ } |
        Where-Object { Test-Path $_ } |
        ForEach-Object { Get-ChildItem $_ -Filter 'zig.exe' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName } |
        Where-Object { $_ } | Select-Object -First 1
}
$temp = Join-Path $root '.toolchain'
$archive = Join-Path $root 'zig.zip'
if (-not $zig) {
    $url = 'https://ziglang.org/download/0.15.2/zig-x86_64-windows-0.15.2.zip'
    $sha256 = '3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c'
    try {
        Invoke-WebRequest -Uri $url -OutFile $archive
        if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLower() -ne $sha256) {
            throw 'Zig SHA-256 check failed.'
        }
        Expand-Archive -LiteralPath $archive -DestinationPath $temp -Force
        $zig = (Get-ChildItem $temp -Filter zig.exe -Recurse | Select-Object -First 1).FullName
    } finally {
        Remove-Item $archive -Force -ErrorAction SilentlyContinue
    }
}
# 注入脚本真源是 hook\js\*.js：编译前重新生成 hook\inc\*.inc（找不到 python 就沿用随仓 inc）。
# 生成器按自身文件位置定位 hook 目录，在哪个 cwd 调用都行。
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if ($python) {
    & python (Join-Path $src 'utility\_nmb_gen_hooks_inc.py')
    if ($LASTEXITCODE -ne 0) { throw '生成 hook\inc\*.inc 失败' }
} else {
    Write-Warning '未找到 python，沿用现有 hook\inc\*.inc（改过 hook\js\*.js 时需手动跑生成器）'
}
try {
    New-Item -ItemType Directory -Force (Join-Path $src 'bin') | Out-Null
    $dllArgs = @(
        'c++', '-target', 'x86_64-windows-gnu', '-std=c++17', '-O2',
        '-DNMB_EXPORTS', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
        ('-I' + $src),
        ('-I' + (Join-Path $src 'utility')),
        ('-I' + (Join-Path $src '.toolchain\ffmpeg-sdk\ffmpeg-n9.0-latest-win64-lgpl-shared-9.0\include')), '-shared',
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
        ('-L' + (Join-Path $src '.toolchain\ffmpeg-sdk\ffmpeg-n9.0-latest-win64-lgpl-shared-9.0\lib')),
        '-lavcodec', '-lavformat', '-lavutil', '-lswscale', '-lswresample',
        '-luser32', '-lgdi32', '-lwinmm', '-lkernel32', '-lole32', '-luuid',
        '-o', (Join-Path $src 'bin\NativeMediaBridge.dll')
    )
    & $zig $dllArgs
    if ($LASTEXITCODE -ne 0) { throw 'NativeMediaBridge.dll build failed.' }
    # bin\ 运行时只保留两个 dll：本桥产物 + 浏览器内核 mb108_x64.dll；
    # 导入库/导出表/pdb 是链接中间物（使用方都走 ctypes），构建后清掉。
    Get-ChildItem (Join-Path $src 'bin') -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in '.exp', '.lib', '.pdb' } |
        Remove-Item -Force
    # 早期的纯 C 手动演示宿主 example.c/example.html 已移除，验证统一走 Python 用例（tests\）。
    Write-Host ('DLL: ' + (Join-Path $src 'bin\NativeMediaBridge.dll'))
} finally {
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
}
