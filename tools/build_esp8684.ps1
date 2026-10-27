<#
文件作用：保留旧命令入口，将构建请求转交给 S2 Mini 构建脚本。
包含模块：旧命令兼容提示、参数透传、S2 Mini 构建脚本调用。
参数说明：LocalSimulation 启用 ESP 本地模拟；Flash 表示编译后烧录；Port 指定烧录串口。
调用方式：在 ESP-IDF 终端执行 .\tools\build_esp8684.ps1 -Flash -Port COM7。
#>
param([switch]$LocalSimulation, [switch]$Flash, [string]$Port)
Write-Warning 'The connected board is S2 Mini; use build_esp32s2.ps1 for new commands.'
& "$PSScriptRoot\build_esp32s2.ps1" @PSBoundParameters
