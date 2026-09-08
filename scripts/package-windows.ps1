param(
    [string]$QtRoot = '',
    [string]$BuildDirectory = 'build-release',
    [string]$OutputDirectory = 'dist',
    [switch]$ZipOnly
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$qtInstallRoots = @('D:\Qt', 'C:\Qt')
if ([string]::IsNullOrWhiteSpace($QtRoot)) {
    $qtCandidates = foreach ($root in $qtInstallRoots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^6\.' } |
            ForEach-Object {
                $candidate = Join-Path $_.FullName 'mingw_64'
                if ((Test-Path -LiteralPath (Join-Path $candidate 'bin\windeployqt.exe')) -and
                    (Test-Path -LiteralPath (Join-Path $candidate 'mkspecs\qconfig.pri'))) {
                    [PSCustomObject]@{ Path = $candidate; Version = [version]$_.Name }
                }
            }
    }
    $selectedQt = $qtCandidates | Sort-Object Version -Descending | Select-Object -First 1
    if (-not $selectedQt) { throw '未找到 Qt 6 MinGW 64 位环境，请通过 -QtRoot 指定路径。' }
    $QtRoot = $selectedQt.Path
}
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$qconfigPath = Join-Path $QtRoot 'mkspecs\qconfig.pri'
if (-not (Test-Path -LiteralPath $qconfigPath)) { throw "Qt 配置不存在: $qconfigPath" }
$qconfig = Get-Content -LiteralPath $qconfigPath
$gccMajor = [regex]::Match(($qconfig -join "`n"), 'QT_GCC_MAJOR_VERSION\s*=\s*(\d+)').Groups[1].Value
$gccMinor = [regex]::Match(($qconfig -join "`n"), 'QT_GCC_MINOR_VERSION\s*=\s*(\d+)').Groups[1].Value
if (-not $gccMajor -or -not $gccMinor) { throw '无法从 Qt 配置识别匹配的 MinGW 版本。' }
$qtBase = Split-Path -Parent (Split-Path -Parent $QtRoot)
$mingwDirectory = Get-ChildItem -LiteralPath (Join-Path $qtBase 'Tools') -Directory `
    -Filter "mingw$gccMajor$gccMinor*_64" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $mingwDirectory) { throw "未找到与 Qt 匹配的 MinGW $gccMajor.$gccMinor 工具链。" }
$cmakePath = Join-Path $qtBase 'Tools\CMake_64\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmakePath)) {
    $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if (-not $cmakeCommand) { throw '未找到 CMake。' }
    $cmakePath = $cmakeCommand.Source
}
$buildPath = Join-Path $projectRoot $BuildDirectory
$outputPath = Join-Path $projectRoot $OutputDirectory
$appPath = Join-Path $outputPath 'IceBeautyWms'
$zipPath = Join-Path $outputPath 'IceBeautyWms-Windows-x64.zip'
$setupPath = Join-Path $outputPath 'IceBeautyWms-Setup.exe'
$cmake = $cmakePath
$mingwBin = Join-Path $mingwDirectory.FullName 'bin'
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
& "$QtRoot\bin\windeployqt.exe" --compiler-runtime --no-opengl-sw `
    --qtpaths "$QtRoot\bin\qtpaths6.exe" $appPath\IceBeautyWms.exe
if ($LASTEXITCODE -ne 0) { throw 'Qt 运行依赖部署失败。' }
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $appPath
Copy-Item -LiteralPath (Join-Path $projectRoot 'DEVELOPMENT_STATUS.md') -Destination $appPath

$requiredFiles = @(
    (Join-Path $appPath 'IceBeautyWms.exe'),
    (Join-Path $appPath 'platforms\qwindows.dll'),
    (Join-Path $appPath 'sqldrivers\qsqlite.dll')
)
foreach ($requiredFile in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile)) {
        throw "发布包缺少运行依赖: $requiredFile"
    }
}
$smokeDatabase = Join-Path $outputPath "package-smoke-$PID.db"
$savedPath = $env:Path
try {
    $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
    $smoke = Start-Process -FilePath (Join-Path $appPath 'IceBeautyWms.exe') `
        -ArgumentList @('--smoke-test', '--database', $smokeDatabase) -WorkingDirectory $appPath `
        -Wait -PassThru -WindowStyle Hidden
    if ($smoke.ExitCode -ne 0) { throw "发布版独立启动检查失败，退出码: $($smoke.ExitCode)" }
} finally {
    $env:Path = $savedPath
    foreach ($databaseFile in @($smokeDatabase, "$smokeDatabase-wal", "$smokeDatabase-shm")) {
        if (Test-Path -LiteralPath $databaseFile) { Remove-Item -LiteralPath $databaseFile -Force }
    }
}

if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path "$appPath\*" -DestinationPath $zipPath -CompressionLevel Optimal

if ($ZipOnly) {
    Write-Host "发布目录: $appPath"
    Write-Host "免安装包: $zipPath"
    return
}

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
