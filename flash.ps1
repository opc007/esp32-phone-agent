# Flash MicroPython + deploy the phone-agent firmware to an ESP32-S3.
#
# Arduino/ESP-IDF toolchains are NOT needed: this only uses esptool + mpremote.
#
# Prerequisites:
#   python -m pip install esptool mpremote
#   python tools\download_firmware.py              # fetch MicroPython images
#   copy firmware\config.example.py firmware\config.py   then edit it
#
# Then put the board into serial download mode (hold BOOT, plug USB / press RST,
# release BOOT after ~2s) and run:
#   powershell -ExecutionPolicy Bypass -File flash.ps1
#
# Options:
#   -Port COMx        serial port in download mode (auto-detected by default)
#   -Variant auto     plain/oct from detected flash size (or force plain|oct)
#   -Python <path>    Python 3 executable (default: python on PATH)
param(
    [string]$Port = "",
    [string]$Variant = "auto",
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
    Write-Host "firmware/config.py is missing." -ForegroundColor Yellow
    Write-Host "Copy firmware/config.example.py to firmware/config.py and fill in WIFI/PORTAL/DEEPSEEK values." -ForegroundColor Yellow
    exit 1
}

$plainBin = Get-ChildItem $fwDir -Filter "ESP32_GENERIC_S3-*.bin" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notmatch "SPIRAM" } | Select-Object -First 1
$octBin = Get-ChildItem $fwDir -Filter "*SPIRAM_OCT*.bin" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $plainBin -and -not $octBin) {
    Write-Host "MicroPython images not found. Run: python tools\download_firmware.py" -ForegroundColor Yellow
    exit 1
}

function List-EspPorts {
    $code = "import serial.tools.list_ports as lp;"
    $code += "[print(p.device + '|' + p.hwid) for p in lp.comports() if '303A' in (p.hwid or '').upper()]"
    return & $py -c $code 2>$null
}

# 1) find the download-mode port (VID 303A, not the 4001 app-mode PID)
if (-not $Port) {
    $cands = List-EspPorts
    $dl = $cands | Where-Object { $_ -notmatch "PID=4001" } | Select-Object -First 1
    if ($dl) { $Port = ($dl -split "\|")[0] }
}
if (-not $Port) {
    Write-Host "No download-mode port found. Hold BOOT, plug USB, release after 2s, then retry." -ForegroundColor Yellow
    exit 1
}
Write-Host "Using port $Port" -ForegroundColor Cyan

# 2) chip / flash info
Write-Host "=== chip info ===" -ForegroundColor Cyan
$info = & $py -m esptool -p $Port -c esp32s3 flash-id 2>&1
$info | Out-Host
$flashMB = 0
foreach ($line in $info) {
    if ($line -match "Detected flash size:\s*(\d+)\s*MB") { $flashMB = [int]$Matches[1] }
}
if ($flashMB -eq 0) {
    Write-Host "Could not read flash size; the board is probably not in download mode." -ForegroundColor Red
    exit 1
}

# 3) pick firmware
if ($Variant -eq "auto") { $bin = if ($flashMB -ge 8 -and $octBin) { $octBin } else { $plainBin } }
elseif ($Variant -eq "oct") { $bin = $octBin }
else { $bin = $plainBin }
if (-not $bin) { Write-Host "Requested firmware variant is not downloaded." -ForegroundColor Red; exit 1 }
Write-Host "Flash=${flashMB}MB -> $($bin.Name)" -ForegroundColor Cyan

# 4) erase + write (ESP32-S3 image starts at 0x0)
& $py -m esptool -p $Port -c esp32s3 erase-flash 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) { Write-Host "erase-flash failed" -ForegroundColor Red; exit 1 }
& $py -m esptool -p $Port -c esp32s3 --baud 460800 write-flash 0 $bin.FullName 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) { Write-Host "write-flash failed" -ForegroundColor Red; exit 1 }

# 5) wait for MicroPython to enumerate
Write-Host "Waiting for MicroPython to enumerate (release BOOT / press RST if it does not)..." -ForegroundColor Cyan
$mpPort = ""
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Seconds 1
    $cands = List-EspPorts
    if ($cands) { $mpPort = (($cands | Select-Object -Last 1) -split "\|")[0]; break }
}
if (-not $mpPort) {
    Write-Host "MicroPython port not found. Replug USB, then run deploy.ps1." -ForegroundColor Yellow
    exit 1
}
Write-Host "MicroPython port: $mpPort" -ForegroundColor Cyan

# 6) deploy firmware files
Write-Host "Deploying firmware files..." -ForegroundColor Cyan
foreach ($f in @("config.py", "portal.py", "llm.py", "agent.py", "main.py")) {
    Write-Host "  $f"
    & $py -m mpremote connect $mpPort fs cp (Join-Path $fwDir $f) (":" + $f) 2>&1 | Out-Null
}
& $py -m mpremote connect $mpPort reset 2>&1 | Out-Null
Write-Host ""
Write-Host "Done. Watch the log with:" -ForegroundColor Green
Write-Host "  $py -m mpremote connect $mpPort repl" -ForegroundColor Green
