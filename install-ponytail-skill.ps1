#Requires -Version 5.1
<#
  安装 Ponytail（马尾辫）到 CodeBuddy / Trae CN 的用户级 skills 目录。

  本脚本只做编排，实际安装全部交给同目录的 install-caveman-skill.ps1
  （那个脚本虽叫 caveman，实为通用安装器，换 -Repo 可装任意技能仓库）。

  默认装三个技能：
    ponytail         主模式。3 档：lite / full（默认）/ ultra，无文言档
    ponytail-review  审 diff 里的过度设计
    ponytail-audit   同款标签，但扫全仓库

  默认剔除 delete: 与 yagni: 两个标签。理由：本机是 C++/Python 项目，
  死代码的静态判定容易被动态调用误伤；"只有一个实现的抽象"多为刻意分层。
  剔除后 review / audit 只会用 stdlib: / native: / shrink: 三类报事。
  注意：剔除的是标签，不是主技能 ladder 第 1 级的 YAGNI 质疑，那个照旧。

  用法：
    powershell -ExecutionPolicy Bypass -File .\install-ponytail-skill.ps1 -DryRun
    powershell -ExecutionPolicy Bypass -File .\install-ponytail-skill.ps1
    ... -Targets CodeBuddy     # 只装一边
    ... -OnlyMain              # 只装主技能，不要 review / audit
    ... -KeepTags              # 保留 delete / yagni 两个标签
    ... -Level ultra           # 默认档改成 ultra（ponytail 默认 full）
    ... -Force                 # 覆盖已存在的

  安装位置：
    CodeBuddy : %USERPROFILE%\.codebuddy\skills\<技能名>\
    Trae CN   : %USERPROFILE%\.trae-cn\skills\<技能名>\
#>
[CmdletBinding()]
param(
    [ValidateSet('All', 'CodeBuddy', 'Trae')][string]$Targets = 'All',
    [string]$Level = 'full',
    [switch]$OnlyMain,
    [switch]$KeepTags,
    [switch]$DryRun,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$core = Join-Path $PSScriptRoot 'install-caveman-skill.ps1'
if (-not (Test-Path -LiteralPath $core)) { throw "core installer not found: $core" }

$skills = if ($OnlyMain) { @('ponytail') } else { @('ponytail', 'ponytail-review', 'ponytail-audit') }

# 哈希表 splatting 是必需的：数组 splatting 会退化成位置参数，
# 把 "-Skill" 当成值喂给 -Targets，撞上 ValidateSet 直接报错。
$args_ = @{
    Repo    = 'DietrichGebert/ponytail'
    Skill   = $skills
    Level   = $Level
    Targets = $Targets
}
if (-not $KeepTags) { $args_['DropTags'] = @('delete', 'yagni') }
if ($DryRun)        { $args_['DryRun']   = $true }
if ($Force)         { $args_['Force']    = $true }

& $core @args_
