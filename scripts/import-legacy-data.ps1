[CmdletBinding()]
param(
    [ValidateSet('IceBeauty','MeiJingLing')][string]$Product,
    [string]$SourceDatabase,
    [ValidateSet('Empty','Replace','Merge')][string]$Mode,
    [switch]$ConfirmImport
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repo 'outputs/lan-dev'
$server = Join-Path $repo 'build-lan-business/WmsServer.exe'
$pgBin = Join-Path $runtime 'postgres/pgsql/bin'
$psql = Join-Path $pgBin 'psql.exe'
. (Join-Path $PSScriptRoot 'lan-data-storage.ps1')

if ($ConfirmImport -and (!$Product -or !$SourceDatabase)) {
    throw '非交互导入必须同时指定 -Product 和 -SourceDatabase。'
}
if (!$Product) {
    switch (Read-Host '导入至哪个系统？输入 1 选择冰美肌，输入 2 选择美鲸灵，回车取消') {
        '1' { $Product = 'IceBeauty' }
        '2' { $Product = 'MeiJingLing' }
        '' { return }
        default { throw '请输入 1 或 2。' }
    }
}
if (!$SourceDatabase) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object Windows.Forms.OpenFileDialog
    try {
        $dialog.Title = '选择从旧单机版备份的 SQLite 主数据库文件'
        $dialog.Filter = 'SQLite 数据库 (*.db;*.sqlite;*.sqlite3)|*.db;*.sqlite;*.sqlite3|所有文件 (*.*)|*.*'
        $dialog.RestoreDirectory = $true
        if ($dialog.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { return }
        $SourceDatabase = $dialog.FileName
    } finally { $dialog.Dispose() }
}
$source = Get-Item -LiteralPath $SourceDatabase -ErrorAction Stop
if ($source.PSIsContainer -or $source.Name -match '-(wal|shm|journal)$') { throw '请选择 SQLite 主数据库文件。' }
$productName = if ($Product -eq 'IceBeauty') { '冰美肌' } else { '美鲸灵' }
$productCode = if ($Product -eq 'IceBeauty') { 'ICE_BEAUTY' } else { 'MEI_JING_LING' }
$database = if ($Product -eq 'IceBeauty') { 'wms_icebeauty_business_demo' } else { 'wms_meijingling_business_demo' }
$servicePort = if ($Product -eq 'IceBeauty') { 18080 } else { 18081 }
$dataRoot = & (Join-Path $PSScriptRoot 'get-lan-data-root.ps1')
$pgData = Join-Path $dataRoot 'pgdata'
if (!(Test-Path -LiteralPath (Join-Path $pgData 'postmaster.pid') -PathType Leaf)) {
    throw '请先运行「启动主机后台.cmd」，等两套后台就绪后再导入。'
}
Write-Host "源文件：$($source.FullName)"
Write-Host "目标系统：$productName    目标库：$database"
Write-Host "主机数据库目录：$pgData"
if (!$Mode) {
    if ($ConfirmImport) { $Mode = 'Empty' }
    else {
        Write-Host '1 导入空库   2 合并（保留双方数据，列出冲突供确认）   3 整库替换（覆盖所选产品）'
        switch (Read-Host '选择方式，回车取消') {
            '1' { $Mode = 'Empty' }
            '2' { $Mode = 'Merge' }
            '3' { $Mode = 'Replace' }
            '' { return }
            default { throw '请输入 1、2 或 3。' }
        }
    }
}
$operation = switch ($Mode) {
    Empty { '导入空库' }
    Merge { '合并旧库和目标库' }
    Replace { '整库替换：目标产品现有的账号、库存、单据及附件将被旧库覆盖' }
}
Write-Host "$productName — $operation"
if (!$ConfirmImport) {
    Write-Host "请保存单据并退出该产品的所有联网前台。导入时将暂停$productName 后台。"
    if ((Read-Host '确认上面显示的目标和导入方式？输入 YES 继续，回车取消') -ne 'YES') { return }
}
$lock = [IO.File]::Open((Join-Path $runtime 'move-to-documents.lock'),'OpenOrCreate','ReadWrite','None')
$restartServer = $false
$restartLan = $false
$importError = $null
$restartError = $null
$savedEnv = @{}
foreach ($key in 'PATH','PGPASSWORD','WMS_PG_HOST','WMS_PG_PORT','WMS_PG_DATABASE','WMS_PG_USER','WMS_PG_PASSWORD','WMS_LAN_TEST_PASSWORDS','WMS_DEMO_ADMIN_PASSWORD') {
    $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key,'Process')
}
try {
    Assert-LanStorageLayout $runtime $dataRoot
    $credentials = Get-Content -LiteralPath (Join-Path $dataRoot 'credentials.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-LanDatabaseDirectory $psql $pgData $credentials.postgres
    $env:PATH = "$pgBin;$env:PATH"
    if (!(Test-Path -LiteralPath (Join-Path (Split-Path $server) 'Qt6Core.dll'))) {
        $env:PATH = "D:\Qt\6.5.3\mingw_64\bin;$env:PATH"
    }
    $helpText = & $server --help
    if ($LASTEXITCODE -or !($helpText -match '--import-mode')) {
        throw '请在包含旧库导入功能的新主机包目录中运行。'
    }
    $env:WMS_PG_HOST = '127.0.0.1'
    $env:WMS_PG_PORT = '55432'
    $env:WMS_PG_DATABASE = $database
    $env:WMS_PG_USER = $database
    $env:WMS_PG_PASSWORD = $credentials.app
    $env:PGPASSWORD = $credentials.app
    function Read-Target([string]$Sql) {
        $result = & $psql -X -qAt -w -h $env:WMS_PG_HOST -p $env:WMS_PG_PORT -U $database -d $database -v ON_ERROR_STOP=1 -c $Sql
        if ($LASTEXITCODE -ne 0) { throw '连接或检查目标数据库失败。' }
        return $result
    }
    if ((Read-Target 'SELECT product FROM wms_business.network_identity') -ne $productCode) {
        throw '目标业务库与选定产品不符。'
    }
    if ($Mode -eq 'Empty' -and (Read-Target 'SELECT EXISTS(SELECT 1 FROM wms_business.materials UNION ALL SELECT 1 FROM wms_business.business_documents UNION ALL SELECT 1 FROM wms_business.stock_balances)') -ne 'f') {
        throw '目标系统已录入物料、单据或库存。请选择合并或整库替换。'
    }
    $listeners = @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -eq $servicePort)
    if ($listeners.Count -gt 0) {
        $pidFile = Join-Path $runtime "$Product-server.json"
        if (!(Test-Path -LiteralPath $pidFile -PathType Leaf)) { throw '该端口已有后台，请先用原主机包停止它。' }
        $record = Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json
        $process = Get-Process -Id $record.pid -ErrorAction Stop
        if ($listeners.Count -ne 1 -or $listeners[0].OwningProcess -ne $process.Id -or
            $listeners[0].LocalAddress -notin @('127.0.0.1','0.0.0.0') -or $process.Path -ne $server -or
            !$process.StartTime -or $process.StartTime.ToUniversalTime() -ne ([datetime]$record.started).ToUniversalTime()) {
            throw '无法确认运行中的后台属于当前主机包。请使用启动后台的同一 Windows 账户和主机包操作。'
        }
        if ((Read-Target "SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() AND pid<>pg_backend_pid() AND xact_start IS NOT NULL") -ne '0') {
            throw '目标系统有业务操作正在进行，请保存并退出相关前台后重试。'
        }
        $restartLan = $listeners[0].LocalAddress -eq '0.0.0.0'
        Stop-Process -InputObject $process -ErrorAction Stop
        $restartServer = $true
        if (!$process.WaitForExit(10000)) { throw '目标后台未能在规定时间内退出。' }
        Remove-Item -LiteralPath $pidFile
    }
    # A server started outside these launchers may be listening on another port.
    $sessionCountSql = 'SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() AND pid<>pg_backend_pid()'
    $sessionCount = Read-Target $sessionCountSql
    for ($retry = 0; $sessionCount -ne '0' -and $restartServer -and $retry -lt 10; $retry++) {
        Start-Sleep -Milliseconds 200
        $sessionCount = Read-Target $sessionCountSql
    }
    if ($sessionCount -ne '0') {
        throw '目标库仍有其他客户端连接，请先退出后重试。'
    }
    # Qt writes UTF-8. Decode the diagnostic before Windows PowerShell displays it.
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $server
    $info.Arguments = '--product {0} --import-mode {1} --import-legacy "{2}"' -f $productCode,$Mode.ToLowerInvariant(),$source.FullName
    $info.UseShellExecute = $false
    $info.RedirectStandardError = $true
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    # Decode conflict descriptions as UTF-8 even on a CP936 console. Forward the
    # operator's answer only after the server prints its decision prompt.
    $info.RedirectStandardOutput = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.RedirectStandardInput = $true
    $import = [Diagnostics.Process]::Start($info)
    try {
        $readError = $import.StandardError.ReadToEndAsync()
        while ($null -ne ($line = $import.StandardOutput.ReadLine())) {
            if ($line.StartsWith('{')) {
                $report = $line | ConvertFrom-Json
                Write-Host "本次写入 $($report.totalRows) 行。"
                continue
            }
            Write-Host $line
            if ($line.StartsWith('核对以上全部处理后，输入 MERGE')) {
                $answer = Read-Host
                $import.StandardInput.WriteLine([string]$answer)
                $import.StandardInput.Flush()
            }
        }
        $detail = $readError.GetAwaiter().GetResult().Trim()
        $import.WaitForExit()
        if ($import.ExitCode -ne 0) { throw "旧库导入失败（退出码 $($import.ExitCode)）。$detail" }
        if ($detail) { Write-Warning $detail }
    } finally {
        $import.StandardInput.Close()
        if (!$import.HasExited) { $import.WaitForExit() }
        $import.Dispose()
    }
    Write-Host "$productName 旧库已成功导入，请登录核对库存和单据。"
} catch { $importError = $_ }
finally {
    $lock.Dispose()
    if ($restartServer) {
        try { & (Join-Path $PSScriptRoot 'start-lan-dev.ps1') -Product $Product -AllowLan:$restartLan }
        catch { $restartError = $_ }
    }
    foreach ($key in $savedEnv.Keys) { [Environment]::SetEnvironmentVariable($key,$savedEnv[$key],'Process') }
}
if ($restartError) { Write-Warning "请用主机启动入口重新启动$productName 后台：$restartError" }
if ($importError) { throw $importError }
if ($restartError) { throw '数据已导入，后台需要手动启动。' }
