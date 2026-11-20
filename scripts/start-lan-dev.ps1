param(
    [ValidateSet('IceBeauty','MeiJingLing')][string]$Product = 'IceBeauty',
    [string]$QtRoot = 'D:\Qt\6.5.3\mingw_64',
    [string]$PostgresRoot = '',
    [string]$BuildDirectory = 'build-lan-business',
    [switch]$AllowLan
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repo 'outputs/lan-dev'
$packagedHost = Test-Path -LiteralPath (Join-Path $repo 'WMS-HOST.txt') -PathType Leaf
. (Join-Path $PSScriptRoot 'lan-data-storage.ps1')
$storageLock = Enter-LanRuntimeLock $runtime
$startupLock = $null
try {
$dataRoot = & (Join-Path $PSScriptRoot 'get-lan-data-root.ps1')
$settingsPath = Join-Path $dataRoot 'credentials.json'
$pgData = Join-Path $dataRoot 'pgdata'
if (!$PostgresRoot) { $PostgresRoot = Join-Path $runtime 'postgres/pgsql' }
$pgBin = Join-Path $PostgresRoot 'bin'
$psql = Join-Path $pgBin 'psql.exe'
$serverExe = Join-Path (Join-Path $repo $BuildDirectory) 'WmsServer.exe'
$profile = if ($Product -eq 'IceBeauty') { 'ICE_BEAUTY' } else { 'MEI_JING_LING' }
$port = if ($Product -eq 'IceBeauty') { 18080 } else { 18081 }
$pidFile = Join-Path $runtime "$Product-server.json"

# Check before attempting to start PostgreSQL. An ordinary desktop
# user may see an elevated WmsServer's PID and start time but not its executable path.
$listener = Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue |
    Where-Object { $_.LocalAddress -in @('127.0.0.1','0.0.0.0') } | Select-Object -First 1
if ($listener) {
    Assert-LanStorageLayout $runtime $dataRoot
    if (Test-Path -LiteralPath $pidFile) {
        $record = Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json
        $process = Get-Process -Id $listener.OwningProcess -ErrorAction SilentlyContinue
        if ($process -and $process.Id -eq $record.pid -and $process.ProcessName -eq 'WmsServer' -and
            $process.StartTime -and $process.StartTime.ToUniversalTime() -eq ([datetime]$record.started).ToUniversalTime() -and
            (!$process.Path -or $process.Path -eq $serverExe)) {
            $health = Invoke-RestMethod -Uri "http://127.0.0.1:$port/health" -TimeoutSec 3 -MaximumRedirection 0
            if ($health.status -ne 'ok' -or $health.product -ne $profile) {
                throw "The running server on port $port did not pass the $Product health check."
            }
            if ($AllowLan -and $listener.LocalAddress -eq '127.0.0.1') {
                throw "$Product is running in local mode. Use the host LAN setup launcher to switch to LAN mode."
            }
            $secrets = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
            Assert-LanDatabaseDirectory $psql $pgData $secrets.postgres
            Write-Output "$Product server is already running on port $port."
            return
        }
    }
    $conflict = Get-Process -Id $listener.OwningProcess -ErrorAction SilentlyContinue
    $conflictPath = if ($conflict.Path) { $conflict.Path } else { '(path unavailable)' }
    throw "Port $port is occupied by $($conflict.ProcessName) (PID $($listener.OwningProcess)): $conflictPath. Close old WMS listeners with: powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\stop-wms-listeners.ps1`" -Product $Product. Then start this build again."
}
$portCheck = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $port)
try { $portCheck.Start() }
catch { throw "Port $port is unavailable: $($_.Exception.Message)" }
finally { $portCheck.Stop() }

if (!(Test-Path -LiteralPath (Join-Path $pgBin 'initdb.exe'))) { throw 'Provide extracted official PostgreSQL binaries with -PostgresRoot.' }
if (!(Test-Path -LiteralPath $serverExe)) { throw 'Build the WmsServer target first.' }
if (!$packagedHost -and !(Test-Path -LiteralPath (Join-Path (Split-Path $serverExe) 'Qt6Core.dll'))) {
    $env:PATH = "$(Join-Path $QtRoot 'bin');$env:PATH"
}
$env:PATH = "$pgBin;$env:PATH"
$env:WMS_PG_BIN = $pgBin
[void][IO.Directory]::CreateDirectory($dataRoot)
# Launchers in different unpacked copies may use the same Documents database.
try {
    $startupLock = [IO.File]::Open((Join-Path $dataRoot '.wms-start.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
} catch [IO.IOException] { throw "无法锁定数据目录 $dataRoot。若另一个后台正在启动，请等待其完成后重试。$($_.Exception.Message)" }
Assert-LanStorageLayout $runtime $dataRoot -ConfirmFirstInstall
function New-LocalPassword {
    $bytes = New-Object byte[] 32
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    return ([BitConverter]::ToString($bytes)).Replace('-','').ToLowerInvariant()
}
if (!(Test-Path -LiteralPath $settingsPath)) {
    $tempSettings = Join-Path $dataRoot ('credentials-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        @{postgres=New-LocalPassword; app=New-LocalPassword; admin='123456'} |
            ConvertTo-Json | Set-Content -LiteralPath $tempSettings -Encoding UTF8
        [IO.File]::Move($tempSettings,$settingsPath)
    } finally { if (Test-Path -LiteralPath $tempSettings) { Remove-Item -LiteralPath $tempSettings -Force } }
}
try { $secrets = [IO.File]::ReadAllText($settingsPath) | ConvertFrom-Json }
catch { throw "无法读取凭据文件 $settingsPath，请保留文件并核对原备份。" }
if (!$secrets.postgres -or !$secrets.app -or !$secrets.admin) { throw "凭据文件不完整：$settingsPath。请保留原文件。" }
if (!(Test-Path -LiteralPath (Join-Path $pgData 'PG_VERSION'))) {
    $pwFile = Join-Path $dataRoot 'init-password.tmp'
    try {
        [IO.File]::WriteAllText($pwFile, $secrets.postgres + "`n", [Text.UTF8Encoding]::new($false))
        Initialize-LanCluster $pgBin $pgData $pwFile
    } finally { Remove-Item -LiteralPath $pwFile -ErrorAction SilentlyContinue }
}
$pgStatus = Start-Process -FilePath (Join-Path $pgBin 'pg_ctl.exe') -ArgumentList @('-D',('"' + $pgData + '"'),'status') -WindowStyle Hidden -PassThru
if (!$pgStatus.WaitForExit(10000)) { throw 'Could not check PostgreSQL status in time.' }
if ($pgStatus.ExitCode -notin @(0,3)) { throw "Could not inspect the PostgreSQL cluster in $pgData (exit $($pgStatus.ExitCode))." }
if ($pgStatus.ExitCode -eq 3) {
    # PostgreSQL stays loopback-only; LAN clients talk exclusively to WmsServer.
    $pgArgs = "-D `"$pgData`" -l `"$(Join-Path $runtime 'postgres.log')`" -o `"-h 127.0.0.1 -p 55432`" -w start"
    $pgStart = Start-Process -FilePath (Join-Path $pgBin 'pg_ctl.exe') -ArgumentList $pgArgs -WindowStyle Hidden -PassThru
    if (!$pgStart.WaitForExit(30000)) { throw 'PostgreSQL startup timed out' }
    if ($pgStart.ExitCode -ne 0) { throw 'PostgreSQL start failed; see outputs/lan-dev/postgres.log' }
}
$dbName = if ($Product -eq 'IceBeauty') { 'wms_icebeauty_business_demo' } else { 'wms_meijingling_business_demo' }
Assert-LanDatabaseDirectory $psql $pgData $secrets.postgres
$locator = & (Join-Path $PSScriptRoot 'get-lan-data-root.ps1') -LocatorPath
if ($dataRoot -ne $runtime -and !(Test-Path -LiteralPath $locator)) {
    [IO.File]::WriteAllText($locator, $dataRoot, [Text.UTF8Encoding]::new($false))
}
$env:PGPASSWORD = $secrets.postgres
function Invoke-AdminSql([string]$sql) {
    $result = $sql | & $psql -X -h 127.0.0.1 -p 55432 -U postgres -d postgres -v ON_ERROR_STOP=1 -t -A
    if ($LASTEXITCODE -ne 0) { throw 'PostgreSQL administration failed' }
    return $result
}
try {
    if (!(Invoke-AdminSql "SELECT 1 FROM pg_roles WHERE rolname='$dbName'")) {
        Invoke-AdminSql "CREATE ROLE $dbName LOGIN PASSWORD '$($secrets.app)'" | Out-Null
    }
    if (!(Invoke-AdminSql "SELECT 1 FROM pg_database WHERE datname='$dbName'")) {
        Invoke-AdminSql "CREATE DATABASE $dbName OWNER $dbName" | Out-Null
    }
} finally { Remove-Item Env:PGPASSWORD -ErrorAction SilentlyContinue }
$env:WMS_PG_HOST = '127.0.0.1'
$env:WMS_PG_PORT = '55432'
$env:WMS_PG_DATABASE = $dbName
$env:WMS_PG_USER = $dbName
$env:WMS_PG_PASSWORD = $secrets.app
$env:WMS_LAN_TEST_PASSWORDS = '1'
$env:PGPASSWORD = $secrets.app
try {
    $schema = & $psql -X -h 127.0.0.1 -p 55432 -U $dbName -d $dbName -t -A -c "SELECT 1 FROM information_schema.schemata WHERE schema_name='wms_lan'"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect demo database' }
} finally { Remove-Item Env:PGPASSWORD -ErrorAction SilentlyContinue }
if (!$schema) {
    $env:WMS_DEMO_ADMIN_PASSWORD = $secrets.admin
    try {
        $initialize = if ($packagedHost) { '--init-lan-empty' } else { '--init-demo' }
        & $serverExe --product $profile $initialize
        if ($LASTEXITCODE -ne 0) { throw 'Demo schema initialization failed' }
    } finally { Remove-Item Env:WMS_DEMO_ADMIN_PASSWORD -ErrorAction SilentlyContinue }
}
$env:PGPASSWORD = $secrets.app
try {
    $businessSchema = & $psql -X -h 127.0.0.1 -p 55432 -U $dbName -d $dbName -t -A -c "SELECT 1 FROM information_schema.schemata WHERE schema_name='wms_business'"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect business database' }
} finally { Remove-Item Env:PGPASSWORD -ErrorAction SilentlyContinue }
if (!$businessSchema) {
    $env:WMS_DEMO_ADMIN_PASSWORD = $secrets.admin
    try {
        & $serverExe --product $profile --init-business
        if ($LASTEXITCODE -ne 0) { throw 'Business schema initialization failed' }
    } finally { Remove-Item Env:WMS_DEMO_ADMIN_PASSWORD -ErrorAction SilentlyContinue }
}
$listen = if ($AllowLan) { '0.0.0.0' } else { '127.0.0.1' }
$arguments = @('--product',$profile,'--port',"$port",'--listen',$listen)
if ($AllowLan) { $arguments += '--allow-insecure-lan' }
$server = Start-Process -FilePath $serverExe -ArgumentList $arguments -WorkingDirectory (Split-Path $serverExe) -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $runtime "$Product-server.stdout.log") -RedirectStandardError (Join-Path $runtime "$Product-server.stderr.log")
@{pid=$server.Id;started=$server.StartTime.ToUniversalTime().ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath $pidFile -Encoding UTF8
for ($attempt=0; $attempt -lt 30; $attempt++) {
    $server.Refresh()
    if ($server.HasExited) { throw "Server exited; see outputs/lan-dev/$Product-server.stderr.log" }
    try {
        $health = Invoke-RestMethod -Uri "http://127.0.0.1:$port/health" -TimeoutSec 2
        if ($health.product -eq $profile) { break }
    } catch { Start-Sleep -Milliseconds 300 }
}
$server.Refresh()
if ($server.HasExited) { throw "Server exited; see outputs/lan-dev/$Product-server.stderr.log" }
if (!$health -or $health.product -ne $profile) { throw 'Server health check failed' }
Write-Output "$Product full business server ready: http://127.0.0.1:$port"
Write-Output 'Test login: admin. The default password for a new test installation is 123456.'
Write-Output 'This development launcher does not install Windows services or open firewall ports.'
} finally {
    if ($startupLock) { $startupLock.Dispose() }
    $storageLock.Dispose()
}
