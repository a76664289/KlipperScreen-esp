# ESP-IDF 构建环境包装器
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tools/idf.ps1 <idf.py 参数...>

$IdfPath  = 'F:\Espressif\frameworks\esp-idf-v5.4.4'
$IdfTools = 'F:\Espressif'
$IdfPyEnv = 'F:\Espressif\python_env\idf5.4_py3.11_env'

# 清掉可能从 Git Bash 继承来的错值（特别是 py3.9 那种），然后显式设置正确的
Remove-Item Env:IDF_PATH            -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM             -ErrorAction SilentlyContinue

$env:IDF_TOOLS_PATH      = $IdfTools
$env:IDF_PYTHON_ENV_PATH = $IdfPyEnv

if (-not (Test-Path "$IdfPath\export.ps1")) {
    Write-Error "ESP-IDF export.ps1 not found at $IdfPath\export.ps1"
    exit 1
}
if (-not (Test-Path "$IdfPyEnv\Scripts\python.exe")) {
    Write-Error "Python env not found at $IdfPyEnv\Scripts\python.exe"
    exit 1
}

. "$IdfPath\export.ps1" | Out-Null
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

idf.py @args
exit $LASTEXITCODE