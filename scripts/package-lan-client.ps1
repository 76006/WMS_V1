param(
    [Parameter(Mandatory=$true)][string]$ServerAddress,
    [string]$QtRoot='D:\Qt\6.5.3\mingw_64',
    [string]$MingwBin='D:\Qt\Tools\mingw1120_64\bin',
    [string]$BuildDirectory='build-lan-business'
)
$ErrorActionPreference='Stop'
$parsedAddress=$null
if(![Net.IPAddress]::TryParse($ServerAddress,[ref]$parsedAddress) -or
    $parsedAddress.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
    [Net.IPAddress]::IsLoopback($parsedAddress)) { throw 'Specify the host LAN IPv4 address.' }
$repo=Split-Path -Parent $PSScriptRoot
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$package=Join-Path $repo "outputs/WMS-LAN-Client-$stamp"
$zip="$package.zip"
if(Test-Path -LiteralPath $package) { throw 'Package output already exists.' }
New-Item -ItemType Directory -Path $package | Out-Null
$savedPath=$env:PATH
try {
    $env:PATH="$MingwBin;$QtRoot\bin;$savedPath"
    foreach($app in @('IceBeautyWms','MeiJingLingWms')) {
        $source=Join-Path $repo "$BuildDirectory/$app.exe"
        if(!(Test-Path -LiteralPath $source)) { throw "Missing build: $source" }
        $target=Join-Path $package "$app.exe"
        Copy-Item -LiteralPath $source -Destination $target
        # Let Qt inspect the original build before stripping its debug symbols.
        & (Join-Path $QtRoot 'bin/windeployqt.exe') --compiler-runtime `
            --qtpaths (Join-Path $QtRoot 'bin/qtpaths6.exe') `
            --qmldir (Join-Path $repo 'resources') --dir $package $target
        if($LASTEXITCODE -ne 0) { throw 'Qt dependency deployment failed.' }
        & (Join-Path $MingwBin 'strip.exe') --strip-debug $target
        if($LASTEXITCODE -ne 0) { throw 'Could not strip distribution copy.' }
    }
} finally { $env:PATH=$savedPath }
foreach($folder in @('仓库系统表单模板','美鲸灵仓库系统表单模板')) {
    Copy-Item -LiteralPath (Join-Path $repo $folder) -Destination $package -Recurse
}
[IO.File]::WriteAllText((Join-Path $package 'server-address.txt'),$ServerAddress,[Text.Encoding]::ASCII)
$launcher=@'
param([ValidateSet('IceBeauty','MeiJingLing')][string]$Product='IceBeauty')
$ErrorActionPreference='Stop'
$server=(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'server-address.txt') -Raw).Trim()
$address=$null
if(![Net.IPAddress]::TryParse($server,[ref]$address) -or $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
    throw 'Enter the host IPv4 address in server-address.txt.'
}
$port=if($Product -eq 'IceBeauty'){18080}else{18081}
Remove-Item Env:WMS_DEMO_LOGIN_PASSWORD -ErrorAction SilentlyContinue
Start-Process -FilePath (Join-Path $PSScriptRoot "$($Product)Wms.exe") -ArgumentList @('--server',"http://${server}:$port") -WorkingDirectory $PSScriptRoot
'@
Set-Content -LiteralPath (Join-Path $package 'start-client.ps1') -Value $launcher -Encoding UTF8
foreach($item in @(@('冰美肌','IceBeauty'),@('美鲸灵','MeiJingLing'))) {
    $cmd="@echo off`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"%~dp0start-client.ps1`" -Product $($item[1])`r`nif errorlevel 1 pause`r`n"
    [IO.File]::WriteAllText((Join-Path $package "启动$($item[0])联网版.cmd"),$cmd,[Text.Encoding]::ASCII)
}
$instructions=@"
第二台电脑使用方法（Windows 64 位）

1. 将整个压缩包复制到第二台电脑并全部解压，不能只复制 EXE，也不要在压缩包里直接运行。
2. 两台电脑连接同一公司局域网；主机先开启局域网测试后台并放行端口。
3. 双击“启动冰美肌联网版.cmd”或“启动美鲸灵联网版.cmd”。
4. 主机地址已填写为 $ServerAddress，冰美肌端口 18080，美鲸灵端口 18081。
5. 使用主机软件中创建的业务账号登录。此客户端包不包含密码和数据库。
6. 一台电脑录入测试入库，另一台点击“刷新”，核对库存及单据是否一致。

主机 IP 改变后，用记事本修改 server-address.txt。不要填写 127.0.0.1，那表示第二台电脑自己。
不需要安装 Qt、PostgreSQL 或 Git。生成旧版 .xls 表单仍需 WPS 表格或 Microsoft Excel。
主机必须开机，后台必须运行；数据统一保存在主机的 PostgreSQL 测试库。
这是隔离测试库，尚未迁移正式单机数据。当前 HTTP 联调仅用于受信任公司局域网。

连接不通时，先在第二台电脑浏览器打开 http://${ServerAddress}:18080/health
美鲸灵对应 http://${ServerAddress}:18081/health
无法打开时检查主机后台、IP、Windows 防火墙、两机是否同网以及访客 Wi-Fi 隔离。
"@
Set-Content -LiteralPath (Join-Path $package '第二台电脑使用说明.txt') -Value $instructions -Encoding UTF8
Set-Content -LiteralPath (Join-Path $package 'qt.conf') -Value "[Paths]`nPrefix=.`nQmlImports=qml" -Encoding ASCII
foreach($file in @('platforms/qwindows.dll','Qt6Core.dll','libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
    if(!(Test-Path -LiteralPath (Join-Path $package $file))) { throw "Missing dependency: $file" }
}
Compress-Archive -LiteralPath $package -DestinationPath $zip -CompressionLevel Optimal
Write-Output "CLIENT_DIRECTORY=$package"
Write-Output "CLIENT_ZIP=$zip"
