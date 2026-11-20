param([ValidateSet('IceBeauty','MeiJingLing','All')][string]$Product='All', [switch]$StopDatabase)
$ErrorActionPreference='Stop'
if($StopDatabase -and $Product -ne 'All') { throw 'Use -Product All to stop the shared development database.' }
$repo=Split-Path -Parent $PSScriptRoot
$runtime=Join-Path $repo 'outputs/lan-dev'
. (Join-Path $PSScriptRoot 'lan-data-storage.ps1')
$storageLock = Enter-LanRuntimeLock $runtime
try {
if ($StopDatabase) {
    $dataRoot = & (Join-Path $PSScriptRoot 'get-lan-data-root.ps1')
    Assert-LanStorageLayout $runtime $dataRoot
    $pgData = Join-Path $dataRoot 'pgdata'
}
$products=if($Product -eq 'All') { @('IceBeauty','MeiJingLing') } else { @($Product) }
# Find listeners across old package/build directories, even when a PID file is absent.
& (Join-Path $PSScriptRoot 'stop-wms-listeners.ps1') -Product $Product
foreach($item in $products) {
    $pidFile=Join-Path $runtime "$item-server.json"
    if(Test-Path -LiteralPath $pidFile) {
        try { $record=Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json }
        catch { Write-Warning "Invalid old PID record left in place: $pidFile"; continue }
        $process=Get-Process -Id $record.pid -ErrorAction SilentlyContinue
        # Do not discard the only record of a still-running process.
        if (!$process -or ($process.StartTime -and $process.StartTime.ToUniversalTime() -ne ([datetime]$record.started).ToUniversalTime())) {
            Remove-Item -LiteralPath $pidFile
        }
    }
}
if($StopDatabase) {
    & (Join-Path $runtime 'postgres/pgsql/bin/pg_ctl.exe') -D $pgData -m fast -w stop
    if($LASTEXITCODE -ne 0) { throw 'Database stop failed' }
}
} finally { $storageLock.Dispose() }
