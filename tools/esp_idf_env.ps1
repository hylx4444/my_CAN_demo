<#
文件作用：在当前 PowerShell 进程中加载已安装的 ESP-IDF 环境，不修改系统设置或安装组件。
包含模块：安装器环境配置读取、进程变量设置、Python 与 SDK 路径验证。
参数说明：Profile 为 ESP-IDF 安装器生成的 PowerShell 配置脚本路径。
调用方式：在构建脚本中点加载 . "$PSScriptRoot\esp_idf_env.ps1"；已激活环境时复用现有配置。
#>
param([string]$Profile = 'C:\Espressif\tools\Microsoft.v6.0.3.PowerShell_profile.ps1')
$ErrorActionPreference = 'Stop'
if (!$env:IDF_PATH -or !$env:IDF_PYTHON_ENV_PATH) {
    if (!(Test-Path -LiteralPath $Profile)) { throw 'Open an ESP-IDF PowerShell terminal or provide -Profile.' }
    # 安装器的 -e 分支仅输出配置，不激活全局函数，也不执行 eim select。
    $espExports = & $Profile -e 6>&1
    foreach ($espExport in $espExports) {
        $espLine = $espExport.ToString()
        if ($espLine -match '^([A-Z_][A-Z0-9_]*)=(.*)$') {
            $espKey = $Matches[1]; $espValue = $Matches[2]
            if ($espKey -eq 'SYSTEM_PATH') { continue }
            if ($espKey -eq 'PATH') { $env:PATH = $espValue + ';' + $env:PATH }
            else { Set-Item -Path "Env:$espKey" -Value $espValue }
        }
    }
}
if (!(Test-Path -LiteralPath "$env:IDF_PATH\tools\idf.py")) { throw 'ESP-IDF path is invalid.' }
if (!(Test-Path -LiteralPath "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe")) { throw 'ESP-IDF Python environment is invalid.' }
$env:PATH = "$env:IDF_PYTHON_ENV_PATH\Scripts;$env:IDF_PATH\tools;$env:PATH"
