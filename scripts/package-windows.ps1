param(
    [string]$QtRoot = 'C:\Qt\6.10.2\mingw_64',
    [string]$BuildDirectory = 'build-release',
    [string]$OutputDirectory = 'dist'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $projectRoot $BuildDirectory
$outputPath = Join-Path $projectRoot $OutputDirectory
$appPath = Join-Path $outputPath 'IceBeautyWms'
$zipPath = Join-Path $outputPath 'IceBeautyWms-Windows-x64.zip'
$setupPath = Join-Path $outputPath 'IceBeautyWms-Setup.exe'
$cmake = 'C:\Qt\Tools\CMake_64\bin\cmake.exe'
$mingwBin = 'C:\Qt\Tools\mingw1310_64\bin'
$env:Path = "$mingwBin;$QtRoot\bin;$env:Path"

& $cmake -S $projectRoot -B $buildPath -G 'MinGW Makefiles' `
    "-DCMAKE_PREFIX_PATH=$QtRoot" '-DCMAKE_BUILD_TYPE=Release' '-DBUILD_TESTING=OFF'
if ($LASTEXITCODE -ne 0) { throw 'Release 配置失败。' }
& $cmake --build $buildPath -j 4
if ($LASTEXITCODE -ne 0) { throw 'Release 构建失败。' }

if (Test-Path -LiteralPath $appPath) { Remove-Item -LiteralPath $appPath -Recurse -Force }
New-Item -ItemType Directory -Path $appPath -Force | Out-Null
& $cmake --install $buildPath --prefix $appPath
if ($LASTEXITCODE -ne 0) { throw '安装文件整理失败。' }
& "$QtRoot\bin\windeployqt.exe" --release --compiler-runtime --no-opengl-sw $appPath\IceBeautyWms.exe
if ($LASTEXITCODE -ne 0) { throw 'Qt 运行依赖部署失败。' }
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $appPath
Copy-Item -LiteralPath (Join-Path $projectRoot 'DEVELOPMENT_STATUS.md') -Destination $appPath

if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path "$appPath\*" -DestinationPath $zipPath -CompressionLevel Optimal

$stagingPath = Join-Path $outputPath 'installer-staging'
if (Test-Path -LiteralPath $stagingPath) { Remove-Item -LiteralPath $stagingPath -Recurse -Force }
New-Item -ItemType Directory -Path $stagingPath -Force | Out-Null
Copy-Item -LiteralPath $zipPath -Destination (Join-Path $stagingPath 'IceBeautyWms.zip')
$installScript = @'
@echo off
setlocal
set "TARGET=%LOCALAPPDATA%\Programs\IceBeautyWms"
if not exist "%TARGET%" mkdir "%TARGET%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath '%~dp0IceBeautyWms.zip' -DestinationPath '%TARGET%' -Force; $ws=New-Object -ComObject WScript.Shell; $sc=$ws.CreateShortcut([Environment]::GetFolderPath('Desktop')+'\IceBeautyWms.lnk'); $sc.TargetPath='%TARGET%\IceBeautyWms.exe'; $sc.WorkingDirectory='%TARGET%'; $sc.Save()"
if errorlevel 1 exit /b 1
start "" "%TARGET%\IceBeautyWms.exe"
endlocal
'@
Set-Content -LiteralPath (Join-Path $stagingPath 'install.cmd') -Value $installScript -Encoding ASCII

$sedPath = Join-Path $stagingPath 'IceBeautyWms.sed'
$sed = @"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=1
HideExtractAnimation=0
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=%InstallPrompt%
DisplayLicense=%DisplayLicense%
FinishMessage=%FinishMessage%
TargetName=$setupPath
FriendlyName=冰美肌库存管理安装程序
AppLaunched=install.cmd
PostInstallCmd=<None>
AdminQuietInstCmd=
UserQuietInstCmd=
SourceFiles=SourceFiles
[Strings]
InstallPrompt=是否安装冰美肌库存管理？
DisplayLicense=
FinishMessage=安装完成。
FILE0=IceBeautyWms.zip
FILE1=install.cmd
[SourceFiles]
SourceFiles0=$stagingPath\
[SourceFiles0]
%FILE0%=
%FILE1%=
"@
Set-Content -LiteralPath $sedPath -Value $sed -Encoding Unicode
if (Test-Path -LiteralPath $setupPath) { Remove-Item -LiteralPath $setupPath -Force }
$iexpress = Start-Process -FilePath "$env:SystemRoot\System32\iexpress.exe" `
    -ArgumentList @('/N', '/Q', $sedPath) -Wait -PassThru -WindowStyle Hidden
if ($iexpress.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $setupPath)) {
    throw 'Windows 安装程序生成失败。'
}
Remove-Item -LiteralPath $stagingPath -Recurse -Force

Write-Host "发布目录: $appPath"
Write-Host "免安装包: $zipPath"
Write-Host "安装程序: $setupPath"
