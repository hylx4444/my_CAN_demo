<#
文件作用：使用 TI C2000 编译器生成 F2800157 的 RAM 或 Flash 演示固件。
包含模块：SDK 与编译器路径检查、编译选项配置、源码编译、链接与映射文件输出。
参数说明：Configuration 选择 RAM 或 FLASH；C2000Ware 指定 SDK 目录；Compiler 指定编译器目录。
调用方式：在工程根目录执行 .\tools\build_c2000.ps1 -Configuration FLASH。
#>
param(
    [ValidateSet('RAM', 'FLASH')][string]$Configuration = 'FLASH',
    [string]$C2000Ware = $env:C2000WARE_ROOT,
    [string]$Compiler = $env:C2000_CGT_ROOT
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (!$C2000Ware) { $C2000Ware = 'D:\work_study\C2000Ware\C2000Ware_26_02_00_00' }
if (!$Compiler) { $Compiler = 'D:\work_study\CCS_21.0.1.00010_win\ccs\tools\compiler\ti-cgt-c2000_25.11.1.LTS' }
$cc = Join-Path $Compiler 'bin\cl2000.exe'
$support = Join-Path $C2000Ware 'device_support\f280015x\common'
$dlib = Join-Path $C2000Ware 'driverlib\f280015x\driverlib'
foreach ($required in @($cc, "$support\include\device.h", "$dlib\ccs\Debug\driverlib.lib")) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing dependency: $required. Supply -C2000Ware and -Compiler." }
}
$outDir = Join-Path $projectRoot "build\c2000\$Configuration"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$options = @('-v28', '-ml', '-mt', '--abi=eabi', '--float_support=fpu32', '--c99',
    '--opt_level=2', '--gen_func_subsections=on', '--define=_LAUNCHXL_F2800157',
    '--define=DEBUG', '--diag_warning=225', '--display_error_number',
    "--include_path=$support\include", "--include_path=$dlib",
    "--include_path=$projectRoot\shared", "--include_path=$Compiler\include")
if ($Configuration -eq 'FLASH') { $options += '--define=_FLASH' } else { $options += '--define=RAM' }
$sourceFiles = @("$projectRoot\firmware\c2000\main.c", "$projectRoot\shared\demo_protocol.c",
    "$support\source\device.c", "$support\source\f280015x_codestartbranch.asm")
$objects = @()
foreach ($sourceFile in $sourceFiles) {
    $objPath = Join-Path $outDir (([IO.Path]::GetFileNameWithoutExtension($sourceFile)) + '.obj')
    & $cc @options --compile_only "--output_file=$objPath" $sourceFile
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $sourceFile" }
    $objects += $objPath
}
$linkerFile = Join-Path $support ("cmd\280015x_generic_" + $Configuration.ToLowerInvariant() + '_lnk.cmd')
$outputFile = Join-Path $outDir 'can_uart_demo.out'
& $cc @options --run_linker --rom_model --entry_point=code_start --stack_size=0x200 --heap_size=0 `
    "--output_file=$outputFile" "--map_file=$outDir\can_uart_demo.map" `
    "--search_path=$Compiler\lib" @objects $linkerFile "$dlib\ccs\Debug\driverlib.lib" --library=libc.a
if ($LASTEXITCODE -ne 0) { throw 'Link failed' }
Write-Host "Built: $outputFile"
