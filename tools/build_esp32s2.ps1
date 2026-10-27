<#
文件作用：构建 WEMOS S2 Mini 固件，并可选通过明确指定的串口烧录。
包含模块：ESP-IDF 环境加载、真实串口与本地模拟配置、编译、可选烧录。
参数说明：LocalSimulation 启用本地模拟；Flash 编译后烧录；Port 是烧录串口，如 COM7。
调用方式：执行 .\tools\build_esp32s2.ps1；仅预览热点时附加 -LocalSimulation；烧录需 -Flash -Port COMx。
#>
param([switch]$LocalSimulation, [switch]$Flash, [string]$Port)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\esp_idf_env.ps1"
if ($Flash -and !$Port) { throw 'Supply -Port COMx when using -Flash.' }
$projectRoot = Split-Path -Parent $PSScriptRoot
$project = (Join-Path $projectRoot 'firmware\esp32s2').Replace('\','/')
$mode = if ($LocalSimulation) { 'local-sim' } else { 'uart' }
$outputDir = (Join-Path $projectRoot "build\esp32s2\$mode").Replace('\','/')
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$defaults = "$project/sdkconfig.defaults"
if ($LocalSimulation) { $defaults += ";$project/sdkconfig.local-sim" }
$options = @('-C', $project, '-B', $outputDir, '-D', 'IDF_TARGET=esp32s2',
    '-D', "SDKCONFIG=$outputDir/sdkconfig", '-D', "SDKCONFIG_DEFAULTS=$defaults")
& "$env:IDF_PYTHON_ENV_PATH/Scripts/python.exe" "$env:IDF_PATH/tools/idf.py" @options build
if ($LASTEXITCODE -ne 0) { throw 'S2 Mini build failed.' }
if ($Flash) {
    & "$env:IDF_PYTHON_ENV_PATH/Scripts/python.exe" "$env:IDF_PATH/tools/idf.py" @options -p $Port flash
    if ($LASTEXITCODE -ne 0) { throw 'S2 Mini flashing failed.' }
}
