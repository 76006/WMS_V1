#Requires -RunAsAdministrator
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$server=Join-Path $repo 'build-lan-business/WmsServer.exe'
if(!(Test-Path -LiteralPath $server)) { throw 'WmsServer.exe was not built.' }
$trusted=@(Get-NetConnectionProfile | Where-Object { $_.NetworkCategory -in @('Private','DomainAuthenticated') })
if($trusted.Count -eq 0) {
    throw 'Set the trusted company network to Private in Windows network settings, then run again.'
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
# Restart only the two development servers tracked by this workspace; keep PostgreSQL and all data.
& (Join-Path $PSScriptRoot 'stop-lan-dev.ps1')
& (Join-Path $PSScriptRoot 'start-lan-dev.ps1') -Product IceBeauty -AllowLan
& (Join-Path $PSScriptRoot 'start-lan-dev.ps1') -Product MeiJingLing -AllowLan
Write-Output 'LAN test enabled. Clients connect to the host LAN IPv4 address, ports 18080 and 18081.'
