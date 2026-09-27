#Requires -Version 5.1
<#
  download-toolchain.ps1 -- download the NativeMediaBridge build toolchain into .toolchain\ at the project root.
  .toolchain\ is git-ignored (see .gitignore); run this once on a fresh machine / CI. Build scripts read from .toolchain\.

  Components and where they land (relative to project root):
    zig 0.15.2          -> .toolchain\zig-x86_64-windows-0.15.2\zig.exe   (preferred compiler; required by build-zig / build-test)
    nasm 2.16.03        -> .toolchain\nasm-2.16.03\nasm.exe                (required by build-ffmpeg-static; found recursively)
    FFmpeg source n9.0  -> .toolchain\ffmpeg-src\FFmpeg-n9.0\configure ... (required by build-ffmpeg-static)
    FFmpeg shared SDK   -> .toolchain\ffmpeg-sdk\ffmpeg-n9.0-latest-win64-lgpl-shared-9.0\  (for build.ps1 / build-cl / CMake MSVC path)
    ffmpeg-static libs  -> .toolchain\ffmpeg-static\                         (built via -BuildFfmpeg; build scripts prefer this)

  Usage:
    powershell -ExecutionPolicy Bypass -File .\download-toolchain.ps1
        # default: download zig only (minimum for the preferred build; ffmpeg-static is already shipped at repo root)
    powershell -ExecutionPolicy Bypass -File .\download-toolchain.ps1 -All
        # download everything: zig + nasm + ffmpeg source + shared SDK
    powershell -ExecutionPolicy Bypass -File .\download-toolchain.ps1 -Nasm -FfmpegSource -BuildFfmpeg
        # fetch nasm + source, then run build-ffmpeg-static.bat to build .toolchain\ffmpeg-static
    powershell -ExecutionPolicy Bypass -File .\download-toolchain.ps1 -DryRun
    powershell -ExecutionPolicy Bypass -File .\download-toolchain.ps1 -Force

  Notes:
    - zig is SHA-256 verified; failure aborts. Other components' download failures only warn (zig is the hard dependency).
    - Shared SDK / source version URLs may drift upstream; override with -FfmpegSdkUrl / -FfmpegSrcUrl if the default 404s.
    - If the shared SDK is a .7z, Windows has no native extractor; the script will tell you to unzip it into .toolchain\ffmpeg-sdk (or use a .zip URL).
#>
[CmdletBinding()]
param(
    [switch]$Zig,
    [switch]$Nasm,
    [switch]$FfmpegSource,
    [switch]$FfmpegSdk,
    [switch]$BuildFfmpeg,
    [switch]$All,
    [switch]$DryRun,
    [switch]$Force,
    [string]$FfmpegSdkUrl = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip',
    [string]$FfmpegSrcUrl  = 'https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n9.0.zip'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

if (-not ($Zig -or $Nasm -or $FfmpegSource -or $FfmpegSdk -or $BuildFfmpeg -or $All)) { $Zig = $true }
if ($All) { $Zig = $true; $Nasm = $true; $FfmpegSource = $true; $FfmpegSdk = $true; $BuildFfmpeg = $true }

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$tc   = Join-Path $root '.toolchain'
New-Item -ItemType Directory -Force $tc | Out-Null

function Get-HashFile($p) { (Get-FileHash $p -Algorithm SHA256).Hash.ToLower() }

function Get-Component {
    param([string]$Name, [string]$Url, [string]$DestDir, [string]$ExpectExe, [string]$Hash = '')
    $present = $ExpectExe -and (Test-Path -LiteralPath $ExpectExe)
    if (-not $Force -and $present) {
        Write-Host ('[toolchain] ' + $Name + ' already present, skip (use -Force to redownload)') -ForegroundColor DarkGray
        return $true
    }
    if ($DryRun) {
        Write-Host ('[toolchain] (dry) ' + $Name + ' <- ' + $Url + '  -> ' + $DestDir) -ForegroundColor Yellow
        return $true
    }
    $zip = Join-Path $tc ($Name + '.zip')
    try {
        Write-Host ('[toolchain] downloading ' + $Name + ' ...') -ForegroundColor Cyan
        Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing -TimeoutSec 300
        if ($Hash -and (Get-HashFile $zip) -ne $Hash) { throw 'SHA-256 mismatch (download tampered or URL changed)' }
        Expand-Archive -LiteralPath $zip -DestinationPath $DestDir -Force
        Remove-Item $zip -Force
        if ($ExpectExe -and -not (Test-Path -LiteralPath $ExpectExe)) { throw ('extracted but missing ' + $ExpectExe) }
        Write-Host ('[toolchain] ' + $Name + ' OK -> ' + $DestDir) -ForegroundColor Green
        return $true
    } catch {
        if (Test-Path -LiteralPath $zip) { Remove-Item $zip -Force -ErrorAction SilentlyContinue }
        Write-Warning ($Name + ' download/extract failed: ' + $_.Exception.Message)
        return $false
    }
}

function Find-Exe($dir, $name) {
    if (-not (Test-Path -LiteralPath $dir)) { return '' }
    $f = Get-ChildItem $dir -Recurse -Filter $name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($f) { return $f.FullName } else { return '' }
}

$zigUrl  = 'https://ziglang.org/download/0.15.2/zig-x86_64-windows-0.15.2.zip'
$zigHash = '3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c'
$zigExe  = Join-Path $tc 'zig-x86_64-windows-0.15.2\zig.exe'
if ($Zig) {
    $ok = Get-Component 'zig' $zigUrl $tc $zigExe $zigHash
    if (-not $ok) { throw 'zig is the hard dependency of the preferred build; download failed, check network or retry with -Force' }
}

if ($Nasm) {
    Get-Component 'nasm' 'https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip' $tc ''
}

if ($FfmpegSource) {
    Get-Component 'ffmpeg-src' $FfmpegSrcUrl (Join-Path $tc 'ffmpeg-src') ''
}

if ($FfmpegSdk) {
    Get-Component 'ffmpeg-sdk' $FfmpegSdkUrl (Join-Path $tc 'ffmpeg-sdk') ''
}

if ($BuildFfmpeg) {
    $nasmExe = Find-Exe $tc 'nasm.exe'
    $srcDir  = ''
    $cfg = Get-ChildItem (Join-Path $tc 'ffmpeg-src') -Recurse -Filter 'configure' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cfg) { $srcDir = $cfg.DirectoryName }
    if (-not $nasmExe) { Write-Warning 'BuildFfmpeg skipped: nasm not found (run -Nasm first)' }
    elseif (-not $srcDir) { Write-Warning 'BuildFfmpeg skipped: ffmpeg source not found (run -FfmpegSource first)' }
    else {
        Write-Host ('[toolchain] running build-ffmpeg-static.bat (src=' + $srcDir + ')') -ForegroundColor Cyan
        if (-not $DryRun) {
            & (Join-Path $root 'build-ffmpeg-static.bat') $srcDir
            if ($LASTEXITCODE -ne 0) { Write-Warning 'build-ffmpeg-static.bat failed, see its output' }
        }
    }
}

Write-Host ''
Write-Host 'done. Build scripts read the toolchain from .toolchain\; the preferred build only needs zig (ffmpeg-static ships at repo root).' -ForegroundColor Yellow
