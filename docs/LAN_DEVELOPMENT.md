# 局域网完整业务版

日期：2026-11-16。单机基线 main / `077c94e`；开发分支 `codex/lan-postgresql-foundation`。

## 用完整界面测试

2026-11-20 起的免安装主机包支持直接双击“启动冰美肌.cmd”或“启动美鲸灵.cmd”，自动启动该系统后台并打开登录界面，适用于只在本机使用。它读取当前 Windows 账户既有的“文档/WMS_OL”，两套系统分别连接各自数据库；首次在没有数据目录的新主机使用才初始化空库。包内不包含旧数据或口令。若旧包/Debug 后台占用端口，先保存业务，再用“一键关闭仓库后台.cmd”关闭旧后台并启动新包。

在另一台 Windows 10/11 64 位主机上，将 `WMS仓库系统.zip` 完整解压到固定文件夹，用日常 Windows 账户双击“启动主机后台.cmd”。如果办公网络被标为公用，按 1 打开网络设置，将已连接且信任的办公网络设为专用，再回到窗口按 2 重新检测。按 Windows 提示授权配置防火墙。两套后台就绪后，主机可以打开“启动冰美肌.cmd”或“启动美鲸灵.cmd”。第二台电脑使用同一个 ZIP，打开 `client` 中对应的联网版入口，首次输入主机启动窗口显示的局域网 IPv4 地址，后续沿用保存在 `server-address.txt` 的设置。新主机首次初始化空库存和空业务单据，默认测试账号为 `admin / 123456`。

双击项目根目录的 `启动冰美肌.cmd` 或 `启动美鲸灵.cmd`。冰美肌和美鲸灵的联网测试管理员均为 `admin`，新测试部署的默认登录密码为 `123456`。在主机使用上述入口时，登录框会预填初始密码。在第二台电脑上使用同一系统的账号和密码登录。若已在软件中修改密码，请输入新密码。

测试后台启用 `WMS_LAN_TEST_PASSWORDS=1` 时，在“用户管理”中新增用户或重置密码，直接确认即可采用 `123456`，也可以填写至少 8 位的自定义密码。用户在“系统设置”中修改个人密码后，下次登录使用新密码。

登录后是原来的完整主窗口，包括物料、仓库库位、送检、出入库、领料、退料、成品入库、调拨、盘点、流水、统计、批次/SN、附件、表单和用户管理。顶部“刷新”或重新进入页面会读取最新数据。初次测试请创建测试仓库、库位和物料，再录入其他入库。

主机可以同时运行后台和前台。重复打开两个前台窗口，一边录单、另一边刷新，即可测试互通。退出前台不会停止后台。

库存调拨支持一张单多条物料。每行分别选择来源仓库、库位、批次、数量和 SN，所有明细使用单头的目标仓库、库位。后台在同一事务中生成一个单号、各行明细及成对的调出/调入流水；任意一行失败时整单回滚。近期调拨列表按单号、行号排列，“撤销所选明细”只处理选中行。修改单据和整单撤销支持多行。升级时需同时更新后台和前台；后台仍兼容原来单条调拨的客户端。无需修改数据库结构或重新导入旧数据。

## 数据位置

第二台电脑解压同一个 `WMS仓库系统.zip`，进入 `client` 目录并填写主机地址，双击对应产品的联网版入口。此目录的 `IceBeautyWmsLan.exe` 和 `MeiJingLingWmsLan.exe` 只连接主机。

联网客户端配置使用独立的应用名称，自动表单归档使用“我的文档/冰美肌仓库系统表单（联网版）”或对应美鲸灵目录，临时文件也独立。撤销送检单时只处理当前联网归档中的文件，不使用服务器记录里来自其他电脑或旧版客户端的绝对路径。手工导出文件仍由用户选择保存位置，请避免覆盖旧导出文件。

原单机版继续使用原数据库。如需将单机历史数据转入主机，请按包内《新主机使用说明》操作。

| 系统 | PostgreSQL 数据库 | 后台地址 |
|---|---|---|
| 冰美肌 | wms_icebeauty_business_demo | http://127.0.0.1:18080 |
| 美鲸灵 | wms_meijingling_business_demo | http://127.0.0.1:18081 |

