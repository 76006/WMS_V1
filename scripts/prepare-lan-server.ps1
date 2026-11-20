param([Parameter(Mandatory)][ValidateSet('ICE_BEAUTY','MEI_JING_LING')][string]$Product)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$repo = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repo 'outputs/lan-dev'
. (Join-Path $PSScriptRoot 'lan-data-storage.ps1')
$storageLock = Enter-LanRuntimeLock $runtime
try {
    $dataRoot = & (Join-Path $PSScriptRoot 'get-lan-data-root.ps1')
    Assert-LanStorageLayout $runtime $dataRoot
    $settingsPath = Join-Path $dataRoot 'credentials.json'
    $pgData = Join-Path $dataRoot 'pgdata'
    # Direct Debug launches only reuse an existing installation. Never initialize,
    # seed, move, or replace a database merely because the user pressed Run.
    if (!(Test-Path -LiteralPath (Join-Path $pgData 'PG_VERSION') -PathType Leaf) -or
        !(Test-Path -LiteralPath $settingsPath -PathType Leaf)) {
        throw "Existing PostgreSQL data/settings not found in $dataRoot. Use the host setup launcher for the first installation."
    }
    $pgBin = Join-Path $runtime 'postgres/pgsql/bin'
    foreach ($file in @('pg_ctl.exe','psql.exe','libpq.dll')) {
        if (!(Test-Path -LiteralPath (Join-Path $pgBin $file) -PathType Leaf)) {
            throw "PostgreSQL runtime file missing: $pgBin\$file"
        }
    }
    try { $secrets = Get-Content -LiteralPath $settingsPath -Raw -Encoding UTF8 | ConvertFrom-Json }
    catch { throw "Cannot read PostgreSQL settings in $settingsPath. Keep the original settings file." }
    if (!$secrets.app -or !$secrets.postgres) { throw "Incomplete PostgreSQL settings in $settingsPath." }
    $pgCtl = Join-Path $pgBin 'pg_ctl.exe'
    $statusText = & $pgCtl -D $pgData status 2>&1
    $statusCode = $LASTEXITCODE
    if ($statusCode -notin @(0,3)) { throw "Cannot inspect the existing PostgreSQL cluster in $pgData." }
    if ($statusCode -eq 3) {
        $occupied = @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -eq 55432)
        if ($occupied.Count) { throw 'Port 55432 belongs to another database process; the existing data directory was not started.' }
        $startText = & $pgCtl -D $pgData -l (Join-Path $runtime 'postgres.log') -o '-h 127.0.0.1 -p 55432' -w -t 15 start 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Existing PostgreSQL startup failed. See $runtime\postgres.log" }
    }
    $psql = Join-Path $pgBin 'psql.exe'
    Assert-LanDatabaseDirectory $psql $pgData $secrets.postgres
    $dbName = if ($Product -eq 'ICE_BEAUTY') { 'wms_icebeauty_business_demo' } else { 'wms_meijingling_business_demo' }
    $oldPassword = $env:PGPASSWORD
    $oldTimeout = $env:PGCONNECT_TIMEOUT
    $oldEncoding = $env:PGCLIENTENCODING
    try {
        $env:PGPASSWORD = $secrets.app
        $env:PGCONNECT_TIMEOUT = '5'
        $env:PGCLIENTENCODING = 'UTF8'
        $identity = & $psql -X -w -h 127.0.0.1 -p 55432 -U $dbName -d $dbName -v ON_ERROR_STOP=1 -At `
            -c 'SELECT product FROM wms_business.network_identity WHERE version=2' 2>&1
        if ($LASTEXITCODE -ne 0 -or @($identity).Count -ne 1 -or "$identity".Trim() -ne $Product) {
            throw "Cannot verify the existing $Product business database. No new database was created."
        }
    } finally {
        $env:PGPASSWORD = $oldPassword
        $env:PGCONNECT_TIMEOUT = $oldTimeout
        $env:PGCLIENTENCODING = $oldEncoding
    }
    # Private stdout pipe to WmsServer. Never log this JSON or put it in command arguments.
    @{product=$Product; host='127.0.0.1'; port=55432; database=$dbName; user=$dbName;
        password=$secrets.app; pgBin=$pgBin; dataRoot=$dataRoot} | ConvertTo-Json -Compress
} finally { $storageLock.Dispose() }
