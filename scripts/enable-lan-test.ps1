#Requires -RunAsAdministrator
param([switch]$FirewallOnly)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$server=Join-Path $repo 'build-lan-business/WmsServer.exe'
if(!(Test-Path -LiteralPath $server)) { throw 'WmsServer.exe was not built.' }
$trusted=@(Get-NetConnectionProfile | Where-Object {
    $_.NetworkCategory -in @('Private','DomainAuthenticated') -and $_.IPv4Connectivity -ne 'Disconnected'
})
if($trusted.Count -eq 0) {
    throw '未检测到已连接的专用或域网络。请在 Windows 网络设置中将受信任的办公网络设为「专用网络」后重试。'
}
$ruleName='WMS-LAN-Business-Test'
$existing=Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue
if($existing) {
    Set-NetFirewallRule -Name $ruleName -Enabled True -Direction Inbound -Action Allow `
        -Profile Private,Domain -Program $server -Protocol TCP -LocalPort 18080,18081 -RemoteAddress LocalSubnet | Out-Null
} else {
    New-NetFirewallRule -Name $ruleName -DisplayName 'WMS LAN business test (local subnet)' `
        -Enabled True -Direction Inbound -Action Allow -Profile Private,Domain `
        -Program $server -Protocol TCP -LocalPort 18080,18081 -RemoteAddress LocalSubnet | Out-Null
}
if ($FirewallOnly) { return }
. (Join-Path $PSScriptRoot 'lan-data-storage.ps1')
$runtime = Join-Path $repo 'outputs/lan-dev'
$storageLock = Enter-LanRuntimeLock $runtime
try {
$dataRoot = & (Join-Path $PSScriptRoot 'get-lan-data-root.ps1')
Assert-LanStorageLayout $runtime $dataRoot
# Restart only the two development servers tracked by this workspace; keep PostgreSQL and all data.
& (Join-Path $PSScriptRoot 'stop-lan-dev.ps1')
& (Join-Path $PSScriptRoot 'start-lan-dev.ps1') -Product IceBeauty -AllowLan
& (Join-Path $PSScriptRoot 'start-lan-dev.ps1') -Product MeiJingLing -AllowLan
Write-Output 'LAN test enabled. Clients connect to the host LAN IPv4 address, ports 18080 and 18081.'
} finally { $storageLock.Dispose() }