新主机在文件资源管理器中打开“文档 → WMS_OL”。冰美肌和美鲸灵的业务数据都存放在其中的 `pgdata` 文件夹，由 PostgreSQL 统一管理。主机安装目录中的 `outputs/lan-dev/data-root-<本机标识>.txt` 记录数据文件夹的完整路径。可在项目目录运行 `.\scripts\get-lan-data-root.ps1` 查询当前使用的位置。数据库监听 `127.0.0.1:55432`，各库的业务表位于 `wms_business` schema。

新主机包首次启动时自动创建空业务库。旧开发机已经初始化的数据库仍可使用原来的 `outputs/lan-dev/pgdata`。若需将旧开发机的测试库迁到它自己的“文档”，先保存单据并退出联网客户端，再在旧开发机执行 `scripts/move-lan-data-to-documents.ps1`。

附件二进制和表单记录也存到服务器。生成、导出或打开 Excel/Word 文件仍在当前操作电脑上进行；本地归档路径不能当作另一台电脑的共享目录。

早期 `wms_icebeauty_demo`、`wms_meijingling_demo` 查询试验库保留，原单机 SQLite 数据未迁移、未删除。不同产品不能交叉登录。联网模式在打开本地 SQLite 文件之前分流。

主机“文档”下的 `WMS_OL\credentials.json` 与数据库配套保存。其中的 `admin` 字段用于初始化管理员和预填主机测试登录框。修改软件登录密码后以数据库中的新密码为准。文件中的 `postgres` 和 `app` 为独立生成的数据库口令，仅供主机使用。客户端只需要服务器地址和软件账号。

## 从旧单机版导入历史数据

每个产品可从一个对应的旧 SQLite 库导入到新主机上刚初始化的业务库。两台旧电脑如果各有独立的业务记录，需要先确定哪一份是完整的源库。这个入口只支持整库导入。

1. 在旧电脑上打开单机版，查看主窗口底部状态栏的「数据库」路径，或打开「系统设置 → 数据库文件」核对。冰美肌默认文件名为 `ice_beauty_wms.db`，美鲸灵为 `mei_jing_ling_wms.db`。
2. 退出所有使用这份旧库的程序，将 `.db` 复制到新主机的备份文件夹，例如 `D:\旧库备份\冰美肌`。同目录若还有该文件对应的 `-wal`、`-shm` 或 `-journal` 文件，请一起复制并保留原名。保留原电脑上的文件。历史 Excel 归档需要时从原填单电脑的「文档」另行备份。
3. 解压带有「导入旧版数据库.cmd」的新主机包，双击「启动主机后台.cmd」并等待就绪。在创建物料、库位、账号或单据之前导入。各台电脑先保存工作并退出目标产品的联网前台。
4. 双击「导入旧版数据库.cmd」，输入 `1` 选冰美肌或 `2` 选美鲸灵，再选择刚复制的主 `.db` 文件。核对显示的源路径、目标产品和数据目录，输入 `YES` 开始。过程中目标产品的后台会短暂停止，导入结束后恢复原监听方式。另一个产品可继续使用。
5. 成功后重新打开联网前台，用旧版账号和原密码登录，核对物料数、主要库位的库存、近期出入库和生产单号、库内附件。登录框预填的测试密码可能需要手动替换。导入后的数据由新主机「文档\WMS_OL\pgdata」内的 PostgreSQL 管理。

导入工具只读打开旧库，在临时快照上校验并升级。它支持本项目结构版本 1 至 21。版本 20 以前的旧库没有产品身份，只支持导入冰美肌。美鲸灵需要旧库自带相应产品标记。

目标库存在业务记录或改动过的初始配置时，导入程序会停止。请保留目标库并核对使用的是哪台主机、哪个产品。导入失败会撤销本次写入，修正窗口提示的问题后可重试。

需要通过命令执行时，可在新主机包目录运行下列命令。去掉 `-ConfirmImport` 会在显示源和目标后请求确认。

```powershell
.\scripts\import-legacy-data.ps1 -Product IceBeauty -SourceDatabase 'D:\旧库备份\冰美肌\ice_beauty_wms.db' -ConfirmImport
```

## 构建与运行

本机环境：Qt 6.5.3 / MinGW 11.2、PostgreSQL 17.12。PostgreSQL 解压在 `outputs/lan-dev/postgres/pgsql`。

Qt Creator 调试后台时，重新构建并选择 `WmsServer` 运行即可。未设置连接环境变量时，程序从所属项目/主机目录自动读取现有数据位置和数据库配置、加载 PostgreSQL DLL，并在需要时启动已有数据库。默认运行冰美肌（18080）；运行参数填写 `--product MEI_JING_LING` 则运行美鲸灵（18081）。不需要在 Qt Creator 中手工填写数据库口令或依赖路径。

