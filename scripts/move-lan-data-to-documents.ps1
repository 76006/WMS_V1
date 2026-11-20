# Run on the host as the Windows account that runs PostgreSQL.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repo 'outputs\lan-dev'
$bin = Join-Path $runtime 'postgres\pgsql\bin'
$old = Join-Path $runtime 'pgdata'
$documents = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
if (!$documents -or !(Test-Path -LiteralPath $documents -PathType Container)) { throw 'Windows Documents folder unavailable.' }
$root = Join-Path $documents 'WMS_OL'
$new = Join-Path $root 'pgdata'
$oldSecrets = Join-Path $runtime 'credentials.json'
$newSecrets = Join-Path $root 'credentials.json'
$marker = Join-Path $root '.wms-data-migration.json'
$locator = & (Join-Path $PSScriptRoot 'get-lan-data-root.ps1') -LocatorPath
$pgCtl = Join-Path $bin 'pg_ctl.exe'
$psql = Join-Path $bin 'psql.exe'
$utf8 = New-Object System.Text.UTF8Encoding($false)
. (Join-Path $PSScriptRoot 'lan-data-storage.ps1')
$products = @{wms_icebeauty_business_demo='ICE_BEAUTY'; wms_meijingling_business_demo='MEI_JING_LING'}

function Same-Path([string]$a, [string]$b) {
    return [IO.Path]::GetFullPath($a).TrimEnd('\') -eq [IO.Path]::GetFullPath($b).TrimEnd('\')
}
function Query([string]$database, [string]$sql) {
    $line = & $psql -X -w -h 127.0.0.1 -p 55432 -U postgres -d $database -At -v ON_ERROR_STOP=1 -c $sql
    if ($LASTEXITCODE -ne 0) { throw "PostgreSQL query failed for $database." }
    return $line
}
function Running([string]$path) {
    if (!(Test-Path -LiteralPath (Join-Path $path 'postmaster.pid'))) { return $false }
    & $pgCtl -D $path status | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "PostgreSQL PID file requires inspection at $path." }
    return $true
}
function Control([string]$path) {
    $lines = & (Join-Path $bin 'pg_controldata.exe') $path
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect the database control file at $path." }
    $idLine = $lines | Select-String '^Database system identifier:\s*(\d+)\s*$'
    $stateLine = $lines | Select-String '^Database cluster state:\s*(.+?)\s*$'
    if (!$idLine -or !$stateLine) { throw 'Cannot verify the PostgreSQL control file.' }
    return @{Id=$idLine.Matches[0].Groups[1].Value; State=$stateLine.Matches[0].Groups[1].Value}
}
function Check-Db([string]$expected, [string]$id) {
    Assert-LanDatabaseDirectory $psql $expected $env:PGPASSWORD
    if ((Query postgres 'SELECT system_identifier FROM pg_control_system()') -ne $id) {
        throw "Port 55432 does not serve the expected cluster at $expected."
    }
    foreach ($db in $products.Keys) {
        $identity = Query $db "SELECT (SELECT product FROM wms_lan.system_identity WHERE id=1),(SELECT product FROM wms_business.network_identity)"
        if ($identity -ne ($products[$db]+'|'+$products[$db])) { throw "Product identity mismatch in $db." }
    }
}
function Start-Cluster([string]$path) {
    $log = Join-Path $runtime 'postgres.log'
    $arguments = "-D `"$path`" -l `"$log`" -o `"-h 127.0.0.1 -p 55432`" -t 50 -w start"
    $starter = Start-Process -FilePath $pgCtl -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (!$starter.WaitForExit(60000) -or $starter.ExitCode -ne 0) { throw "Check PostgreSQL startup at $path and $log." }
}
function Tree-Manifest([string]$path, [bool]$hash) {
    # Walk one level at a time so a junction can never lead the copy outside this cluster.
    $entries = @{}
    $dirs = New-Object 'System.Collections.Generic.Stack[string]'
    $dirs.Push($path)
    while ($dirs.Count) {
        foreach ($item in (Get-ChildItem -LiteralPath $dirs.Pop() -Force)) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "External link requires separate migration: $($item.FullName)" }
            $key = $item.FullName.Substring($path.Length+1)
            if ($item.PSIsContainer) {
                $entries[$key] = 'dir'
                $dirs.Push($item.FullName)
            } elseif ($hash) {
                $entries[$key] = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
            } else {
                $entries[$key] = [long]$item.Length
            }
        }
    }
    return $entries
}
function Secure-Root([string]$path) {
    # The target contains both the entire cluster and its database credentials.
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true,$false)
    $owner = [Security.Principal.WindowsIdentity]::GetCurrent().User
    foreach ($sid in @($owner.Value,'S-1-5-18','S-1-5-32-544') | Select-Object -Unique) {
        $identity = New-Object Security.Principal.SecurityIdentifier($sid)
        $rule = New-Object Security.AccessControl.FileSystemAccessRule($identity,'FullControl','ContainerInherit,ObjectInherit','None','Allow')
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $path -AclObject $acl
}

