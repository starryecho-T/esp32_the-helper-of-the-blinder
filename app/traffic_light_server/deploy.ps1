# deploy.ps1 —— 在 Windows Server（阿里云 ECS Windows 镜像）上部署交通灯识别服务
#
# 用法（管理员权限的 PowerShell）：
#   cd C:\SmartCaneServer
#   powershell -ExecutionPolicy Bypass -File deploy.ps1
#
# 可选参数：
#   -Port 8000            监听端口
#   -Python "python"      python 可执行文件名或完整路径
#   -UseNssm              已安装 NSSM 时，注册成真正的 Windows 服务（推荐）
#
# 脚本做的事：
#   1. 检查 python 与依赖，缺的用阿里云镜像补装
#   2. 写入系统环境变量（DEVICE_TOKEN 等），server.py 会直接读取
#   3. 防火墙放行端口（阿里云安全组还要另外在控制台放行）
#   4. 启动 uvicorn，并配置开机自启（NSSM 服务 或 计划任务）
#   5. 自检 /health 与 /status

param(
    [int]$Port = 8000,
    [string]$Python = "python",
    [switch]$UseNssm
)

$ErrorActionPreference = "Stop"
$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$Mirror = "https://mirrors.aliyun.com/pypi/simple/"
$ServiceName = "TrafficLightServer"
$LogOut = Join-Path $AppDir "service.log"
$LogErr = Join-Path $AppDir "service.err.log"

function Write-Step($msg) { Write-Host ""; Write-Host "=== $msg ===" -ForegroundColor Cyan }

Write-Step "1/6 检查 Python"
$py = Get-Command $Python -ErrorAction SilentlyContinue
if ($null -eq $py) { throw "找不到 $Python，请先安装 Python 3.10 或 3.11，并勾选 Add to PATH" }
$pyPath = $py.Source
Write-Host "  python: $pyPath"
& $pyPath -V

Write-Step "2/6 检查依赖"
$probe = "import importlib.util; ms=['fastapi','uvicorn','ultralytics','cv2','numpy','requests']; print(','.join([m for m in ms if importlib.util.find_spec(m) is None]))"
$missing = (& $pyPath -c $probe).Trim()
if ($missing -ne "") {
    Write-Host "  缺少: $missing -> 用阿里云镜像补装（torch 较大，请耐心）" -ForegroundColor Yellow
    & $pyPath -m pip install -r (Join-Path $AppDir "requirements.txt") -i $Mirror
} else {
    Write-Host "  fastapi / uvicorn / ultralytics / cv2 / numpy / requests 齐全"
}

if (-not (Test-Path (Join-Path $AppDir "yolov8n.pt"))) {
    throw "缺少 yolov8n.pt，请把它放到 $AppDir"
}
Write-Step "3/6 模型文件 yolov8n.pt 已就位"

Write-Step "4/6 写入环境变量并生成 DEVICE_TOKEN"
$chars = 48..57 + 65..90 + 97..122
$token = -join ($chars | Get-Random -Count 24 | ForEach-Object { [char]$_ })
$envVars = @{
    YOLO_MODEL    = Join-Path $AppDir "yolov8n.pt"
    FRAME_PATH    = Join-Path $AppDir "latest.jpg"
    FRAME_MAX_AGE = "20"
    CAMERA_URL    = ""
    DEVICE_TOKEN  = $token
}
foreach ($k in $envVars.Keys) {
    [Environment]::SetEnvironmentVariable($k, $envVars[$k], "Machine")
    Set-Item -Path "Env:$k" -Value $envVars[$k]
}
Write-Host "  DEVICE_TOKEN = $token" -ForegroundColor Green
Write-Host "  这个 token 要填进 ESP32 固件的 DEVICE_TOKEN" -ForegroundColor Yellow

Write-Step "5/6 配置开机自启并启动"
$uvicornArgs = "-m uvicorn server:app --host 0.0.0.0 --port $Port --workers 1"

$nssm = Get-Command nssm -ErrorAction SilentlyContinue
if ($UseNssm -and ($null -eq $nssm)) {
    Write-Host "  没找到 nssm，改用计划任务。想用服务模式：下载 https://nssm.cc/download 放入 PATH 后重跑" -ForegroundColor Yellow
    $UseNssm = $false
}

if ($UseNssm) {
    & nssm install $ServiceName $pyPath $uvicornArgs | Out-Null
    & nssm set $ServiceName AppDirectory $AppDir | Out-Null
    & nssm set $ServiceName AppStdout $LogOut | Out-Null
    & nssm set $ServiceName AppStderr $LogOut | Out-Null
    & nssm start $ServiceName | Out-Null
    Write-Host "  已用 NSSM 注册并启动服务: $ServiceName"
} else {
    $tr = "cmd /c cd /d $AppDir && $pyPath $uvicornArgs"
    schtasks /Create /TN $ServiceName /SC ONSTART /RU SYSTEM /RL HIGHEST /TR $tr /F | Out-Null
    Write-Host "  已创建开机启动计划任务: $ServiceName"
    $proc = Start-Process -FilePath $pyPath -ArgumentList $uvicornArgs -WorkingDirectory $AppDir `
        -RedirectStandardOutput $LogOut -RedirectStandardError $LogErr -PassThru -WindowStyle Hidden
    Write-Host "  已启动进程 PID=$($proc.Id)，日志: $LogOut"
}

Write-Step "6/6 防火墙放行与自检"
$rule = Get-NetFirewallRule -DisplayName $ServiceName -ErrorAction SilentlyContinue
if ($null -eq $rule) {
    New-NetFirewallRule -DisplayName $ServiceName -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow | Out-Null
    Write-Host "  Windows 防火墙已放行 TCP $Port"
} else {
    Write-Host "  Windows 防火墙规则已存在"
}

Start-Sleep -Seconds 6
try {
    $h = Invoke-RestMethod "http://127.0.0.1:$Port/health"
    Write-Host "  /health -> $h"
} catch {
    Write-Host "  /health 无响应，看日志: $LogOut" -ForegroundColor Red
}
try {
    $s = Invoke-RestMethod "http://127.0.0.1:$Port/status"
    Write-Host "  /status -> $s"
} catch { }

Write-Host ""
Write-Host "完成。别忘了：" -ForegroundColor Cyan
Write-Host "  1. 阿里云控制台 -> 安全组 -> 入方向放行 TCP $Port（脚本管不到）"
Write-Host "  2. 本机测试：浏览器打开 http://127.0.0.1:$Port/health"
Write-Host "  3. ESP32 固件里 SERVER_HOST 改成 http://<ECS公网IP>:$Port ，DEVICE_TOKEN 填上面的值"
