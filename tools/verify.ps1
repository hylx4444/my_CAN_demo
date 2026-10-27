<#
文件作用：统一验证真实传感器协议、电脑链路及两种开发板的固件构建。
包含模块：原生 C 测试、Node.js 集成测试、157 RAM/Flash 构建、可选 S2 Mini 双模式构建。
参数说明：SkipC2000 跳过 TI 构建；IncludeEsp32S2 额外构建 ESP 的串口及本地模拟固件。
调用方式：执行 .\tools\verify.ps1；完整检查使用 -IncludeEsp32S2，无 TI 工具链使用 -SkipC2000。
#>
param([switch]$SkipC2000, [switch]$IncludeEsp32S2)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
& "$PSScriptRoot\build_host.ps1"
if ($LASTEXITCODE -ne 0) { throw 'Host build failed' }
& node --test "$projectRoot\tests\test_simulator.mjs"
if ($LASTEXITCODE -ne 0) { throw 'Integration tests failed' }
if (!$SkipC2000) {
    & "$PSScriptRoot\build_c2000.ps1" -Configuration RAM
    & "$PSScriptRoot\build_c2000.ps1" -Configuration FLASH
}
if ($IncludeEsp32S2) {
    & "$PSScriptRoot\build_esp32s2.ps1"
    & "$PSScriptRoot\build_esp32s2.ps1" -LocalSimulation
}