$lock = [IO.File]::Open((Join-Path $runtime 'move-to-documents.lock'),'OpenOrCreate','ReadWrite','None')
$priorPassword = $env:PGPASSWORD
$priorLocale = $env:LC_ALL
$priorTimeout = $env:PGCONNECT_TIMEOUT
$restartOld = $false
try {
    $env:LC_ALL = 'C'
    $env:PGCONNECT_TIMEOUT = '5'
    if ((Test-Path -LiteralPath $locator) -and !(Same-Path ([IO.File]::ReadAllText($locator).Trim()) $root)) {
        throw 'The saved data directory belongs to a different Documents folder. Run as the host database owner.'
    }
    $state = $null
    if (Test-Path -LiteralPath $marker) {
        $state = [IO.File]::ReadAllText($marker) | ConvertFrom-Json
        if ($state.version -ne 1 -or !(Same-Path $state.source $old) -or !(Same-Path $state.target $new) -or
            $state.cluster -notmatch '^\d+$') { throw 'Destination marker does not match this migration.' }
    } elseif ((Test-Path -LiteralPath $root) -and @(Get-ChildItem -LiteralPath $root -Force).Count) {
        throw "Destination already contains files. Nothing was replaced: $root"
    }
    foreach ($path in @($root,$old,$new,$newSecrets)) {
        if ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "A linked data location requires separate migration: $path"
        }
    }
    if ($state -and (Test-Path -LiteralPath $new)) {
        # Published copies are never reseeded. A later run can finish a cutover after interruption.
        if ((Test-Path -LiteralPath $old) -and (Running $old)) { throw 'Both data locations exist and the original is active. Manual review required.' }
        if ((Control $new).Id -ne $state.cluster) { throw 'Unexpected cluster at the final destination.' }
        $env:PGPASSWORD = ([IO.File]::ReadAllText($newSecrets) | ConvertFrom-Json).postgres
        if (!(Running $new)) { Start-Cluster $new }
    } else {
        $oldControl = Control $old
        if ($state -and $state.cluster -ne $oldControl.Id) { throw 'Source cluster has changed since migration started.' }
        $env:PGPASSWORD = ([IO.File]::ReadAllText($oldSecrets) | ConvertFrom-Json).postgres
        $online = Running $old
        if (!$online -and !$state) { throw 'Start and verify the original test database before the first migration.' }
        if ($online) {
            Check-Db $old $oldControl.Id
            if ((Query postgres "SELECT count(*) FROM pg_tablespace WHERE pg_tablespace_location(oid) <> ''") -ne '0') {
                throw 'External tablespaces require separate migration.'
            }
        }
        $sizes = Tree-Manifest $old $false
        $bytes = ($sizes.Values | Where-Object { $_ -is [long] } | Measure-Object -Sum).Sum
        $drive = New-Object IO.DriveInfo([IO.Path]::GetPathRoot($root))
        if ($drive.AvailableFreeSpace -lt ($bytes + 128MB)) { throw 'Insufficient free space in Documents.' }
        if (!$state) {
            [IO.Directory]::CreateDirectory($root) | Out-Null
            Secure-Root $root
            $state = [pscustomobject]@{version=1;source=$old;target=$new;cluster=$oldControl.Id}
            [IO.File]::WriteAllText($marker,($state | ConvertTo-Json),$utf8)
        }
        if ((Test-Path -LiteralPath $newSecrets) -and (Get-FileHash -LiteralPath $newSecrets).Hash -ne (Get-FileHash -LiteralPath $oldSecrets).Hash) {
            throw 'The existing destination credentials do not match the source.'
        }
        $stage = Join-Path $root ('.copy-'+[Guid]::NewGuid().ToString('N'))
        [IO.Directory]::CreateDirectory($stage) | Out-Null
        if ($online) {
            if ((Query postgres "SELECT count(*) FROM pg_stat_activity WHERE pid <> pg_backend_pid() AND backend_type='client backend' AND xact_start IS NOT NULL") -ne '0' -or
                (Query postgres 'SELECT count(*) FROM pg_prepared_xacts') -ne '0') {
                throw 'A transaction is in progress. Finish the current operations and rerun.'
            }
            & $pgCtl -D $old -m fast -t 50 -w stop
            if ($LASTEXITCODE -ne 0) { throw 'Original database stop failed. Verify its state before retrying.' }
        }
        $restartOld = $true
        if ((Running $old) -or (Control $old).State -ne 'shut down') { throw 'Original cluster did not stop cleanly.' }
        $expected = Tree-Manifest $old $true
        & robocopy.exe $old $stage /E /COPY:DAT /DCOPY:DAT /XJ /R:1 /W:1 /NFL /NDL /NP /NJH /NJS | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "Copy failed. Partial copy preserved at $stage." }
        $actual = Tree-Manifest $stage $true
        if ($expected.Count -ne $actual.Count) { throw 'Copied directory entry count differs.' }
        foreach ($key in $expected.Keys) {
            if ($expected[$key] -ne $actual[$key]) { throw "SHA-256 or directory mismatch: $key" }
        }
        if (!(Test-Path -LiteralPath $newSecrets)) {
            [IO.File]::Copy($oldSecrets,$newSecrets,$false)
        }
        if ((Get-FileHash -LiteralPath $oldSecrets).Hash -ne (Get-FileHash -LiteralPath $newSecrets).Hash) { throw 'Credential copy mismatch.' }
        [IO.Directory]::Move($stage,$new)
        $restartOld = $false
        Write-Output "Verified $($expected.Count) file and directory entries. Starting copied cluster."
        Start-Cluster $new
    }
    Check-Db $new $state.cluster
    if (!(Test-Path -LiteralPath $locator)) {
        $locatorTemp = $locator + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
        [IO.File]::WriteAllText($locatorTemp,($root+[Environment]::NewLine),$utf8)
        [IO.File]::Move($locatorTemp,$locator)
    }
    Write-Output "Both product databases verified. Active data directory: $new"
    Write-Output "Offline source retained: $old"
} catch {
    if ($restartOld -and !(Test-Path -LiteralPath $new) -and !(Running $old)) {
        Write-Warning 'Cutover did not publish the new cluster. Restarting the original in place.'
        Start-Cluster $old
    }
    throw
} finally {
    $env:PGPASSWORD = $priorPassword
    $env:LC_ALL = $priorLocale
    $env:PGCONNECT_TIMEOUT = $priorTimeout
    $lock.Dispose()
}
