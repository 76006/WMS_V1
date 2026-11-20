# Exercise the actual release entry points without changing the machine's network or starting services.
param([string]$Package = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist/WMS仓库系统.zip'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$tempRoot = (Resolve-Path -LiteralPath ([IO.Path]::GetTempPath())).Path.TrimEnd('\')
$fixture = Join-Path $tempRoot ('wms-network-' + [guid]::NewGuid().ToString('N'))
$wms = Join-Path $fixture '仓库 主机'

$simulation = @{}
function Check($condition, [string]$message) { if (!$condition) { throw $message } }
function Get-NetConnectionProfile {
    [pscustomobject]@{Name='Office'; InterfaceAlias='Ethernet'; NetworkCategory=$simulation.category
        InterfaceIndex=7; IPv4Connectivity=$simulation.connectivity}
}
function Get-NetFirewallRule {
    if ($simulation.ruleProfile) {
        [pscustomobject]@{Enabled='True'; Direction='Inbound'; Action='Allow'; Profile=$simulation.ruleProfile}
    }
}
function Get-NetFirewallApplicationFilter { [pscustomobject]@{Program=(Join-Path $wms 'build-lan-business\WmsServer.exe')} }
function Get-NetFirewallPortFilter { [pscustomobject]@{Protocol='TCP'; LocalPort=@('18080','18081')} }
function Get-NetFirewallAddressFilter { [pscustomobject]@{RemoteAddress='LocalSubnet'} }
function Get-NetIPAddress { [pscustomobject]@{IPAddress='192.0.2.42'; AddressState='Preferred'} }
function Read-Host {
    Check ($answers.Count -gt 0) 'Unexpected prompt'
    $answer = $answers.Dequeue()
    if ($answer -eq '2') { $simulation.category = 'Private'; $simulation.connectivity = 'LocalNetwork' }
    return $answer
}
function Start-Process($FilePath,$ArgumentList,$Verb,$WindowStyle,[switch]$PassThru) {
    if ($FilePath -eq 'ms-settings:network-status') { $simulation.settings++; return }
    if ($Verb -eq 'RunAs') {
        Check ($simulation.category -in @('Private','DomainAuthenticated')) 'Firewall setup on public profile'
        Check (($ArgumentList -join ' ') -match 'enable-lan-test.ps1.*-FirewallOnly') 'Wrong elevation request'
        $simulation.firewalls++
        $child = [pscustomobject]@{ExitCode=0}
        $child | Add-Member ScriptMethod WaitForExit {}
        return $child
    }
    $simulation.client = (@($FilePath) + $ArgumentList) -join ' '
}
try {
    $archive = [IO.Compression.ZipFile]::OpenRead($Package)
    try {
        foreach ($name in 'scripts/start-packaged-lan-host.ps1','启动主机后台.cmd','client/start-client.ps1','client/server-address.txt') {
            $path = Join-Path $wms $name
            [void][IO.Directory]::CreateDirectory((Split-Path -Parent $path))
            $member = "WMS/$name"
            $entry = $archive.GetEntry($member)
            if (!$entry) { $entry = $archive.GetEntry($member.Replace('/','\')) }
            Check $entry "Missing package file: $member"
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$path)
        }
    } finally { $archive.Dispose() }
    $launcher = Get-Content -LiteralPath (Join-Path $wms '启动主机后台.cmd') -Raw
    Check ($launcher -match 'start-packaged-lan-host.ps1" -InteractiveSetup') 'CMD skipped interactive setup'
    [void][IO.Directory]::CreateDirectory((Join-Path $wms 'build-lan-business'))
    [IO.File]::WriteAllText((Join-Path $wms 'build-lan-business/WmsServer.exe'),'')
    Set-Content (Join-Path $wms 'scripts/start-lan-dev.ps1') 'param($Product,[switch]$AllowLan); "$Product/$AllowLan"' -Encoding ASCII
    Set-Content (Join-Path $wms 'scripts/get-lan-data-root.ps1') '"test-data"' -Encoding ASCII
    $hostScript = Join-Path $wms 'scripts/start-packaged-lan-host.ps1'
    $answers = New-Object 'System.Collections.Generic.Queue[string]'
    $simulation.settings = 0
    $simulation.firewalls = 0
    $simulation.category = 'Public'
    $simulation.connectivity = 'LocalNetwork'
    $answers.Enqueue('')
    $result = @(& $hostScript -InteractiveSetup)
    Check ($result.Count -eq 0 -and $simulation.firewalls -eq 0) 'Cancelling started a service'
    $answers.Enqueue('1')
    $answers.Enqueue('2')
    $result = @(& $hostScript -InteractiveSetup)
    Check ($simulation.settings -eq 1 -and $simulation.firewalls -eq 1 -and ($result -join ',') -eq 'IceBeauty/True,MeiJingLing/True') 'Public to private retry failed'
    $simulation.category = 'DomainAuthenticated'
    $simulation.ruleProfile = 'Private'
    $result = @(& $hostScript)
    Check ($simulation.firewalls -eq 2 -and $result.Count -eq 2) 'Domain did not request a matching firewall rule'
    $simulation.ruleProfile = 'Domain, Private'
    $result = @(& $hostScript)
    Check ($simulation.firewalls -eq 2 -and $result.Count -eq 2) 'Matching rule unexpectedly needed elevation'
    $simulation.connectivity = 'Disconnected'
    $answers.Enqueue('')
    $result = @(& $hostScript -InteractiveSetup)
    Check ($result.Count -eq 0 -and $simulation.firewalls -eq 2) 'Disconnected adapter was accepted'

    $clientScript = Join-Path $wms 'client/start-client.ps1'
    $address = Join-Path $wms 'client/server-address.txt'
    [IO.File]::WriteAllText($address,'')
    $answers.Enqueue('127.0.0.1')
    $answers.Enqueue('192.0.2.42')
    & $clientScript -Product IceBeauty
    Check (([IO.File]::ReadAllText($address) -eq '192.0.2.42') -and $simulation.client.EndsWith('--server http://192.0.2.42:18080')) 'First client connection failed'
    & $clientScript -Product MeiJingLing
    Check ($simulation.client.EndsWith('--server http://192.0.2.42:18081') -and !$answers.Count) 'Saved client address was not reused'
    Write-Output 'PASS packaged host network retry, cancel, domain, disconnected, and both client entries.'
} finally {
    if (Test-Path -LiteralPath $fixture) {
        $resolved = (Resolve-Path -LiteralPath $fixture).Path
        if ((Split-Path $resolved) -ne $tempRoot -or (Split-Path $resolved -Leaf) -notmatch '^wms-network-[0-9a-f]{32}$') { throw "Unexpected test path: $resolved" }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
