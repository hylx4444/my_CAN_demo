<#
文件作用：生成可在 CCS 中导入的 F2800157 演示工程描述文件。
包含模块：SDK 检查、路径转义、RAM 与 Flash 配置模板、XML 校验、工程文件输出。
参数说明：C2000Ware 为 SDK 根目录，默认读取环境变量，其次使用本机已知安装路径。
调用方式：执行 .\tools\generate_ccs_project.ps1，再在 CCS 中导入输出的 projectspec 文件。
#>
param([string]$C2000Ware = $env:C2000WARE_ROOT)
$ErrorActionPreference = 'Stop'
if (!$C2000Ware) { $C2000Ware = 'D:\work_study\C2000Ware\C2000Ware_26_02_00_00' }
if (!(Test-Path -LiteralPath "$C2000Ware\device_support\f280015x\common\include\device.h")) {
    throw 'Provide an existing C2000Ware installation using -C2000Ware.'
}
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot 'build\ccs'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$sdk = [Security.SecurityElement]::Escape($C2000Ware.Replace('\','/'))
$src = [Security.SecurityElement]::Escape($projectRoot.Replace('\','/'))
$xml = @'
<projectSpec>
  <project name="can_uart_demo" device="TMS320F2800157" cgtVersion="25.11.1.LTS"
    outputFormat="ELF" launchWizard="False" linkerCommandFile="" enableSysConfigTool="false">
    <configuration name="LAUNCHXL_FLASH"
      compilerBuildOptions="-v28 -ml -mt --abi=eabi --float_support=fpu32 --c99 --opt_level=2 --gen_func_subsections=on --define=DEBUG --define=_LAUNCHXL_F2800157 --define=_FLASH -I${C2000WARE_ROOT}/device_support/f280015x/common/include -I${C2000WARE_ROOT}/driverlib/f280015x/driverlib -I${DEMO_ROOT}/shared"
      linkerBuildOptions="--rom_model --entry_point=code_start --stack_size=0x200 --heap_size=0 -llibc.a" />
    <configuration name="LAUNCHXL_RAM"
      compilerBuildOptions="-v28 -ml -mt --abi=eabi --float_support=fpu32 --c99 --opt_level=2 --gen_func_subsections=on --define=DEBUG --define=_LAUNCHXL_F2800157 --define=RAM -I${C2000WARE_ROOT}/device_support/f280015x/common/include -I${C2000WARE_ROOT}/driverlib/f280015x/driverlib -I${DEMO_ROOT}/shared"
      linkerBuildOptions="--rom_model --entry_point=code_start --stack_size=0x200 --heap_size=0 -llibc.a" />
    <pathVariable name="C2000WARE_ROOT" path="@SDK@" scope="project" />
    <pathVariable name="DEMO_ROOT" path="@SRC@" scope="project" />
    <file action="link" path="@SRC@/firmware/c2000/main.c" />
    <file action="link" path="@SRC@/shared/demo_protocol.c" />
    <file action="link" path="@SDK@/device_support/f280015x/common/source/device.c" />
    <file action="link" path="@SDK@/device_support/f280015x/common/source/f280015x_codestartbranch.asm" />
    <file action="link" path="@SDK@/device_support/f280015x/common/cmd/280015x_generic_flash_lnk.cmd" applicableConfigurations="LAUNCHXL_FLASH" />
    <file action="link" path="@SDK@/device_support/f280015x/common/cmd/280015x_generic_ram_lnk.cmd" applicableConfigurations="LAUNCHXL_RAM" />
    <file action="link" path="@SDK@/driverlib/f280015x/driverlib/ccs/Debug/driverlib.lib" />
    <file action="copy" path="@SDK@/device_support/f280015x/common/targetConfigs/TMS320F2800157.ccxml" targetDirectory="targetConfigs" />
  </project>
</projectSpec>
'@
$xml = $xml.Replace('@SDK@', $sdk).Replace('@SRC@', $src)
[xml]$validated = $xml
$output = Join-Path $outputDir 'can_uart_demo.projectspec'
[IO.File]::WriteAllText($output, $xml, (New-Object Text.UTF8Encoding($false)))
Write-Host "Import in CCS: $output"
