param(
    [ValidateSet('IceBeauty','MeiJingLing')][string]$Product='IceBeauty',
    [string]$QtRoot='D:\Qt\6.5.3\mingw_64',
    [string]$ServerUrl='',
    [switch]$StartLocalServer
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if(!$ServerUrl) { $ServerUrl=if($Product -eq 'IceBeauty') {'http://127.0.0.1:18080'} else {'http://127.0.0.1:18081'} }
$packagedHost = Test-Path -LiteralPath (Join-Path $repo 'WMS-HOST.txt') -PathType Leaf
$executable=Join-Path $repo "client/$($Product)WmsLan.exe"
if (!$packagedHost) {
    # The workspace launcher and the Qt Creator Debug kit use the same target.
    $buildDir = Join-Path $repo 'build/Desktop_Qt_6_5_3_MinGW_64_bit-Debug'
    $cache = Join-Path $buildDir 'CMakeCache.txt'
    if (!(Test-Path -LiteralPath $cache)) { throw "Configure the Qt Creator Debug kit in $buildDir first." }
    $cmake = (Select-String -LiteralPath $cache -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$').Matches.Groups[1].Value
    $compiler = (Select-String -LiteralPath $cache -Pattern '^CMAKE_CXX_COMPILER:[^=]+=(.+)$').Matches.Groups[1].Value
    $env:PATH="$(Split-Path $compiler);$(Join-Path $QtRoot 'bin');$env:PATH"
    & $cmake --build $buildDir --target "$($Product)Wms" --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Could not update the desktop client. Save your work, close the old desktop window and retry.' }
    $executable=Join-Path $buildDir "$($Product)Wms.exe"
}
if (!(Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Desktop client missing: $executable" }
if ($StartLocalServer) {
    & (Join-Path $PSScriptRoot 'start-lan-dev.ps1') -Product $Product -QtRoot $QtRoot
}
try {
    if(([uri]$ServerUrl).Host -eq '127.0.0.1') {
        $dataRoot = & (Join-Path $PSScriptRoot 'get-lan-data-root.ps1')
        $secretPath = Join-Path $dataRoot 'credentials.json'
        if (Test-Path -LiteralPath $secretPath) {
            $localCredentials=Get-Content -LiteralPath $secretPath -Raw | ConvertFrom-Json
            $env:WMS_DEMO_LOGIN_PASSWORD=$localCredentials.admin
        }
    }
    # This is the interactive desktop explicitly requested by the user.
    Start-Process -FilePath $executable -ArgumentList @('--server',$ServerUrl) -WorkingDirectory (Split-Path $executable)
} finally { Remove-Item Env:WMS_DEMO_LOGIN_PASSWORD -ErrorAction SilentlyContinue }
