<#
文件作用：验证传感器和公共 C 协议，再生成测试夹具与电脑模拟源。
包含模块：GCC 编译选项、传感器测试、协议测试、二进制夹具与模拟源编译。
参数说明：Gcc 为 GCC 编译器命令或完整路径，默认使用当前环境中的 gcc。
调用方式：在工程根目录执行 .\tools\build_host.ps1，测试失败时脚本终止。
#>
param([string]$Gcc = 'gcc')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot 'build\host'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$flags = @('-std=c99', '-Wall', '-Wextra', '-Werror', '-O2', "-I$projectRoot\shared")
& $Gcc @flags "$projectRoot\shared\demo_protocol.c" "$projectRoot\tests\test_sensor.c" -o "$outputDir\test_sensor.exe"
if ($LASTEXITCODE -ne 0) { throw 'Sensor test compilation failed' }
& "$outputDir\test_sensor.exe"
if ($LASTEXITCODE -ne 0) { throw 'Sensor tests failed' }
& $Gcc @flags "$projectRoot\shared\demo_protocol.c" "$projectRoot\tests\sensor_fixture.c" -o "$outputDir\sensor_fixture.exe"
if ($LASTEXITCODE -ne 0) { throw 'Sensor fixture compilation failed' }
& $Gcc @flags "$projectRoot\shared\demo_protocol.c" "$projectRoot\tests\test_protocol.c" -o "$outputDir\test_protocol.exe"
if ($LASTEXITCODE -ne 0) { throw 'Protocol test compilation failed' }
& "$outputDir\test_protocol.exe"
if ($LASTEXITCODE -ne 0) { throw 'Protocol tests failed' }
& $Gcc @flags "$projectRoot\shared\demo_protocol.c" "$projectRoot\tools\host_source.c" -o "$outputDir\host_source.exe"
if ($LASTEXITCODE -ne 0) { throw 'Host simulator compilation failed' }
Write-Host "Built: $outputDir\host_source.exe"
