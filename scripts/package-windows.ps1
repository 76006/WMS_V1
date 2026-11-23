param(
    [string]$ServerAddress = '',
    [string]$QtRoot = 'D:\Qt\6.5.3\mingw_64',
    [string]$MingwBin = 'D:\Qt\Tools\mingw1120_64\bin',
    [string]$PostgresRoot,
    [string]$VcRuntimeDirectory
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildRelative = 'build-lan-portable-release'
$build = Join-Path $repo $buildRelative
$serverSource = Join-Path $build 'WmsServer.exe'
if (!$PostgresRoot) { $PostgresRoot = Join-Path $repo 'outputs/lan-dev/postgres/pgsql' }
if ($ServerAddress) {
    $ip = $null
    if (![Net.IPAddress]::TryParse($ServerAddress, [ref]$ip) -or
        $ip.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or [Net.IPAddress]::IsLoopback($ip)) {
        throw 'ServerAddress must be the LAN IPv4 address of the host.'
    }
}
if (!$VcRuntimeDirectory) {
    $vc = Get-Item "$env:ProgramFiles\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (!$vc) { throw 'Specify -VcRuntimeDirectory (Visual C++ redistributable x64 CRT directory).' }
    $VcRuntimeDirectory = $vc.FullName
}

function Install-ClientFiles([string]$client) {
    $savedPath=$env:PATH
    try {
        $env:PATH="$MingwBin;$QtRoot\bin;$savedPath"
        foreach($app in @('IceBeautyWms','MeiJingLingWms')) {
            $source=Join-Path $repo "$buildRelative/$($app)Lan.exe"
            if(!(Test-Path -LiteralPath $source)) { throw "Missing build: $source" }
            $target=Join-Path $client "$($app)Lan.exe"
            Copy-Item -LiteralPath $source -Destination $target
            # Let Qt inspect the original build before stripping its debug symbols.
            & (Join-Path $QtRoot 'bin/windeployqt.exe') --compiler-runtime --skip-plugin-types sqldrivers `
                --qtpaths (Join-Path $QtRoot 'bin/qtpaths6.exe') `
                --qmldir (Join-Path $repo 'resources') --dir $client $target
            if($LASTEXITCODE -ne 0) { throw 'Qt dependency deployment failed.' }
            & (Join-Path $MingwBin 'strip.exe') --strip-debug $target
            if($LASTEXITCODE -ne 0) { throw 'Could not strip distribution copy.' }
        }
    } finally { $env:PATH=$savedPath }
    foreach($folder in @('仓库系统表单模板','美鲸灵仓库系统表单模板')) {
        $templateDir = New-Item -ItemType Directory -Path (Join-Path $client $folder)
        Get-ChildItem -LiteralPath (Join-Path $repo $folder) -File -Filter '*.json' | Copy-Item -Destination $templateDir.FullName
    }
    [IO.File]::WriteAllText((Join-Path $client 'server-address.txt'),$ServerAddress,[Text.Encoding]::ASCII)
    $launcher=@'
param([ValidateSet('IceBeauty','MeiJingLing')][string]$Product='IceBeauty')
$ErrorActionPreference='Stop'
$addressFile = Join-Path $PSScriptRoot 'server-address.txt'
$savedAddress = if ([IO.File]::Exists($addressFile)) { [IO.File]::ReadAllText($addressFile).Trim() } else { '' }
$server = $savedAddress
$address = $null
while (![Net.IPAddress]::TryParse($server,[ref]$address) -or
       $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
       $address.ToString() -ne $server -or [Net.IPAddress]::IsLoopback($address) -or $server -eq '0.0.0.0') {
    $server = Read-Host '请输入主机启动窗口显示的局域网 IPv4 地址，直接回车退出'
    if ([string]::IsNullOrWhiteSpace($server)) { return }
    $server = $server.Trim()
}
if ($server -ne $savedAddress) {
    try { [IO.File]::WriteAllText($addressFile,$server,[Text.Encoding]::ASCII) }
    catch { Write-Warning '本次将使用输入的地址。此目录无法保存设置，下次请重新输入。' }
}
$port=if($Product -eq 'IceBeauty'){18080}else{18081}
Remove-Item Env:WMS_DEMO_LOGIN_PASSWORD -ErrorAction SilentlyContinue
Start-Process -FilePath (Join-Path $PSScriptRoot "$($Product)WmsLan.exe") -ArgumentList @('--server',"http://${server}:$port") -WorkingDirectory $PSScriptRoot
'@
    Set-Content -LiteralPath (Join-Path $client 'start-client.ps1') -Value $launcher -Encoding UTF8
    foreach($item in @(@('冰美肌','IceBeauty'),@('美鲸灵','MeiJingLing'))) {
        $cmd="@echo off`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"%~dp0start-client.ps1`" -Product $($item[1])`r`nif errorlevel 1 pause`r`n"
        [IO.File]::WriteAllText((Join-Path $client "启动$($item[0])联网版.cmd"),$cmd,[Text.Encoding]::ASCII)
    }
    $guideAddress = if ($ServerAddress) { $ServerAddress } else { '主机IP' }
    $instructions=@"
第二台电脑使用方法（Windows 64 位，仅联网客户端）

1. 解压 WMS仓库系统.zip 后，进入 client 文件夹。
2. 两台电脑连接同一公司局域网；主机先开启局域网测试后台并放行端口。
3. 双击“启动冰美肌联网版.cmd”或“启动美鲸灵联网版.cmd”。
4. 首次提示输入地址时，填写主机启动窗口显示的局域网 IPv4 地址，回车后自动保存到 server-address.txt。冰美肌端口 18080，美鲸灵端口 18081。
5. 使用主机软件中创建的业务账号登录。
6. 一台电脑录入测试入库，另一台点击“刷新”，核对库存及单据是否一致。

此目录中的前台只有联网入口，拒绝 --local 和 --database 参数，不读取原单机数据库。
联网配置使用独立名称，自动归档放在“我的文档/冰美肌仓库系统表单（联网版）”或对应美鲸灵目录。
旧单机版仍使用旧数据库，两边数据不会自动合并。手动导出文件时请选择新的保存位置。

主机 IP 改变后，用记事本修改 server-address.txt。不要填写 127.0.0.1，那表示第二台电脑自己。
不需要安装 Qt、PostgreSQL 或 Git。业务单据按 JSON 模板直接生成 .xlsx，可用 WPS 表格或 Microsoft Excel 打开。
主机必须开机，后台必须运行；数据统一保存在主机的 PostgreSQL 测试库。
实际数据以所连接主机为准；主机已有的数据继续使用，新主机首次部署才初始化空库。当前 HTTP 联调用于受信任公司局域网。

连接不通时，先在第二台电脑浏览器打开 http://${guideAddress}:18080/health
美鲸灵对应 http://${guideAddress}:18081/health
无法打开时检查主机后台、IP、Windows 防火墙、两机是否同网以及访客 Wi-Fi 隔离。
"@
    Set-Content -LiteralPath (Join-Path $client '第二台电脑使用说明.txt') -Value $instructions -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $client 'qt.conf') -Value "[Paths]`nPrefix=.`nQmlImports=qml" -Encoding ASCII
    foreach($file in @('platforms/qwindows.dll','Qt6Core.dll','libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
        if(!(Test-Path -LiteralPath (Join-Path $client $file))) { throw "Missing dependency: $file" }
    }
}

function Install-PackageFiles([string]$package) {
    $scripts = Join-Path $package 'scripts'
    $server = Join-Path $package 'build-lan-business'
    $pg = Join-Path $package 'outputs/lan-dev/postgres/pgsql'
    $client = Join-Path $package 'client'
    foreach ($dir in @($scripts,$server,$client,"$pg/bin","$pg/lib","$server/sqldrivers")) {
        [void][IO.Directory]::CreateDirectory($dir)
    }
    function Copy-Named($source,$destination,[string[]]$names) {
        foreach ($name in $names) { Copy-Item -LiteralPath (Join-Path $source $name) -Destination $destination }
    }
    Copy-Named $PSScriptRoot $scripts @('start-lan-dev.ps1','stop-lan-dev.ps1','enable-lan-test.ps1','get-lan-data-root.ps1',
        'lan-data-storage.ps1','start-lan-client.ps1','start-packaged-lan-host.ps1','import-legacy-data.ps1','stop-wms-listeners.ps1',
        'prepare-lan-server.ps1')
    [IO.File]::WriteAllText((Join-Path $package 'WMS-HOST.txt'),"WMS LAN portable test host v1$([Environment]::NewLine)")

    # Assemble only the PostgreSQL engine, client utilities and distribution components.
    Copy-Named $PostgresRoot $pg @('server_license.txt','commandlinetools_3rd_party_licenses.txt')
    Copy-Named "$PostgresRoot/bin" "$pg/bin" @('postgres.exe','initdb.exe','pg_ctl.exe','pg_isready.exe','psql.exe',
        'pg_dump.exe','pg_dumpall.exe','pg_restore.exe','pg_controldata.exe')
    Get-ChildItem -LiteralPath "$PostgresRoot/bin" -Filter '*.dll' -File | Where-Object Name -notlike 'wx*.dll' |
        Copy-Item -Destination "$pg/bin"
    Get-ChildItem -LiteralPath "$VcRuntimeDirectory" -Filter '*.dll' -File | Copy-Item -Destination "$pg/bin"
    Get-ChildItem -LiteralPath "$PostgresRoot/lib" -Filter '*.dll' -File | Copy-Item -Destination "$pg/lib"
    Copy-Item -LiteralPath "$PostgresRoot/share" -Destination $pg -Recurse

    Install-ClientFiles $client

    Copy-Item -LiteralPath $serverSource -Destination (Join-Path $server 'WmsServer.exe')
    Copy-Named (Join-Path $QtRoot 'plugins/sqldrivers') "$server/sqldrivers" @('qsqlpsql.dll','qsqlite.dll')
    $oldPath = $env:PATH
    try {
        $env:PATH = "$MingwBin;$QtRoot/bin;$pg/bin;$oldPath"
        & (Join-Path $QtRoot 'bin/windeployqt.exe') --no-translations --compiler-runtime --skip-plugin-types sqldrivers --dir $server (Join-Path $server 'WmsServer.exe')
        if ($LASTEXITCODE) { throw 'Qt server deployment failed.' }
        & (Join-Path $MingwBin 'strip.exe') --strip-debug (Join-Path $server 'WmsServer.exe')
        if ($LASTEXITCODE) { throw 'Could not strip the packaged copy of WmsServer.' }
        $env:PATH = "$pg/bin;$env:SystemRoot/System32;$env:SystemRoot"
        $helpText = & (Join-Path $server 'WmsServer.exe') --help
        if ($LASTEXITCODE -or !($helpText -match '--init-lan-empty') -or !($helpText -match '--import-legacy')) {
            throw 'Use a server build with both empty LAN initialization and legacy SQLite import.'
        }
        & (Join-Path $pg 'bin/initdb.exe') --version
        if ($LASTEXITCODE) { throw 'The packaged PostgreSQL runtime is incomplete.' }
    } finally { $env:PATH = $oldPath }
    [IO.File]::WriteAllText((Join-Path $server 'qt.conf'),"[Paths]`r`nPrefix=.`r`n",[Text.Encoding]::ASCII)
    $licenseDir = Join-Path $package 'licenses'
    [void][IO.Directory]::CreateDirectory($licenseDir)
    $qtLicense = Join-Path (Split-Path $QtRoot) 'Src/qtbase/LICENSES'
    if (Test-Path -LiteralPath $qtLicense) { Copy-Item -LiteralPath $qtLicense -Destination (Join-Path $licenseDir 'Qt') -Recurse }

    function Write-Launcher([string]$name,[string]$command,[switch]$PauseOnError) {
        $finish = if ($PauseOnError) { 'if errorlevel 1 pause' } else { 'pause' }
        [IO.File]::WriteAllText((Join-Path $package $name),("@echo off`r`n" + $command + "`r`n" + $finish + "`r`n"),[Text.Encoding]::ASCII)
    }
    $ps = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File '
    Write-Launcher '启动冰美肌.cmd' ($ps + '"%~dp0scripts\start-lan-client.ps1" -Product IceBeauty -StartLocalServer') -PauseOnError
    Write-Launcher '启动美鲸灵.cmd' ($ps + '"%~dp0scripts\start-lan-client.ps1" -Product MeiJingLing -StartLocalServer') -PauseOnError
    Write-Launcher '启动主机后台.cmd' ($ps + '"%~dp0scripts\start-packaged-lan-host.ps1" -InteractiveSetup')
    Write-Launcher '停止主机后台.cmd' ($ps + '"%~dp0scripts\stop-lan-dev.ps1" -StopDatabase')
    Write-Launcher '一键关闭仓库后台.cmd' ($ps + '"%~dp0scripts\stop-wms-listeners.ps1" -Product All')
    Write-Launcher '导入旧版数据库.cmd' ($ps + '"%~dp0scripts\import-legacy-data.ps1"')
    $guide = @'
新主机使用说明（Windows 10/11 64 位，局域网测试版）

本机直接使用：完整解压后，双击“启动冰美肌.cmd”或“启动美鲸灵.cmd”，自动启动对应后台并打开登录界面；两个系统可以同时打开，不需要 Qt Creator。沿用此 Windows 账户“文档\WMS_OL”中的既有数据和密码。软件包不包含业务数据库，也不覆盖已有数据库。首次在没有 WMS_OL 的新电脑使用，才初始化空数据库，初始账号 admin / 123456。

若提示端口被旧包或 Debug 后台占用，先保存单据、关闭前台，再双击“一键关闭仓库后台.cmd”，然后重新启动。要让其他电脑访问，请按下面的局域网步骤启动；此前若已使用本机入口，先关闭后台，再启动局域网后台。

供其他电脑共同使用：

1. 把整个 ZIP 解压到新主机的固定程序目录，例如 D:\WMS主机。用负责运行仓库软件的日常 Windows 账户登录并解压。
2. 在主机双击“启动主机后台.cmd”。如果提示当前办公网络为 Public（公用），输入 1 打开 Windows 设置，选择已连接的办公 Wi-Fi 或以太网，在网络属性里设为“专用网络”，回到启动窗口输入 2。已连接专用或域网络时会直接继续。首次配置防火墙时请按 Windows 提示授权。等待两套后台就绪，记下窗口显示的第二台电脑应填写的主机 IP 地址。
3. 在这台主机双击“启动冰美肌.cmd”或“启动美鲸灵.cmd”。首次应用账号 admin，测试密码 123456。两套业务库独立，首次仅初始化基础配置和管理员。已有数据目录会继续使用。
4. 其他电脑解压同一个 ZIP，进入 client 文件夹，在 server-address.txt 填写主机的局域网 IPv4 地址，再双击对应的联网版入口。原来指向旧主机的地址需要更新。能在其他电脑的浏览器打开 http://新主机IP:18080/health（美鲸灵 18081）即代表可连接。
5. 物料、库存、单据、账号和已上传的附件集中存放在新主机“文档\WMS_OL\pgdata”中，配套 credentials.json 在同一 WMS_OL 目录。请妥善保管整个 WMS_OL。两款产品的自动归档表单分别保存在填单电脑的“文档”下。
   首次安装曾中断时，若启动窗口提示“未发现数据库文件”，并且这台主机从未录入过业务数据，可输入 1 继续建库。若主机以前使用过仓库，请保留 WMS_OL，从同一份备份核对 pgdata 和 credentials.json。
6. 合上或关闭前台后其他电脑可继续使用。系统重启后请再次双击“启动主机后台.cmd”。需要关闭服务时，请先保存各台电脑的单据，再双击“停止主机后台.cmd”。
7. 遇到 18080/18081 被旧程序占用，可双击“一键关闭仓库后台.cmd”。它按实际端口识别各目录下的 WmsServer，关闭后验证端口释放；数据库保持运行，数据和附件保留。权限不足时按 Windows 提示提权。非 WMS 程序不会被关闭。

从旧单机版导入历史数据

1. 在旧版主窗口底部状态栏查看「数据库」路径，或打开「系统设置 → 数据库文件」核对。冰美肌的默认文件名为 ice_beauty_wms.db，美鲸灵为 mei_jing_ling_wms.db。退出所有使用该旧库的单机程序，将 .db 复制到新主机单独的备份文件夹。同目录若有该库的 -wal、-shm、-journal 文件，请一起复制并保留原名。旧版已经生成的 Excel 表单保存在原填单电脑，需要时单独复制备份。
2. 启动主机后台。请先保存单据并退出目标系统的所有联网前台，再双击「导入旧版数据库.cmd」。输入 1 选冰美肌，2 选美鲸灵，在文件选择框选中备份的 .db。选择 1 导入空库、2 合并或 3 整库替换，核对目标、源文件与操作后输入 YES。
3. 合并会保留主机档案和业务数据。独立旧库的重复单号会列出拟用的新号，重叠库存会列出合并前后的数量。核对后输入 MERGE 才提交有冲突的合并，直接回车撤销。重导同一来源时会识别已导入的记录。
4. 整库替换覆盖所选产品全部账号、库存、单据和附件。要保留替换前的业务，请先备份主机库。导入成功后重新登录核对；整库替换使用旧库账号密码，合并时已有同名账号继续使用主机密码。
5. 导入按实际表和字段判断兼容性，不设旧库版本上限。原 SQLite 文件只读。导入时暂停所选产品后台，结束后恢复原监听方式。

每次更换程序请保留 WMS_OL。备份运行中的数据库时请使用 PostgreSQL 的备份工具，或正常停止主机后台后复制整个 WMS_OL。此包含运行环境，不需要安装 Qt 或 PostgreSQL。业务表单按配套 JSON 模板直接生成 .xlsx，查看或编辑可使用 WPS 表格或 Microsoft Excel。
'@
    Set-Content -LiteralPath (Join-Path $package '新主机使用说明.txt') -Encoding UTF8 -Value $guide

    $forbidden = @(Get-ChildItem -LiteralPath $package -Force -Recurse |
        Where-Object { $_.Name -match '^(pgdata|credentials\.json|pgpass\.conf|postgresql\.auto\.conf|data-root.*\.txt|.*-server\.json|postmaster\.pid)$|\.(db|sqlite3?|log|pid|zip)$' })
    if ($forbidden.Count) { throw ('Unexpected runtime or data file: ' + $forbidden[0].FullName) }
    if (Test-Path -LiteralPath (Join-Path $client 'sqldrivers/qsqlite.dll')) { throw 'Expected the LAN-only frontend runtime.' }
    foreach ($path in @("$server/Qt6HttpServer.dll","$server/sqldrivers/qsqlpsql.dll","$server/sqldrivers/qsqlite.dll","$pg/bin/libpq.dll",
        "$pg/bin/vcruntime140.dll","$pg/share/postgres.bki","$pg/lib/citext.dll","$client/platforms/qwindows.dll")) {
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing runtime file: $path" }
    }
}

$dist = Join-Path $repo 'dist'
$work = Join-Path $repo '.codex-work/packaging'
foreach ($dir in @($dist,$work)) { [void][IO.Directory]::CreateDirectory($dir) }
# Serialize builds and replacement. Keep the current ZIP available until verification completes.
$lock = [IO.File]::Open((Join-Path $work 'release.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
$stage = Join-Path $work ([guid]::NewGuid().ToString('N'))
$oldEnvironmentPath = $env:Path
try {
    $env:Path = "$MingwBin;$QtRoot/bin;$env:Path"
    & cmake -S $repo -B $build -G 'MinGW Makefiles' "-DCMAKE_PREFIX_PATH=$QtRoot" '-DCMAKE_BUILD_TYPE=Release' '-DWMS_NETWORK_CLIENT_ONLY=ON' '-DWMS_BUILD_SERVER=ON'
    if ($LASTEXITCODE) { throw 'Release configuration failed.' }
    & cmake --build $build --target IceBeautyWms MeiJingLingWms WmsServer --parallel 4
    if ($LASTEXITCODE) { throw 'Release build failed.' }
    $package = Join-Path $stage 'WMS'

    Install-PackageFiles $package
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $candidate = Join-Path $stage 'release.tmp'
    [IO.Compression.ZipFile]::CreateFromDirectory($package,$candidate,[IO.Compression.CompressionLevel]::Optimal,$true)
    # Check the archived bytes against every deployed file, including launchers, binaries and templates.
    $archive = [IO.Compression.ZipFile]::OpenRead($candidate)
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        foreach ($file in Get-ChildItem -LiteralPath $package -File -Recurse) {
            $rel = $file.FullName.Substring($package.Length + 1).Replace('\','/')
            $entry = $archive.GetEntry("WMS/$rel")
            if (!$entry) { $entry = $archive.GetEntry("WMS\$($rel.Replace('/','\'))") }
            if (!$entry -or $entry.Length -ne $file.Length) { throw "Invalid ZIP entry: $rel" }
            $stream = $entry.Open()
            try { $actual = [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','') }
            finally { $stream.Dispose() }
            if ($actual -ne (Get-FileHash -LiteralPath $file.FullName).Hash) { throw "Invalid ZIP data: $rel" }
        }
    } finally { $hash.Dispose(); $archive.Dispose() }
    $destination = Join-Path $dist 'WMS仓库系统.zip'
    if ([IO.File]::Exists($destination)) {
        [IO.File]::Replace($candidate,$destination,[System.Management.Automation.Language.NullString]::Value)
    } else { [IO.File]::Move($candidate,$destination) }
    Write-Output "PACKAGE=$destination"
} finally {
    $env:Path = $oldEnvironmentPath
    try {
        if (Test-Path -LiteralPath $stage) {
            $resolved = (Resolve-Path -LiteralPath $stage).Path
            if ((Split-Path $resolved) -ne (Resolve-Path -LiteralPath $work).Path -or
                (Split-Path $resolved -Leaf) -notmatch '^[a-f0-9]{32}$') { throw "Unexpected staging path: $resolved" }
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    } finally { $lock.Dispose() }
}
