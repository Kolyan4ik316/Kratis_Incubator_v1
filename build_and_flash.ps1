$cli      = "$env:TEMP\arduino-cli\arduino-cli.exe"
$config   = "$env:TEMP\arduino-cli\cli-config.yaml"
$sketch   = "C:\Users\Admin\Documents\Arduino\sketch_jan6a"
$build    = "$env:TEMP\build_jan6a_flash"
$esptool  = "C:\Users\Admin\Documents\Arduino\hardware\espressif\esp32\tools\esptool\esptool.exe"
$fqbn     = "espressif:esp32:esp32c3:UploadSpeed=921600,CDCOnBoot=cdc,CPUFreq=160,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs"

# --- Пошук arduino-cli якщо немає в TEMP ---
if (-not (Test-Path $cli)) {
    Write-Host "arduino-cli not found in TEMP, downloading..." -ForegroundColor Yellow
    Invoke-WebRequest -Uri "https://downloads.arduino.cc/arduino-cli/nightly/arduino-cli_nightly-latest_Windows_64bit.zip" -OutFile "$env:TEMP\arduino-cli.zip" -UseBasicParsing
    Expand-Archive -Path "$env:TEMP\arduino-cli.zip" -DestinationPath "$env:TEMP\arduino-cli" -Force
}

# --- Конфіг ---
if (-not (Test-Path $config)) {
@"
board_manager:
  additional_urls: []
directories:
  data: C:\Users\Admin\AppData\Local\Arduino15
  downloads: C:\Users\Admin\AppData\Local\Arduino15\staging
  user: C:\Users\Admin\Documents\Arduino
"@ | Out-File -Encoding UTF8 $config
}

New-Item -ItemType Directory -Force -Path $build | Out-Null

# --- Компіляція ---
Write-Host ""
Write-Host "=== COMPILING ===" -ForegroundColor Cyan
& $cli compile --config-file $config --fqbn $fqbn --build-path $build $sketch 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "COMPILE FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "Compile OK" -ForegroundColor Green

# --- Пошук COM-порту ---
Write-Host ""
Write-Host "=== DETECTING PORT ===" -ForegroundColor Cyan
$ports = Get-PnpDevice -Class Ports -Status OK -ErrorAction SilentlyContinue |
         Where-Object { $_.FriendlyName -match "USB|CP210|CH340|CH9102|UART|Serial" }

if ($ports.Count -eq 0) {
    Write-Host "No USB-Serial port found! Connect ESP32 and retry." -ForegroundColor Red
    exit 1
}

# Витягуємо номер COM-порту
$comMatch = $ports[0].FriendlyName -match "COM(\d+)"
if (-not $comMatch) {
    Write-Host "Cannot parse COM port from: $($ports[0].FriendlyName)" -ForegroundColor Red
    exit 1
}
$comPort = "COM" + $Matches[1]
Write-Host "Found: $($ports[0].FriendlyName) -> $comPort" -ForegroundColor Green

# --- Прошивка ---
Write-Host ""
Write-Host "=== FLASHING to $comPort ===" -ForegroundColor Cyan
& $esptool --chip esp32c3 --port $comPort --baud 921600 `
    --before default_reset --after hard_reset write_flash `
    --flash_mode dio --flash_freq 80m --flash_size 4MB `
    0x0    "$build\sketch_jan6a.ino.bootloader.bin" `
    0x8000 "$build\sketch_jan6a.ino.partitions.bin" `
    0xe000 "C:\Users\Admin\Documents\Arduino\hardware\espressif\esp32\tools\partitions\boot_app0.bin" `
    0x10000 "$build\sketch_jan6a.ino.bin"

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "=== FLASH SUCCESSFUL ===" -ForegroundColor Green
} else {
    Write-Host "FLASH FAILED" -ForegroundColor Red
}
