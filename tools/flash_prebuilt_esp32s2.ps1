<#
文件作用：在没有 ESP-IDF 的电脑上校验并烧录仓库中的 S2 Mini 预编译固件。
包含模块：模式选择、发布清单校验、SHA256 校验、轻量 esptool 调用；不编译或安装软件。
参数说明：Port 为实际串口；Mode 默认 uart；Python 为 Python 命令或路径；EsptoolExe 可指定独立工具；CheckOnly 只校验，不连接板卡。
调用方式：.\tools\flash_prebuilt_esp32s2.ps1 -Port COM7；无板卡时使用 -CheckOnly；模拟模式增加 -Mode local-sim。
#>
param(
    [string]$Port,
    [ValidateSet('uart','local-sim')][string]$Mode = 'uart',
    [string]$Python = 'python',
    [string]$EsptoolExe,
    [ValidateRange(9600,921600)][int]$Baud = 460800,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$releaseDir = Join-Path $projectRoot "releases\esp32s2\$Mode"
$manifestFile = Join-Path $releaseDir 'manifest.json'
if (!(Test-Path -LiteralPath $manifestFile)) { throw "Missing prebuilt package: $releaseDir" }
$manifest = Get-Content -Raw -LiteralPath $manifestFile | ConvertFrom-Json
if ($manifest.schema_version -ne 1 -or $manifest.chip -ne 'esp32s2' -or
    $manifest.mode -ne $Mode -or $manifest.flash_size -ne '4MB' -or @($manifest.files).Count -ne 3) {
    throw 'Invalid S2 Mini release manifest.'
}
$expectedFiles = @{ '0x1000'='bootloader.bin'; '0x8000'='partition-table.bin'; '0x10000'='application.bin' }
$seenOffsets = @{}
$segments = @()
foreach ($file in $manifest.files) {
    if (!$expectedFiles.ContainsKey($file.offset) -or $expectedFiles[$file.offset] -ne $file.file -or $seenOffsets.ContainsKey($file.offset)) {
        throw 'Unexpected or duplicate Flash segment.'
    }
    $seenOffsets[$file.offset] = $true
    $filePath = Join-Path $releaseDir $file.file
    if (!(Test-Path -LiteralPath $filePath)) { throw "Missing binary: $filePath" }
    if ((Get-Item -LiteralPath $filePath).Length -ne $file.bytes -or
        (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Binary checksum mismatch: $($file.file). Fetch the complete release again."
    }
    $segments += @($file.offset, $filePath)
}
Write-Host "Package verified: $($manifest.board), mode=$Mode, ESP-IDF $($manifest.esp_idf)"
if ($CheckOnly) { Write-Host 'CheckOnly: no serial port was opened.'; return }
if (!$Port) { throw 'Supply the board port using -Port COMx, or use -CheckOnly.' }
$flashOptions = @('--chip','esp32s2','--port',$Port,'--baud',"$Baud",'--before','default-reset',
    '--after','hard-reset','write-flash','--flash-mode',$manifest.flash_mode,
    '--flash-size',$manifest.flash_size,'--flash-freq',$manifest.flash_freq) + $segments
if ($EsptoolExe) {
    & $EsptoolExe version
    if ($LASTEXITCODE -ne 0) { throw 'Cannot start standalone esptool.' }
    & $EsptoolExe @flashOptions
} else {
    & $Python -m esptool version
    if ($LASTEXITCODE -ne 0) { throw 'Install esptool 5.x with your Python interpreter, or supply -EsptoolExe.' }
    & $Python -m esptool @flashOptions
}
if ($LASTEXITCODE -ne 0) { throw 'Flashing failed. Check port, USB cable, and BOOT/RESET state.' }
Write-Host 'Flashing complete. Press RESET if the board does not restart automatically.'
