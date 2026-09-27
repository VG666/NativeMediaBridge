#Requires -Version 5.1
<#
  安装 Caveman（"原始人"）skill 到 CodeBuddy 和 Trae CN 的用户级 skills 目录。
  只做复制，不动注册表、不改配置文件。卸载 = 删掉目标目录。

  用法：
    powershell -ExecutionPolicy Bypass -File .\install-caveman-skill.ps1 -List       # 看已装
    powershell -ExecutionPolicy Bypass -File .\install-caveman-skill.ps1 -DryRun     # 只看计划
    powershell -ExecutionPolicy Bypass -File .\install-caveman-skill.ps1             # 真装
    ... -Targets CodeBuddy      # 只装一边
    ... -Skill caveman,caveman-commit   # 只装这几个技能
    ... -Skill '*'                      # 原版 20+ 个技能全装
    ... -Level full                     # 默认档保持上游 full
    ... -Level ''                       # 完全不碰 SKILL.md
    ... -DropTags delete,yagni          # 剔除 ponytail 的这两个审查标签
    ... -Repo L2ncE/caveman-chinese     # 换中文单技能版
    ... -LocalZip D:\caveman.zip        # 网络不通时喂本地 zip
    ... -LocalDir C:\path\to\skill      # 从本地已有技能目录装（市场技能无 GitHub 源时用）

  默认：Repo=JuliusBrussee/caveman（原版） Skill=caveman（只要主技能）
        Level=wenyan-lite（半文言档；上游默认是 full）

  安装位置：
    CodeBuddy : %USERPROFILE%\.codebuddy\skills\<skill名>\
    Trae CN   : %USERPROFILE%\.trae-cn\skills\<skill名>\
  装完重启 IDE 生效。若 IDE 里没出现，说明真实路径不同，把路径发我改常量。

  仓库选择（2026-09-17 核实）：
    JuliusBrussee/caveman    原版，skills/ 下 20+ 个技能，含 wenyan 文言三档。
    bbylw/caveman-cn         **不是技能包**：只有 README.md / index.html / CNAME /
                             LICENSE，是个汉化文档站，装不了。老脚本默认值就是它。
    L2ncE/caveman-chinese    单技能包，SKILL.md 在仓库根，可用。
    Hayatelin/caveman-zh-CN  汉化技能包，7 个技能，可用。
  注意：中文输出不需要汉化版 —— 用 wenyan 档位就是中文（繁体文言）。
  另：上游 skills/<名>/ 与 plugins/caveman/skills/<名>/ 同名同内容共两份，
      脚本自动去重并优先取独立的 skills/<名>/（其 SKILL.md 不引用外部文件，自足）。
  -DropTags 只对带 ## Tags 段的技能生效（如 ponytail-review / ponytail-audit）；
      主技能没有标签表会被自动跳过。它禁的是**标签**，不是 ladder 第 1 级的 YAGNI 理念。