此入口依赖配套的 `scripts/prepare-lan-server.ps1` 及已有 PostgreSQL 运行库。首次部署仍用“启动主机后台.cmd”初始化；直接调试入口只连接已有库，核对实际数据目录与产品归属，不会因找不到配置而创建新库。显式设置了 `WMS_PG_*` 连接变量时按该配置运行，数据库名、用户名、口令必须完整；可用 `WMS_PG_BIN` 指定另一处 PostgreSQL 客户端 DLL 目录。驱动加载、配置缺失和连接失败分别提示。

```powershell
$env:PATH = 'D:/Qt/Tools/mingw1120_64/bin;D:/Qt/6.5.3/mingw_64/bin;' + $env:PATH
cmake -S . -B build-lan-business -G 'MinGW Makefiles' -DCMAKE_PREFIX_PATH=D:/Qt/6.5.3/mingw_64 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-lan-business --target WmsServer IceBeautyWms MeiJingLingWms --parallel 4
./scripts/start-lan-client.ps1 -Product IceBeauty -StartLocalServer
./scripts/start-lan-client.ps1 -Product MeiJingLing -StartLocalServer
```

只启动后台：`scripts/start-lan-dev.ps1 -Product IceBeauty`。停止开发后台：`scripts/stop-lan-dev.ps1`；加 `-StopDatabase` 同时停止 PostgreSQL。停止不会清空数据，切换后台构建版本前先停止旧后台。

18080/18081 被旧包或其他构建目录的后台占用时，双击项目根目录的 `一键关闭仓库后台.cmd`。它会根据实际监听进程核对 WmsServer 路径、产品和启动时间，关闭后确认端口释放，不依赖当前目录的 PID 文件或后台健康检查，也不会停止 PostgreSQL。权限不足时自动发起一次 Windows 提权；端口属于其他程序时保留该程序并显示占用信息。只关闭冰美肌可运行 `scripts/stop-wms-listeners.ps1 -Product IceBeauty`，美鲸灵用 `-Product MeiJingLing`。新主机包也包含这个入口。

只构建桌面程序可设置 `-DWMS_BUILD_SERVER=OFF`。单机入口保留，`--local` 直接进入原单机登录。联网客户端使用 `--server http://127.0.0.1:18080` 参数，另一套系统端口为 18081。

直接运行 EXE 需要 Qt 运行库在 PATH 中；双击入口已处理。第二台电脑需要带 Qt 运行库的部署包，不能只复制一个 EXE。

## 第二台电脑联调

在项目根目录运行 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1`。发布目录只保留 `dist/WMS仓库系统.zip`，每次验证通过后原位替换。可用 `-ServerAddress` 预填主机的局域网 IPv4 地址。

将同一个 ZIP 解压到主机和其他电脑。主机使用包根目录的启动入口。其他电脑进入 `client`，在 `server-address.txt` 中填写实际主机地址，再双击相应的联网版入口。


运行客户端不需要安装 Qt、Git 或 PostgreSQL。业务单据按 JSON 模板直接生成 .xlsx，可用 WPS 表格或 Microsoft Excel 打开。主机必须开机且后台运行；一台录入测试入库，另一台点“刷新”核对单据和库存。连接不通时，在第二台浏览器打开 `http://主机IP:18080/health`（美鲸灵为 18081），检查后台、IP、端口和访客 Wi-Fi 隔离。

以下是手工启动方法：

先停止对应后台，再用 `start-lan-dev.ps1 -Product IceBeauty -AllowLan` 启动。客户端填写主机实际局域网 IP 和端口。Windows 防火墙需要允许对应后台端口，数据库 55432 始终只监听本机。

开发入口使用 HTTP，适合受信任局域网内的测试数据。正式部署仍需 HTTPS、Windows 服务、备份恢复及第二台实体电脑验收。本次没有自动开放防火墙或安装系统服务。

## 实现与一致性

