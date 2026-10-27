<#
文件作用：在有 ESP-IDF 的电脑编译 S2 Mini，并生成可随项目提交的预编译烧录包。
包含模块：双模式构建、Flash 参数读取、启动程序与分区及应用复制、SHA256 校验和清单生成。
参数说明：Mode 选择 uart、local-sim 或 all，默认同时发布正常与本地模拟两种模式。
调用方式：在工程根目录执行 .\tools\package_esp32s2.ps1；本脚本只构建和打包，不烧录。
#>
param([ValidateSet('uart','local-sim','all')][string]$Mode = 'all')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$packageModes = if ($Mode -eq 'all') { @('uart','local-sim') } else { @($Mode) }
$utf8NoBom = New-Object Text.UTF8Encoding($false)
foreach ($packageMode in $packageModes) {
    if ($packageMode -eq 'local-sim') { & "$PSScriptRoot\build_esp32s2.ps1" -LocalSimulation }
    else { & "$PSScriptRoot\build_esp32s2.ps1" }
    $buildDir = Join-Path $projectRoot "build\esp32s2\$packageMode"
    $releaseDir = Join-Path $projectRoot "releases\esp32s2\$packageMode"
    $flashConfig = Get-Content -Raw -LiteralPath "$buildDir\flasher_args.json" | ConvertFrom-Json
    $description = Get-Content -Raw -LiteralPath "$buildDir\project_description.json" | ConvertFrom-Json
    if ($flashConfig.extra_esptool_args.chip -ne 'esp32s2' -or $flashConfig.flash_settings.flash_size -ne '4MB') {
        throw 'Unexpected chip or Flash size; refusing to package this build.'
    }
    if (@($flashConfig.flash_files.PSObject.Properties).Count -ne 3) {
        throw 'Unexpected Flash segments; update packaging before publishing this build.'
    }
    New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
    $files = @()
    foreach ($segment in $flashConfig.flash_files.PSObject.Properties) {
        $fileName = switch ($segment.Name) {
            '0x1000' { 'bootloader.bin' }
            '0x8000' { 'partition-table.bin' }
            '0x10000' { 'application.bin' }
            default { throw "Unexpected Flash offset: $($segment.Name)" }
        }
        Copy-Item -LiteralPath (Join-Path $buildDir $segment.Value) -Destination (Join-Path $releaseDir $fileName)
        $releaseFile = Get-Item -LiteralPath (Join-Path $releaseDir $fileName)
        $files += [ordered]@{
            offset = $segment.Name
            file = $fileName
            bytes = $releaseFile.Length
            sha256 = (Get-FileHash -LiteralPath $releaseFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $manifest = [ordered]@{
        schema_version = 1
        board = 'WEMOS S2 Mini'
        chip = 'esp32s2'
        mode = $packageMode
        project = $description.project_name
        project_version = $description.project_version
        esp_idf = $description.git_revision
        packaged_at_utc = [DateTime]::UtcNow.ToString('o')
        flash_mode = $flashConfig.flash_settings.flash_mode
        flash_size = $flashConfig.flash_settings.flash_size
        flash_freq = $flashConfig.flash_settings.flash_freq
        files = $files
    }
    [IO.File]::WriteAllText("$releaseDir\manifest.json", ($manifest | ConvertTo-Json -Depth 6) + "`n", $utf8NoBom)
    $checksumLines = @($files | ForEach-Object { $_.sha256 + '  ' + $_.file })
    [IO.File]::WriteAllText("$releaseDir\SHA256SUMS.txt", ($checksumLines -join "`n") + "`n", $utf8NoBom)
    Write-Host "Packaged: $releaseDir"
}