#>
[CmdletBinding()]
param(
    [string]$Repo   = 'JuliusBrussee/caveman',
    [string]$Branch = 'main',
    [ValidateSet('All', 'CodeBuddy', 'Trae')][string]$Targets = 'All',
    [string[]]$Skill = @('caveman'),
    [string]$Level   = 'wenyan-lite',
    [string[]]$DropTags = @(),
    [string]$LocalZip = '',
    [string]$LocalDir = '',
    [switch]$DryRun,
    [switch]$Force,
    [switch]$List
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$KnownLevels = @('lite', 'full', 'ultra', 'wenyan-lite', 'wenyan-full', 'wenyan-ultra')

$AllTargets = [ordered]@{
    CodeBuddy = Join-Path $env:USERPROFILE '.codebuddy\skills'
    Trae      = Join-Path $env:USERPROFILE '.trae-cn\skills'
}
if ($Targets -eq 'All') { $TargetMap = $AllTargets }
else { $TargetMap = [ordered]@{}; $TargetMap[$Targets] = $AllTargets[$Targets] }

# 读 SKILL.md 里 "Default: **xxx**" 那一行，拿当前默认档
function Get-DefaultLevel {
    param([string]$Dir)
    $f = Join-Path $Dir 'SKILL.md'
    if (-not (Test-Path -LiteralPath $f)) { return '' }
    $m = [regex]::Match([System.IO.File]::ReadAllText($f), 'Default:\s*\*\*([\w\-]+)\*\*')
    if ($m.Success) { return $m.Groups[1].Value }
    return '?'
}

# 改写副本的默认档位；改完回读校验，绝不静默成功
function Set-DefaultLevel {
    param([string]$Dir, [string]$Lv, [switch]$WhatIfOnly)
    $f = Join-Path $Dir 'SKILL.md'
    if (-not (Test-Path -LiteralPath $f)) { return 'skip: no SKILL.md' }

    $text = [System.IO.File]::ReadAllText($f)
    $m = [regex]::Match($text, 'Default:\s*\*\*[\w\-]+\*\*')
    if (-not $m.Success) {
        # 分辨两种情况：上游改了措辞（该提醒手动设），还是这技能压根没有档位概念
        # —— ponytail-review / ponytail-audit 就是纯标签清单，不参与档位切换。
        if ($text -match '(?i)intensity level') {
            return ('skip: upstream wording changed, set manually with /caveman ' + $Lv)
        }
        return 'skip: no level concept in this skill'
    }
    if ($m.Groups[0].Value -match ('\*\*' + [regex]::Escape($Lv) + '\*\*')) { return ('already ' + $Lv) }

    $was = $m.Groups[0].Value
    if ($WhatIfOnly) { return ('would set ' + $Lv + ' (was "' + $was + '")') }

    $new = $text.Substring(0, $m.Index) + ('Default: **' + $Lv + '**') + $text.Substring($m.Index + $m.Length)
    # 无 BOM 写回，避免给 markdown 塞 BOM
    [System.IO.File]::WriteAllText($f, $new, (New-Object System.Text.UTF8Encoding($false)))

    if ([System.IO.File]::ReadAllText($f) -match ('Default:\s*\*\*' + [regex]::Escape($Lv) + '\*\*')) {
        return ('set to ' + $Lv + '  (was "' + $was + '")')
    }
    return 'FAILED: write did not verify'
}

# 从副本里剔除指定的审查标签（如 ponytail 的 delete / yagni），并注入一条硬禁令。
# 只对带 "## Tags" 段的技能生效；主技能没标签表就跳过，不会被污染。
function Remove-Tags {
    param([string]$Dir, [string[]]$Tags, [switch]$WhatIfOnly)
    $f = Join-Path $Dir 'SKILL.md'
    if (-not (Test-Path -LiteralPath $f)) { return 'skip: no SKILL.md' }

    $text  = [System.IO.File]::ReadAllText($f)
    $lines = @($text -split "`r?`n")
    $alt   = (($Tags | ForEach-Object { [regex]::Escape($_) }) -join '|')
    $pat   = '^\s*[-*]\s*`?(' + $alt + ')`?:'
    $kept    = @($lines | Where-Object { $_ -notmatch $pat })
    $removed = $lines.Count - $kept.Count

    if ($removed -eq 0 -and $text -notmatch '(?m)^##\s*Tags') {
        return 'skip: no tag list in this skill'
    }

    $ban = "`n## Local override (install script)`n`nNever emit these tags, not even as a note: " +
           (($Tags | ForEach-Object { '`' + $_ + ':`' }) -join ', ') + ".`n" +
           "If you spot that category, stay silent about it and never count it in the `net:` line.`n"

    if ($WhatIfOnly) {
        return ('would drop ' + $removed + ' tag line(s) and ban ' + ($Tags -join ', '))
    }

    [System.IO.File]::WriteAllText($f, (($kept -join "`n") + $ban), (New-Object System.Text.UTF8Encoding($false)))

    if ([System.IO.File]::ReadAllText($f) -match 'Local override') {
        return ('dropped ' + $removed + ' tag line(s); banned ' + ($Tags -join ', '))
    }
    return 'FAILED: write did not verify'
}

function Show-Installed {
    foreach ($kv in $TargetMap.GetEnumerator()) {
        Write-Host ("== {0} : {1}" -f $kv.Key, $kv.Value) -ForegroundColor Cyan
        if (-not (Test-Path -LiteralPath $kv.Value)) { Write-Host "   (folder not created yet)"; continue }
        $items = @(Get-ChildItem -LiteralPath $kv.Value -Directory -ErrorAction SilentlyContinue)
        if ($items.Count -eq 0) { Write-Host "   (empty)" }
        else {
            foreach ($it in $items) {
                $lv = Get-DefaultLevel $it.FullName
                if ($lv) { Write-Host ("   - {0}   [default level: {1}]" -f $it.Name, $lv) }
                else { Write-Host ("   - " + $it.Name) }
            }
        }
    }
}

function Get-RepoZip {
    param([string]$R, [string]$B, [string]$Out)
    foreach ($u in @("https://codeload.github.com/$R/zip/refs/heads/$B",
                     "https://github.com/$R/archive/refs/heads/$B.zip")) {
        try {
            Write-Host "  downloading $u"
            Invoke-WebRequest -Uri $u -OutFile $Out -UseBasicParsing -TimeoutSec 90
            if ((Test-Path -LiteralPath $Out) -and (Get-Item -LiteralPath $Out).Length -gt 0) { return $true }
        } catch { Write-Warning ("  failed: " + $_.Exception.Message) }
    }
    return $false
}

if ($List) { Write-Host "Installed skills:" -ForegroundColor Yellow; Show-Installed; return }

if ($Level -and ($KnownLevels -notcontains $Level)) {
    Write-Warning ("'{0}' not in known levels ({1}) - upstream may have renamed it, will still try." -f $Level, ($KnownLevels -join ', '))
}

Write-Host "== caveman skill installer ==" -ForegroundColor Yellow
Write-Host ("repo    : {0} ({1})" -f $Repo, $Branch)
Write-Host ("targets : " + (($TargetMap.Keys) -join ', '))
Write-Host ("skills  : " + ($Skill -join ', '))
Write-Host ("level   : " + $(if ($Level) { $Level } else { '(leave upstream default)' }))
Write-Host ("droptags: " + $(if ($DropTags) { $DropTags -join ', ' } else { '(none)' }))
Write-Host ("mode    : " + $(if ($DryRun) { 'DRY-RUN, nothing written' } else { 'REAL INSTALL' }))

$tmp = Join-Path $env:TEMP ('caveman-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null

try {
    # 本地目录源：整目录当解压根，跳过下载；技能名取目录名（市场技能没有 GitHub 源时用它）
    $rootName = 'caveman'
    if ($LocalDir) {
        if (-not (Test-Path -LiteralPath $LocalDir)) { throw "LocalDir not found: $LocalDir" }
        $src      = (Get-Item -LiteralPath $LocalDir).FullName
        $rootDir  = $src
        $rootName = (Get-Item -LiteralPath $LocalDir).Name
        Write-Host ("using local dir: " + $src)
    } else {
        $zip = $LocalZip
        if ($zip) {
            if (-not (Test-Path -LiteralPath $zip)) { throw "LocalZip not found: $zip" }
            Write-Host "using local zip: $zip"
        } else {
            $zip = Join-Path $tmp 'pkg.zip'
            $ok  = Get-RepoZip $Repo $Branch $zip
            if (-not $ok -and $Branch -eq 'main') { Write-Host "  retry with master"; $ok = Get-RepoZip $Repo 'master' $zip }
            if (-not $ok) { throw "download failed. GitHub unreachable? use -LocalZip <path>." }
        }

        $src = Join-Path $tmp 'src'
        Expand-Archive -LiteralPath $zip -DestinationPath $src -Force
        $rootDir = (Get-ChildItem -LiteralPath $src -Directory | Select-Object -First 1).FullName
    }

    $found = @(Get-ChildItem -LiteralPath $src -Recurse -File -Filter 'SKILL.md' -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -notmatch '[\\/]\.git[\\/]' } |
               ForEach-Object { $_.Directory } | Sort-Object FullName -Unique)

    if ($found.Count -eq 0) {
        Write-Host "package layout:" -ForegroundColor Yellow
        Get-ChildItem -LiteralPath $src -Recurse -Depth 3 | Select-Object -ExpandProperty FullName | ForEach-Object { Write-Host "   $_" }
        throw ("no SKILL.md found in '$Repo' - that repo is not an installable skill package " +
               "(bbylw/caveman-cn is a docs site: README/index.html/CNAME only). Use -Repo JuliusBrussee/caveman.")
    }

    # 根目录下的 SKILL.md 用 'caveman' 作名字；其余用目录名。
    # -contains 是精确匹配，所以 caveman 不会误吃到 caveman-commit。
    $all = foreach ($d in $found) {
        [pscustomobject]@{
            Name = if ($d.FullName -eq $rootDir) { $rootName } else { $d.Name }
            Dir  = $d.FullName
        }
    }

    # 仓库常给多个平台各打包一份同名技能（caveman: skills/ 与 plugins/caveman/skills/；
    # ponytail: skills/ 与 .openclaw/skills/）。不去重就会复制两遍，或挑到平台裁剪版
    # —— ponytail 两份并不等价：skills/ 6637 字节、.openclaw/ 只有 5957 字节。
    # 同名只留一个，优先平台中立的顶层 skills/<名>/，其次排除 plugins，最后排除隐藏目录。
    $all = @($all | Group-Object Name | ForEach-Object {
        $dup = $_.Group
        $top = @($dup | Where-Object { $_.Dir -match '[\\/]skills[\\/][^\\/]+$' -and $_.Dir -notmatch '[\\/]\.[^\\/]+[\\/]' })
        $mid = @($dup | Where-Object { $_.Dir -notmatch '[\\/]plugins[\\/]' -and $_.Dir -notmatch '[\\/]\.[^\\/]+[\\/]' })
        $low = @($dup | Where-Object { $_.Dir -notmatch '[\\/]plugins[\\/]' })
        if ($top.Count) { $top[0] } elseif ($mid.Count) { $mid[0] } elseif ($low.Count) { $low[0] } else { $dup[0] }
    } | Sort-Object Name)

    $wantAll = ($Skill -contains '*')
    $picked  = @($all | Where-Object { $wantAll -or ($Skill -contains $_.Name) })

    if ($picked.Count -eq 0) {
        Write-Host ("available in this repo: " + (($all | ForEach-Object { $_.Name }) -join ', ')) -ForegroundColor Yellow
        throw ("none of the requested skills exist: " + ($Skill -join ', '))
    }

    Write-Host ("repo has {0} skill(s); installing {1}: {2}" -f $all.Count, $picked.Count, (($picked | ForEach-Object { $_.Name }) -join ', ')) -ForegroundColor Green

    foreach ($s in $picked) {
        foreach ($kv in $TargetMap.GetEnumerator()) {
            $dest = Join-Path $kv.Value $s.Name
            if (Test-Path -LiteralPath $dest) {
                if (-not $Force) { Write-Host ("  skip (exists): " + $dest); continue }
                if ($DryRun) { Write-Host ("  would replace: " + $dest) }
                else { Remove-Item -LiteralPath $dest -Recurse -Force; Write-Host ("  replaced: " + $dest) }
            }
            if ($DryRun) {
                Write-Host ("  would copy: {0} -> {1}" -f $s.Dir, $dest)
                if ($Level)    { Write-Host ("      " + (Set-DefaultLevel $s.Dir $Level -WhatIfOnly)) }
                if ($DropTags)  { Write-Host ("      " + (Remove-Tags $s.Dir $DropTags -WhatIfOnly)) }
            }
            else {
                New-Item -ItemType Directory -Path $kv.Value -Force | Out-Null
                Copy-Item -LiteralPath $s.Dir -Destination $dest -Recurse -Force
                Write-Host ("  installed: " + $dest)
                if ($Level)    { Write-Host ("      level -> " + (Set-DefaultLevel $dest $Level)) }
                if ($DropTags) { Write-Host ("      tags ->  " + (Remove-Tags $dest $DropTags)) }
            }
        }
    }
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "done. restart the IDE and check the skills list." -ForegroundColor Yellow
