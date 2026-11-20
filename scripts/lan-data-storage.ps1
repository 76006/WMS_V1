function Enter-LanRuntimeLock([string]$Runtime) {
    [void][IO.Directory]::CreateDirectory($Runtime)
    $lockPath = Join-Path $Runtime 'move-to-documents.lock'
    try {
        # Launchers may nest. A database migration holds this same file exclusively.
        return [IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'ReadWrite')
    } catch [IO.IOException] { throw "Cannot access the LAN storage lock. If migration is running, wait for it to finish. $($_.Exception.Message)" }
}

function Assert-LanStorageLayout([string]$Runtime, [string]$DataRoot, [switch]$ConfirmFirstInstall) {
    $cluster = Join-Path $DataRoot 'pgdata'
    $credentials = Join-Path $DataRoot 'credentials.json'
    if ((Test-Path -LiteralPath (Join-Path $cluster 'PG_VERSION') -PathType Leaf) -and
        (Test-Path -LiteralPath $credentials -PathType Leaf)) { return }
    if ((Test-Path -LiteralPath (Join-Path $Runtime 'pgdata')) -or
        (Test-Path -LiteralPath (Join-Path $Runtime 'credentials.json'))) {
        throw "检测到程序运行目录中还有数据库或凭据。请先核对或迁移原数据库及配套 credentials.json。数据位置：$DataRoot"
    }
    $locator = & (Join-Path $PSScriptRoot 'get-lan-data-root.ps1') -LocatorPath
    if ((Test-Path -LiteralPath $locator) -or (Test-Path -LiteralPath (Join-Path $DataRoot '.wms-data-migration.json'))) {
        throw "发现已有数据库的位置或迁移记录，但 pgdata 与 credentials.json 不齐全。请从同一份备份恢复，数据位置：$DataRoot"
    }
    if ((Test-Path -LiteralPath $cluster) -or (Test-Path -LiteralPath $credentials)) {
        $entries = @(Get-ChildItem -LiteralPath $DataRoot -Force -ErrorAction Stop)
        $unknown = @($entries | Where-Object {
            $_.Name -notin @('credentials.json','pgdata','.wms-start.lock','init-password.tmp') -or
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint)
        })
        $empty = !(Test-Path -LiteralPath $cluster) -or (
            (Test-Path -LiteralPath $cluster -PathType Container) -and
            !(Get-ChildItem -LiteralPath $cluster -Force -ErrorAction Stop | Select-Object -First 1))
        if ($ConfirmFirstInstall -and !$unknown.Count -and $empty) {
            Write-Host "在 $DataRoot 中未发现数据库文件。"
            $reply = Read-Host '仅当这是首次安装且从未录入业务数据时，输入 1 继续初始化；直接回车停止'
            if ($reply -eq '1') { return }
        }
        throw "已停止初始化。请保留 $DataRoot；如以前用过仓库，请恢复同一份备份中的 pgdata 和 credentials.json。"
    }
}

function Initialize-LanCluster([string]$PgBin, [string]$PgData, [string]$PasswordFile) {
    $share = (Resolve-Path -LiteralPath (Join-Path (Split-Path -Parent $PgBin) 'share')).Path
    $drive = $null
    try {
        # initdb includes a share filename in UTF-8 SQL. A Windows ANSI path can break that SQL.
        if ($share -match '[^\x00-\x7f]') {
            $subst = Join-Path $env:SystemRoot 'System32/subst.exe'
            $inUse = [IO.Directory]::GetLogicalDrives()
            foreach ($letter in 90..68) {
                $candidate = ([char]$letter).ToString() + ':'
                if (($candidate + '\') -in $inUse) { continue }
                & $subst $candidate $share | Out-Null
                if ($LASTEXITCODE -eq 0) {
                    $drive = $candidate
                    $share = $drive + '/'
                    break
                }
            }
            if (!$drive) { throw '无法为初始化文件创建临时盘符。可将软件包解压至纯英文路径（例如 D:\WMSHost）后重试。' }
        }
        & (Join-Path $PgBin 'initdb.exe') -D $PgData -L $share -U postgres --encoding=UTF8 --locale=C --auth=scram-sha-256 "--pwfile=$PasswordFile"
        if ($LASTEXITCODE -ne 0) { throw "首次建库失败，请查看上方 initdb 的具体错误。数据位置：$PgData" }
    } finally {
        if ($drive) { & $subst $drive /D | Out-Null }
    }
}

function Assert-LanDatabaseDirectory([string]$Psql, [string]$PgData, [string]$Password) {
    # Only send credentials to the loopback postmaster recorded by this cluster.
    $pidFile = Join-Path $PgData 'postmaster.pid'
    $codePage = [int](Get-ItemPropertyValue -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage' -Name ACP)
    $pidBytes = [IO.File]::ReadAllBytes($pidFile)
    $postmaster = [Text.Encoding]::GetEncoding($codePage).GetString($pidBytes) -split '\r?\n'
    $utf8Path = ([Text.Encoding]::UTF8.GetString($pidBytes) -split '\r?\n')[1]
    $listeners = Get-NetTCPConnection -State Listen -LocalPort 55432 -ErrorAction Stop |
        Where-Object { $_.LocalAddress -eq '127.0.0.1' }
    if ($postmaster.Count -lt 4 -or $postmaster[3] -ne '55432' -or
        $listeners.OwningProcess -notcontains [int]$postmaster[0] -or
        (@($postmaster[1],$utf8Path) -replace '/', '\') -notcontains [IO.Path]::GetFullPath($PgData)) {
        throw "The local PostgreSQL listener does not match the cluster in $PgData."
    }
    $savedPassword = $env:PGPASSWORD
    $savedTimeout = $env:PGCONNECT_TIMEOUT
    try {
        $env:PGPASSWORD = $Password
        $env:PGCONNECT_TIMEOUT = '5'
        # PostgreSQL on Windows writes its native path in the system code page.
        # Compare the running server's own PID file as bytes across the psql boundary.
        $actual = & $Psql -X -w -h 127.0.0.1 -p 55432 -U postgres -d postgres -At -c "SELECT encode(pg_read_binary_file('postmaster.pid'),'hex')"
        if ($LASTEXITCODE -ne 0 -or $actual -ne [BitConverter]::ToString($pidBytes).Replace('-','')) {
            throw "PostgreSQL on port 55432 does not match the cluster in $PgData."
        }
    } finally {
        $env:PGPASSWORD = $savedPassword
        $env:PGCONNECT_TIMEOUT = $savedTimeout
    }
}
