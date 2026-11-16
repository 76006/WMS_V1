param(
    [ValidateSet('IceBeauty','MeiJingLing')][string]$Product = 'IceBeauty',
    [string]$QtRoot = 'D:\Qt\6.5.3\mingw_64',
    [string]$PostgresRoot = '',
    [string]$BuildDirectory = 'build-lan',
    [switch]$AllowLan
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repo 'outputs/lan-dev'
if (!$PostgresRoot) { $PostgresRoot = Join-Path $runtime 'postgres/pgsql' }
$pgBin = Join-Path $PostgresRoot 'bin'
$serverExe = Join-Path (Join-Path $repo $BuildDirectory) 'WmsServer.exe'
if (!(Test-Path -LiteralPath (Join-Path $pgBin 'initdb.exe'))) { throw 'Provide extracted official PostgreSQL binaries with -PostgresRoot.' }
if (!(Test-Path -LiteralPath $serverExe)) { throw 'Build the WmsServer target first.' }
$env:PATH = "$pgBin;$(Join-Path $QtRoot 'bin');$env:PATH"
New-Item -ItemType Directory -Path $runtime -Force | Out-Null
$settingsPath = Join-Path $runtime 'credentials.json'
function New-LocalPassword {
    $bytes = New-Object byte[] 32
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    return ([BitConverter]::ToString($bytes)).Replace('-','').ToLowerInvariant()
}
if (!(Test-Path -LiteralPath $settingsPath)) {
    @{postgres=New-LocalPassword; app=New-LocalPassword; admin=New-LocalPassword} |
        ConvertTo-Json | Set-Content -LiteralPath $settingsPath -Encoding UTF8
}
$secrets = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
$pgData = Join-Path $runtime 'pgdata'
if (!(Test-Path -LiteralPath (Join-Path $pgData 'PG_VERSION'))) {
    $pwFile = Join-Path $runtime 'init-password.tmp'
    try {
        [IO.File]::WriteAllText($pwFile, $secrets.postgres + "`n", [Text.UTF8Encoding]::new($false))
        & (Join-Path $pgBin 'initdb.exe') -D $pgData -U postgres --encoding=UTF8 --locale=C --auth=scram-sha-256 --pwfile=$pwFile
        if ($LASTEXITCODE -ne 0) { throw 'initdb failed' }
    } finally { Remove-Item -LiteralPath $pwFile -ErrorAction SilentlyContinue }
}
& (Join-Path $pgBin 'pg_ctl.exe') -D $pgData status *> $null
if ($LASTEXITCODE -ne 0) {
    # PostgreSQL stays loopback-only; LAN clients talk exclusively to WmsServer.
    $pgArgs = "-D `"$pgData`" -l `"$(Join-Path $runtime 'postgres.log')`" -o `"-h 127.0.0.1 -p 55432`" -w start"
    $pgStart = Start-Process -FilePath (Join-Path $pgBin 'pg_ctl.exe') -ArgumentList $pgArgs -WindowStyle Hidden -PassThru
    if (!$pgStart.WaitForExit(30000)) { throw 'PostgreSQL startup timed out' }
    if ($pgStart.ExitCode -ne 0) { throw 'PostgreSQL start failed; see outputs/lan-dev/postgres.log' }
}
$dbName = if ($Product -eq 'IceBeauty') { 'wms_icebeauty_demo' } else { 'wms_meijingling_demo' }
$profile = if ($Product -eq 'IceBeauty') { 'ICE_BEAUTY' } else { 'MEI_JING_LING' }
$port = if ($Product -eq 'IceBeauty') { 18080 } else { 18081 }
$env:PGPASSWORD = $secrets.postgres
$psql = Join-Path $pgBin 'psql.exe'
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
$env:PGPASSWORD = $secrets.app
try {
    $schema = & $psql -X -h 127.0.0.1 -p 55432 -U $dbName -d $dbName -t -A -c "SELECT 1 FROM information_schema.schemata WHERE schema_name='wms_lan'"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect demo database' }
} finally { Remove-Item Env:PGPASSWORD -ErrorAction SilentlyContinue }
if (!$schema) {
    $env:WMS_DEMO_ADMIN_PASSWORD = $secrets.admin
    try {
        & $serverExe --product $profile --init-demo
        if ($LASTEXITCODE -ne 0) { throw 'Demo schema initialization failed' }
    } finally { Remove-Item Env:WMS_DEMO_ADMIN_PASSWORD -ErrorAction SilentlyContinue }
}
$pidFile = Join-Path $runtime "$Product-server.json"
if (Test-Path -LiteralPath $pidFile) {
    $previous = Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json
    $process = Get-Process -Id $previous.pid -ErrorAction SilentlyContinue
    if ($process -and $process.Path -eq $serverExe -and $process.StartTime.ToUniversalTime() -eq ([datetime]$previous.started).ToUniversalTime()) {
        Write-Output "$Product server is already running on port $port. Stop it before changing listen settings."
        return
    }
}
$portCheck = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $port)
try { $portCheck.Start() }
catch { throw "Port $port is occupied by another process. Stop that process before starting this development server." }
finally { $portCheck.Stop() }
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
Write-Output "$Product query pilot ready: http://127.0.0.1:$port"
Write-Output 'Login: admin. The generated test password is the admin value in outputs/lan-dev/credentials.json.'
Write-Output 'This development launcher does not install Windows services or open firewall ports.'
