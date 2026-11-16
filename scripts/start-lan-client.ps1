param(
    [ValidateSet('IceBeauty','MeiJingLing')][string]$Product='IceBeauty',
    [string]$QtRoot='D:\Qt\6.5.3\mingw_64',
    [string]$ServerUrl='',
    [switch]$StartLocalServer
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if($StartLocalServer) { & (Join-Path $PSScriptRoot 'start-lan-dev.ps1') -Product $Product -QtRoot $QtRoot }
if(!$ServerUrl) { $ServerUrl=if($Product -eq 'IceBeauty') {'http://127.0.0.1:18080'} else {'http://127.0.0.1:18081'} }
$executable=Join-Path $repo "build-lan-business/$($Product)Wms.exe"
if(!(Test-Path -LiteralPath $executable)) { throw 'Build the desktop targets in build-lan-business first.' }
$env:PATH="$(Join-Path $QtRoot 'bin');$env:PATH"
$secretPath=Join-Path $repo 'outputs/lan-dev/credentials.json'
try {
    if(([uri]$ServerUrl).Host -eq '127.0.0.1' -and (Test-Path -LiteralPath $secretPath)) {
        $localCredentials=Get-Content -LiteralPath $secretPath -Raw | ConvertFrom-Json
        $env:WMS_DEMO_LOGIN_PASSWORD=$localCredentials.admin
    }
    # This is the interactive desktop explicitly requested by the user.
    Start-Process -FilePath $executable -ArgumentList @('--server',$ServerUrl) -WorkingDirectory (Split-Path $executable)
} finally { Remove-Item Env:WMS_DEMO_LOGIN_PASSWORD -ErrorAction SilentlyContinue }
