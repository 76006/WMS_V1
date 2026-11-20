# Run initialization and both application servers as the signed-in Windows user,
# even when a different administrator approves the local firewall rule.
param([switch]$InteractiveSetup)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$server = (Resolve-Path -LiteralPath (Join-Path $repo 'build-lan-business/WmsServer.exe')).Path
$guide = '在主机打开「设置 → 网络和 Internet」，选择当前办公 Wi-Fi 或以太网，在其属性中将「网络配置文件类型」设为「专用网络」。'
while ($true) {
    $connected = @(Get-NetConnectionProfile -ErrorAction Stop | Where-Object {
        $_.IPv4Connectivity -ne 'Disconnected'
    })
    $profiles = @($connected | Where-Object { $_.NetworkCategory -in @('Private','DomainAuthenticated') })
    if ($profiles.Count) { break }
    if ($connected.Count) {
        foreach ($network in $connected) {
            Write-Host ("当前网络：{0}（{1}），类型：{2}" -f $network.Name,$network.InterfaceAlias,$network.NetworkCategory)
        }
        Write-Host '主机需要通过受信任的办公网络向第二台电脑提供服务。仅在确认是自己的办公网络后选择「专用」。'
        Write-Host $guide
    } else {
        Write-Host '未检测到已连接的 IPv4 网络。请将主机接入与第二台电脑相同的办公 Wi-Fi 或有线网络。'
    }
    Write-Host '如果本机是第二台电脑，请进入 client 文件夹，双击对应的「联网版」入口。'
    if (!$InteractiveSetup) { throw '局域网后台尚未启动。请连接办公网络后双击「启动主机后台.cmd」继续设置。' }
    $choice = Read-Host '输入 1 打开 Windows 网络设置，设置好后输入 2 重新检测，直接回车退出'
    if ($choice -eq '1') {
        try { Start-Process 'ms-settings:network-status' -ErrorAction Stop }
        catch { Write-Host '无法自动打开设置。请按 Win + I 手动进入「网络和 Internet」。' }
    } elseif ($choice -ne '2') {
        Write-Host '已退出主机启动。'
        return
    }
}

$rule = Get-NetFirewallRule -Name 'WMS-LAN-Business-Test' -PolicyStore ActiveStore -ErrorAction SilentlyContinue
$knownProgram = $false
if ($rule -and $rule.Enabled -eq 'True' -and $rule.Direction -eq 'Inbound' -and $rule.Action -eq 'Allow') {
    $filter = $rule | Get-NetFirewallApplicationFilter
    $ports = $rule | Get-NetFirewallPortFilter
    $clients = $rule | Get-NetFirewallAddressFilter
    $knownProgram = $filter.Program -eq $server -and $ports.Protocol -eq 'TCP' -and
        (@($ports.LocalPort | Sort-Object) -join ',') -eq '18080,18081' -and
        (@($clients.RemoteAddress) -join ',') -eq 'LocalSubnet' -and
        "$($rule.Profile)" -match 'Private' -and "$($rule.Profile)" -match 'Domain' -and "$($rule.Profile)" -notmatch 'Public'
}
if (!$knownProgram) {
    $configure = Join-Path $PSScriptRoot 'enable-lan-test.ps1'
    $process = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe') `
        -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"' + $configure + '"'),'-FirewallOnly') `
        -Verb RunAs -WindowStyle Hidden -PassThru
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw 'Windows 未能配置 WMS 局域网防火墙规则。请确认办公网络为专用或域网络，并请管理员检查防火墙策略。' }
}
$startScript = Join-Path $PSScriptRoot 'start-lan-dev.ps1'
foreach ($product in 'IceBeauty','MeiJingLing') { & $startScript -Product $product -AllowLan }
Write-Host ('Data folder: ' + (& (Join-Path $PSScriptRoot 'get-lan-data-root.ps1')))
foreach ($profile in $profiles) {
    Get-NetIPAddress -InterfaceIndex $profile.InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.AddressState -eq 'Preferred' -and $_.IPAddress -notlike '169.254.*' } |
        ForEach-Object { Write-Host ('第二台电脑的 client\server-address.txt 填写：' + $_.IPAddress + '  (' + $profile.InterfaceAlias + ')') }
}
