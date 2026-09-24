# debug_check.ps1 —— 一键体检：把整条链路查一遍，直接告诉你哪里出问题
#
# 用法（在你自己电脑上跑，不用远程服务器）：
#   powershell -ExecutionPolicy Bypass -File debug_check.ps1
#
# 可选参数：
#   -Server 39.106.216.80      服务器地址
#   -Port 8000                 端口
#   -DeviceToken "xxx"         上传用的 token
#   -ViewToken "yyy"           看画面用的 token
#   -ImagePath ".\test.jpg"    测试用的图片（不填就跳过上传检查）

param(
    [string]$Server = "39.106.216.80",
    [int]$Port = 8000,
    [string]$DeviceToken = "YFOpzRWJxw6G2dl4jkNUMX7h",
    [string]$ViewToken = "Jxt7HvzaZVfhkF1D",
    [string]$ImagePath = ""
)

$base = "http://${Server}:${Port}"
$ErrorActionPreference = "Continue"
$ProgressPreference = "SilentlyContinue"

function Show($ok, $msg) {
    if ($ok) { Write-Host "  [通过] $msg" -ForegroundColor Green }
    else     { Write-Host "  [问题] $msg" -ForegroundColor Red }
}

Write-Host ""
Write-Host "========== 体检目标：$base ==========" -ForegroundColor Cyan

Write-Host ""
Write-Host "[1] 服务器活着吗" -ForegroundColor Yellow
try {
    $h = Invoke-RestMethod "$base/health" -TimeoutSec 10
    Show ($h -match "OK") "服务器在线 (/health 返回 $h)"
} catch {
    Show $false "连不上服务器：$($_.Exception.Message)"
    Write-Host ""
    Write-Host "  → 服务器层问题：需要远程到服务器重启服务" -ForegroundColor Yellow
    Write-Host "    Get-Process python | Stop-Process -Force" -ForegroundColor Gray
    Write-Host "    然后重新启动 uvicorn，看 C:\SmartCaneServer\service.err.log" -ForegroundColor Gray
    exit 1
}

Write-Host ""
Write-Host "[2] 设备在推帧吗" -ForegroundColor Yellow
try {
    $s = Invoke-RestMethod "$base/status" -TimeoutSec 10
    Write-Host "     $s"
    if ($s -match "age=([\d.]+)s") {
        $age = [double]$Matches[1]
        Show ($age -lt 5) "设备在线，最新帧 $age 秒前"
    } elseif ($s -match "no-frame") {
        Show $false "没有帧：设备没推上来（no-frame）"
        Write-Host ""
        Write-Host "  → 设备层问题，按顺序查：" -ForegroundColor Yellow
        Write-Host "    1. ESP32 烧了固件吗？串口看到 [upload] ... HTTP 200 吗？" -ForegroundColor Gray
        Write-Host "    2. HTTP 401 → token 错了；HTTP -1 → WiFi 没网" -ForegroundColor Gray
        Write-Host "    3. ESP32 只支持 2.4GHz WiFi" -ForegroundColor Gray
    } else {
        Show $false "返回异常：$s"
    }
} catch {
    Show $false "查 status 失败：$($_.Exception.Message)"
}

if ($ImagePath -and (Test-Path $ImagePath)) {
    Write-Host ""
    Write-Host "[3] 上传通道正常吗（用 $ImagePath 测试）" -ForegroundColor Yellow
    try {
        $bytes = [System.IO.File]::ReadAllBytes($ImagePath)
        $r = Invoke-WebRequest -Method Post -Uri "$base/upload?token=$DeviceToken" -ContentType "image/jpeg" -Body $bytes -TimeoutSec 30
        Show ($r.StatusCode -eq 200) "上传成功 HTTP $($r.StatusCode)，$($bytes.Length) 字节"
    } catch {
        $code = $_.Exception.Response.StatusCode.value__
        Show $false "上传失败 HTTP $code"
        if ($code -eq 401) { Write-Host "  → token 错了，核对服务器端 DEVICE_TOKEN" -ForegroundColor Yellow }
    }

    Write-Host ""
    Write-Host "[4] 识别能出结果吗" -ForegroundColor Yellow
    try {
        $d = Invoke-RestMethod "$base/detect" -TimeoutSec 60
        Write-Host "     识别结果：$d"
        Show ($d -match "\|") "识别接口正常"
    } catch {
        $code = $_.Exception.Response.StatusCode.value__
        Show $false "识别失败 HTTP $code"
        if ($code -eq 503) { Write-Host "  → 超过 20 秒没有新帧，先做第 3 步上传" -ForegroundColor Yellow }
    }
} else {
    Write-Host ""
    Write-Host "[3-4] 跳过（没指定测试图片）" -ForegroundColor DarkGray
    Write-Host "     想测的话加参数：-ImagePath `".\test_annotated.png`"" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "[5] 家属端能取到图吗" -ForegroundColor Yellow
try {
    $img = Invoke-WebRequest "$base/latest.jpg?token=$ViewToken" -TimeoutSec 30
    Show ($img.StatusCode -eq 200) "取图成功 HTTP $($img.StatusCode)，$($img.RawContentLength) 字节"
} catch {
    $code = $_.Exception.Response.StatusCode.value__
    Show $false "取图失败 HTTP $code"
    if ($code -eq 401) { Write-Host "  → VIEW_TOKEN 错了" -ForegroundColor Yellow }
    if ($code -eq 503) { Write-Host "  → 没有可用帧，先让设备推帧或手动上传" -ForegroundColor Yellow }
}

Write-Host ""
Write-Host "========== 体检结束 ==========" -ForegroundColor Cyan
Write-Host "浏览器直接看："
Write-Host "  状态   $base/status" -ForegroundColor Gray
Write-Host "  画面   $base/latest.jpg?token=$ViewToken" -ForegroundColor Gray
Write-Host ""
