param([switch]$LocatorPath)
$ErrorActionPreference = 'Stop'
# The per-machine locator keeps elevated launches on the host user's data directory
# while allowing a copied application to use the new computer's own Documents.
$repo = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repo 'outputs/lan-dev'
$machineGuid = [guid](Get-ItemPropertyValue -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Cryptography' -Name MachineGuid)
$locator = Join-Path $runtime "data-root-$machineGuid.txt"
if ($LocatorPath) { return $locator }
if (Test-Path -LiteralPath $locator) {
    $path = [IO.File]::ReadAllText($locator).Trim()
    if ($path -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\]+\\[^\\]+\\)' -or $path -match '[\r\n]') {
        throw "Invalid data directory in $locator."
    }
    $path = [IO.Path]::GetFullPath($path).TrimEnd('\', '/')
    if ([IO.Path]::GetFileName($path) -ne 'WMS_OL') { throw "Expected a WMS_OL directory in $locator." }
    return $path
}
$documents = [Environment]::GetFolderPath('MyDocuments', 'DoNotVerify')
if ([string]::IsNullOrWhiteSpace($documents)) { throw 'Windows Documents folder is unavailable.' }
$dataRoot = Join-Path $documents 'WMS_OL'
# Keep the existing development host usable until its owner explicitly migrates it.
if (!(Test-Path -LiteralPath (Join-Path $repo 'WMS-HOST.txt')) -and
    !(Test-Path -LiteralPath (Join-Path $dataRoot 'pgdata')) -and
    !(Test-Path -LiteralPath (Join-Path $dataRoot 'credentials.json')) -and
    (Test-Path -LiteralPath (Join-Path $runtime 'pgdata/PG_VERSION') -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $runtime 'credentials.json') -PathType Leaf)) {
    return $runtime
}
return $dataRoot
