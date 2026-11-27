# test\suite 的运行入口。自动用仓库 .venv 的 python，跑完打印产物目录。
#
#   .\test\suite\run.ps1                 # 全套
#   .\test\suite\run.ps1 -m mpv          # 只跑 mpv 层
#   .\test\suite\run.ps1 -Case test_30   # 只跑文件名匹配的用例
#   .\test\suite\run.ps1 -Extra "-x -q"  # 透传其它 pytest 参数
#
param(
    [string]$m = "",
    [string]$Case = "",
    [string]$Extra = ""
)

$suite = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $suite)

$python = Join-Path $root ".venv\Scripts\python.exe"
if (-not (Test-Path $python)) {
    $python = (Get-Command python -ErrorAction SilentlyContinue).Source
}
if (-not $python) {
    throw "找不到 .venv\Scripts\python.exe；先在仓库根建 .venv（见 AGENT.md）"
}

$env:PYTHONUTF8 = "1"
$target = Join-Path $suite "cases"

$argv = @("-m", "pytest", $target, "-q")
if ($m) { $argv += @("-m", $m) }
if ($Case) { $argv += @("-k", $Case) }
if ($Extra) { $argv += $Extra.Split(" ") | Where-Object { $_ } }

Write-Host "> $python $($argv -join ' ')" -ForegroundColor DarkGray
& $python @argv
$code = $LASTEXITCODE

$latest = Get-ChildItem (Join-Path $root "test\artifacts") -Directory -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latest) {
    Write-Host ""
    Write-Host "产物：$($latest.FullName)" -ForegroundColor Cyan
    if ($code -ne 0) {
        Write-Host "失败时先看每个用例目录里的 mpv.log / vs.log / script.vpy" -ForegroundColor Yellow
    }
}

exit $code
