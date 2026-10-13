<#
  公网隧道守护脚本：ssh 隧道断开后自动重连
  用途：把本机 8090 端口的模拟校园卡服务暴露成公网 https 地址
  说明：localhost.run 免注册即可用；带密钥可获得稳定子域名
  运行：powershell -NoProfile -ExecutionPolicy Bypass -File .\tunnel.ps1
#>
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$key = Join-Path $PSScriptRoot 'tunnelkey'
$kh  = Join-Path $PSScriptRoot 'known_hosts_tunnel'
$log = Join-Path $PSScriptRoot 'lhr.log'

if (-not (Test-Path $key)) {
    Write-Host '生成隧道密钥...'
    ssh-keygen -q -t ed25519 -f $key -N '' -C 'campus-card'
}

$sshArgs = @('-i', $key,
             '-o', 'StrictHostKeyChecking=no',
             '-o', ('UserKnownHostsFile=' + $kh),
             '-o', 'ServerAliveInterval=20',
             '-o', 'ServerAliveCountMax=3',
             '-o', 'ExitOnForwardFailure=yes',
             '-R', '80:127.0.0.1:8090',
             'nokey@localhost.run')

$n = 0
while ($true) {
    $n++
    Write-Host ('[{0}] 建立隧道 {1} ...' -f $n, (Get-Date -Format 'HH:mm:ss'))
    ssh @sshArgs 2>&1 | Tee-Object -FilePath $log -Append
    Write-Host ('[{0}] 隧道断开，5 秒后重连' -f $n)
    Start-Sleep -Seconds 5
}
