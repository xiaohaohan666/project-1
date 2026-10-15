# ============================================================
#  校园一卡通 —— 启动脚本（PowerShell 版）
#  用法：右键「使用 PowerShell 运行」
#       或：powershell -ExecutionPolicy Bypass -File .\start.ps1
#  可选参数：端口号，默认 8090
# ============================================================
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Set-Location $PSScriptRoot

$port = if ($args.Count -ge 1) { [int]$args[0] } else { 8090 }

Write-Host '============================================================' -ForegroundColor Cyan
Write-Host '  校园一卡通 —— 启动' -ForegroundColor Cyan
Write-Host '============================================================' -ForegroundColor Cyan
Write-Host ''

# ---------- 1. 找编译器 ----------
$gcc = $null
foreach ($c in @('gcc',
                 'C:\Program Files\mingw64\bin\gcc.exe',
                 'C:\mingw64\bin\gcc.exe',
                 'E:\mingw64\bin\gcc.exe')) {
    $cmd = Get-Command $c -ErrorAction SilentlyContinue
    if ($cmd) { $gcc = $cmd.Source; break }
    if (Test-Path $c) { $gcc = $c; break }
}

# ---------- 2. 需要时编译 ----------
$needBuild = -not (Test-Path 'server.exe')
if (-not $needBuild) {
    $exeTime = (Get-Item 'server.exe').LastWriteTime
    foreach ($src in @('src\campus.c', 'src\campus.h', 'src\server.c')) {
        if ((Get-Item $src).LastWriteTime -gt $exeTime) { $needBuild = $true; break }
    }
}

if ($needBuild) {
    if (-not $gcc) {
        Write-Host '[错误] 没有找到 gcc，且 server.exe 不存在或已过期。' -ForegroundColor Red
        Write-Host '       请安装 MinGW-w64，或把编译好的 server.exe 放到本目录。'
        Read-Host '按回车退出'
        exit 1
    }
    Write-Host '[1/2] 正在编译（源码是 UTF-8，必须带 -fexec-charset=UTF-8）...' -ForegroundColor Yellow
    & $gcc -std=c11 -Wall -Wextra -O2 -fexec-charset=UTF-8 `
           -Isrc src\server.c src\campus.c -o server.exe -lws2_32
    if ($LASTEXITCODE -ne 0) {
        Write-Host '[错误] 编译失败，请查看上面的报错。' -ForegroundColor Red
        Read-Host '按回车退出'
        exit 1
    }
    Write-Host '      编译完成。' -ForegroundColor Green
    Write-Host ''
}

# ---------- 3. 取内网 IP ----------
$ip = '127.0.0.1'
try {
    $lan = [System.Net.Dns]::GetHostAddresses([System.Net.Dns]::GetHostName()) |
           Where-Object { $_.AddressFamily -eq 'InterNetwork' -and
                          $_.IPAddressToString -notlike '169.254.*' } |
           Select-Object -First 1
    if ($lan) { $ip = $lan.IPAddressToString }
} catch { }

Write-Host "[2/2] 正在启动服务（监听 0.0.0.0:$port，局域网可访问）..." -ForegroundColor Yellow
Write-Host ''
Write-Host '  ----------------------------------------------------------'
Write-Host "    本机打开      http://127.0.0.1:$port/" -ForegroundColor Green
Write-Host "    手机/同网设备  http://${ip}:$port/     （需同一个 WiFi）" -ForegroundColor Green
Write-Host '  ----------------------------------------------------------'
Write-Host '    演示账号'
Write-Host '      学生       C001 / C002 / C003     登录密码 123456'
Write-Host '      交易密码   010101 / 010102 / 010103'
Write-Host '      管理员     admin123'
Write-Host '  ----------------------------------------------------------'
Write-Host '    关闭本窗口即停止服务；数据保存在 data\ 目录。'
Write-Host '  ----------------------------------------------------------'
Write-Host ''

Start-Process "http://127.0.0.1:$port/"
& '.\server.exe' $port web data 0.0.0.0