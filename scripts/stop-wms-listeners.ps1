param(
    [ValidateSet('IceBeauty','MeiJingLing','All')][string]$Product = 'All',
    [switch]$NoElevation,
    [string]$LogPath = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$ports = if ($Product -eq 'IceBeauty') { @(18080) } elseif ($Product -eq 'MeiJingLing') { @(18081) } else { @(18080,18081) }
if ($LogPath) { [IO.File]::WriteAllText($LogPath, '', [Text.UTF8Encoding]::new($false)) }

function Write-StopMessage([string]$Message) {
    Write-Host $Message
    if ($LogPath) { [IO.File]::AppendAllText($LogPath, $Message + [Environment]::NewLine, [Text.UTF8Encoding]::new($false)) }
}

function Get-WmsPortListeners {
    # Enumerate once: an empty result is valid, but a denied query must never mean success.
    @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object { $_.LocalPort -in $ports })
}

function Close-WmsListeners {
    $candidates = @(Get-WmsPortListeners | Sort-Object OwningProcess -Unique)
    foreach ($listener in $candidates) {
        $processId = [int]$listener.OwningProcess
        $info = Get-CimInstance Win32_Process -Filter "ProcessId=$processId" -ErrorAction Stop
        if (!$info) { continue }
        if ($info.Name -ine 'WmsServer.exe') {
            Write-StopMessage "Port $($listener.LocalPort) belongs to $($info.Name) (PID $processId); this is not WMS and was left running."
            continue
        }
        if (!$info.ExecutablePath -or !$info.CommandLine) {
            throw [UnauthorizedAccessException]::new("Cannot inspect WmsServer PID $processId. Administrator access is required.")
        }
        if ([IO.Path]::GetFileName($info.ExecutablePath) -ine 'WmsServer.exe') {
            throw "The executable identity changed for PID $processId; no process was stopped."
        }
        # A healthy HTTP endpoint and a PID file are deliberately not required:
        # old packages and a broken database connection must still be stoppable.
        $match = [regex]::Match($info.CommandLine, '(?i)(?:^|\s)"?--product"?(?:=|\s+)"?(ICE_BEAUTY|MEI_JING_LING)"?(?=\s|$)')
        $profile = 'ICE_BEAUTY' # WmsServer's documented command-line default.
        if ($match.Success) { $profile = $match.Groups[1].Value.ToUpperInvariant() }
        elseif ($info.CommandLine -match '(?i)(?:^|\s)"?--product"?(?:=|\s)') {
            throw "Cannot identify the product of WmsServer PID $processId; it was left running."
        }
        $expected = if ($listener.LocalPort -eq 18080) { 'ICE_BEAUTY' } else { 'MEI_JING_LING' }
        if ($profile -ne $expected -and $Product -ne 'All') {
            Write-StopMessage "Port $($listener.LocalPort) belongs to $profile, not $Product; it was left running."
            continue
        }
        $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if (!$process) { continue }
        # Open/check this process before termination; never kill a recycled PID.
        if (!$process.StartTime -or !$process.Path) {
            throw [UnauthorizedAccessException]::new("Cannot access WmsServer PID $processId.")
        }
        # CIM timestamps have less precision than Process.StartTime.
        if (!$info.CreationDate -or $process.Path -ine $info.ExecutablePath -or
            [Math]::Abs(($process.StartTime.ToUniversalTime() - $info.CreationDate.ToUniversalTime()).TotalMilliseconds) -gt 1) {
            throw "Process identity changed for PID $processId; retry the launcher."
        }
        if (!(Get-WmsPortListeners | Where-Object OwningProcess -eq $processId)) { continue }
        Write-StopMessage "Stopping $profile, PID $processId, $($process.Path)"
        Stop-Process -InputObject $process -Force -ErrorAction Stop
        if (!$process.WaitForExit(5000)) { throw "WmsServer PID $processId did not exit in time." }
    }
    $remaining = @(Get-WmsPortListeners)
    if ($remaining.Count) {
        $details = ($remaining | ForEach-Object { "$($_.LocalPort) (PID $($_.OwningProcess))" } | Sort-Object -Unique) -join ', '
        throw "Ports are still occupied: $details. No unrelated processes were terminated."
    }
    Write-StopMessage ('Done. WMS listening ports are free: ' + ($ports -join ', '))
    Write-StopMessage 'PostgreSQL, inventory data and attachments were left untouched.'
}

try {
    Close-WmsListeners
} catch {
    $failure = $_
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $nativeCode = $failure.Exception.PSObject.Properties['NativeErrorCode']
    $cimStatus = $failure.Exception.PSObject.Properties['StatusCode']
    $accessDenied = $failure.Exception -is [UnauthorizedAccessException] -or
        $failure.CategoryInfo.Category -eq [Management.Automation.ErrorCategory]::PermissionDenied -or
        $failure.Exception.HResult -eq -2147024891 -or
        ($nativeCode -and $nativeCode.Value -eq 5) -or
        ($cimStatus -and $cimStatus.Value -eq 2)
    if (!$NoElevation -and !$isAdmin -and $accessDenied) {
        # The child has exactly the same bounded operation; it cannot stop PostgreSQL.
        $logDirectory = Join-Path $repo 'outputs/lan-dev'
        [void][IO.Directory]::CreateDirectory($logDirectory)
        $childLog = Join-Path $logDirectory ('stop-listeners-' + [guid]::NewGuid().ToString('N') + '.log')
        Write-StopMessage 'Windows requires administrator access to close the old server. Accept the UAC prompt to continue.'
        try {
            $arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -Product {1} -NoElevation -LogPath "{2}"' -f $PSCommandPath,$Product,$childLog
            $child = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe') `
                -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru
            $child.WaitForExit()
            if (Test-Path -LiteralPath $childLog) { Get-Content -LiteralPath $childLog -Encoding UTF8 | ForEach-Object { Write-StopMessage $_ } }
            if ($child.ExitCode -ne 0) { throw "Closing the WMS server failed. Details: $childLog" }
            if (@(Get-WmsPortListeners).Count) { throw 'A process started listening again; the ports have not stayed free.' }
            return
        } catch {
            Write-StopMessage ('ERROR: ' + $_.Exception.Message)
            throw
        }
    }
    Write-StopMessage ('ERROR: ' + $failure.Exception.Message)
    throw
}