- v2 登录校验产品、账号和密码，每次数据请求重新校验账号及权限。服务器禁用单机 Debug 的空密码便利入口。
- RemoteBackend / QueryBridge 将原 Qt SQL 模型和基础资料维护接到受限 HTTP 通道。查询不返回密码哈希，客户端不能直接改写库存业务表。
- 38 个类型化服务适配器在服务器执行库存、送检、附件、签收和用户操作，复用原有库存规则和业务事务。
- PostgreSQL 包装层处理 SQLite 查询差异，支持嵌套保存点；多步骤维护和导入保持同一事务，失败回滚。
- 当前使用事务级 advisory lock 按数据库串行化业务写入，面向当前 2～5 台电脑，不代表已完成大量并发优化。跨请求写事务占用期间，其他写入提示稍后重试。
- 写请求 UUID、参数摘要和成功结果与业务数据一并提交，同请求重试不会重复入账。响应持续丢失时前台提示先查询单据。审计操作人取自登录会话。
- 桌面保持同步查询和提交接口，并提供超时提示。单机“测试数据清理/SQLite 文件备份”不对联网库开放，数据库备份应在主机执行。
- 重复初始化拒绝覆盖已有业务库。主机提供旧版 SQLite 整库导入，适用于对应产品已完成空库初始化的业务库。

## 验证

先启动开发 PostgreSQL，再执行：

```powershell
$env:PYTHONIOENCODING='utf-8'
python tests/business_integration.py
python tests/lan_integration.py
python tests/legacy_import_integration.py
python tests/transfer_integration.py
```

本次最终验证：完整业务 60 项、基础回归 49 项，共 109 项通过。

旧库导入另使用合成 SQLite 和随机 PostgreSQL 测试库验证：双产品历史数据、附件字节、原密码与操作人、WAL 中已提交记录、源文件不变、非空库/重复导入拦截、失败回滚、旧结构升级，以及导入后的实际 HTTP 出库。可设置 `WMS_TEST_BUILD` 指向单独的服务器构建目录；样例 SQLite 由 `build-lan-business` 中的原单机 EXE 创建。

多明细调拨测试覆盖双产品同单多行、后续行库存不足、同一库存累计超量、SN 重复或缺失、整单回滚、重复请求、修改、逐行和整单撤销、旧客户端请求格式。构建 `build-lan-client` 后运行 `python tests/build_transfer_page_smoke.py`，再将 `WMS_TRANSFER_UI_TEST` 指向生成的测试 EXE，运行 `tests/transfer_integration.py` 会实际填写调拨界面、点击确认并核对两行同属一个单号。

原材料入库表单在数量之后增加逐行供应商，备注保留在最后。入库页供应商为默认值，送检明细或已有批次可带出供应商，在表单确认窗口双击供应商可逐行修改；采购、退料、其他和期初入库均适用。供应商随入库保存到批次及表单快照，修改无关字段后重新生成保留逐行值，修改单头供应商则更新全单。成品入库保留原模板。原材料表单横向打印、重复表头，短单据收紧空白行；已有 Excel 文件不会自动改写。

供应商测试：`python tests/build_transfer_page_smoke.py inbound_supplier_smoke` 后，设置 `WMS_TEST_BUILD=build-lan-password-server`，运行 `python tests/inbound_supplier_integration.py`，验证两产品实际入库界面、逐行填写、批次及附件保存、修改与重生成。测试使用随机数据库和唯一单号，生成的测试归档随后清除。`powershell -NoProfile -ExecutionPolicy Bypass -File tests/inbound_supplier_forms.ps1` 用 Excel 验证两产品原材料/成品模板的 1 行与 22 行输出、供应商默认值、备注、数量和签字栏，测试文件位于 `outputs/inbound-supplier/forms`。

测试创建随机命名的独立数据库，结束后删除，不读写正式 SQLite 或人工测试库。覆盖双产品库存和生产流程、送检附件、修改撤销、调拨盘点、BOM、表单记录、权限、重复请求、并发出库和实际桌面 23 个页面。桌面验证也通过原 C++ 服务适配器执行提交、读取、修订和撤销。基础测试覆盖登录会话、产品隔离及单机初始化回归。

服务字段变更后使用 `scripts/generate-business-rpc.py` 更新协议及分派代码。`scripts/generate-postgres-schema.py` 仅用于从全新空 SQLite 基线生成静态结构，不是正式数据迁移工具。

专用客户端隔离验证：`python tests/network_client_isolation.py --package 实际解压目录/WMS/client`。使用新建哨兵 SQLite 文件，检查单机参数被拒绝、联网前后文件内容及时间戳不变，并在没有本机 Qt PATH 的环境中加载完整界面。`tests/profile_isolation.cpp` 只读取路径元数据，验证两产品的配置、数据库默认路径、归档与临时目录分离，以及单机路径保持原值。
