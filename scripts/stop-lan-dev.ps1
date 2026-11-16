param([ValidateSet('IceBeauty','MeiJingLing','All')][string]$Product='All', [switch]$StopDatabase)
$ErrorActionPreference='Stop'
if($StopDatabase -and $Product -ne 'All') { throw 'Use -Product All to stop the shared development database.' }
$repo=Split-Path -Parent $PSScriptRoot
$runtime=Join-Path $repo 'outputs/lan-dev'
$products=if($Product -eq 'All') { @('IceBeauty','MeiJingLing') } else { @($Product) }
foreach($item in $products) {
    $pidFile=Join-Path $runtime "$item-server.json"
    if(Test-Path -LiteralPath $pidFile) {
        $record=Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json
        $process=Get-Process -Id $record.pid -ErrorAction SilentlyContinue
        if($process -and $process.ProcessName -eq 'WmsServer' -and $process.Path.StartsWith($repo+'\', [StringComparison]::OrdinalIgnoreCase) -and $process.StartTime.ToUniversalTime() -eq ([datetime]$record.started).ToUniversalTime()) {
            Stop-Process -Id $process.Id
        }
        Remove-Item -LiteralPath $pidFile
    }
}
if($StopDatabase) {
    & (Join-Path $runtime 'postgres/pgsql/bin/pg_ctl.exe') -D (Join-Path $runtime 'pgdata') -m fast -w stop
    if($LASTEXITCODE -ne 0) { throw 'Database stop failed' }
}
