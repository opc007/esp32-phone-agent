# Deploy the phone-agent firmware files to an ESP32-S3 that already runs MicroPython.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File deploy.ps1 [-Port COMx] [-Python <path>]
param(
    [string]$Port = "",
    [string]$Python = ""
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$fwDir = Join-Path $here "firmware"

function Resolve-Python {
    if ($Python) { return $Python }
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $cmd = Get-Command python3 -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "Python 3 not found. Install it, add it to PATH, or pass -Python <path>."
}
$py = Resolve-Python

if (-not (Test-Path (Join-Path $fwDir "config.py"))) {
    Write-Host "firmware/config.py is missing; copy config.example.py and fill it in." -ForegroundColor Yellow
    exit 1
}

function List-EspPorts {
    $code = "import serial.tools.list_ports as lp;"
    $code += "[print(p.device + '|' + p.hwid) for p in lp.comports() if '303A' in (p.hwid or '').upper()]"
    return & $py -c $code 2>$null
}

$candidates = @()
if ($Port) { $candidates = @($Port) }
else { $candidates = List-EspPorts | ForEach-Object { ($_ -split "\|")[0] } }
if (-not $candidates) {
    Write-Host "No ESP32 serial port found. Replug the board." -ForegroundColor Yellow
    exit 1
}

$mpPort = ""
foreach ($cand in $candidates) {
    Write-Host "Trying $cand ..."
    & $py -m mpremote connect $cand eval "print('ok')" 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { $mpPort = $cand; break }
}
if (-not $mpPort) {
    Write-Host "No MicroPython REPL found on: $($candidates -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "MicroPython port: $mpPort" -ForegroundColor Cyan

foreach ($f in @("config.py", "portal.py", "llm.py", "agent.py", "main.py")) {
    Write-Host "  upload $f"
    & $py -m mpremote connect $mpPort fs cp (Join-Path $fwDir $f) (":" + $f) 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "  upload $f FAILED" -ForegroundColor Red }
}
Write-Host "reset..."
& $py -m mpremote connect $mpPort reset 2>&1 | Out-Null
Write-Host "Done. Logs: $py -m mpremote connect $mpPort repl" -ForegroundColor Green
