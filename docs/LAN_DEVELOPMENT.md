# 局域网完整业务版

日期：2026-11-16。单机基线 main / `077c94e`；开发分支 `codex/lan-postgresql-foundation`。

## 用完整界面测试

双击项目根目录的 `启动冰美肌联网测试.cmd` 或 `启动美鲸灵联网测试.cmd`。脚本启动所选后台，打开联网登录窗口。本机测试用户名为 admin，密码自动填入；点击“连接并登录”。若自行修改过管理员密码，请手动输入新密码。

登录后是原来的完整主窗口，包括物料、仓库库位、送检、出入库、领料、退料、成品入库、调拨、盘点、流水、统计、批次/SN、附件、表单和用户管理。顶部“刷新”或重新进入页面会读取最新数据。初次测试请创建测试仓库、库位和物料，再录入其他入库。

主机可以同时运行后台和前台。重复打开两个前台窗口，一边录单、另一边刷新，即可测试互通。退出前台不会停止后台。

## 数据位置

| 系统 | PostgreSQL 数据库 | 后台地址 |
|---|---|---|
| 冰美肌 | wms_icebeauty_business_demo | http://127.0.0.1:18080 |
| 美鲸灵 | wms_meijingling_business_demo | http://127.0.0.1:18081 |

完整业务表在各库的 `wms_business` schema。初始仅有管理员、角色、分类、项目和编号规则，没有正式物料或库存。PostgreSQL 数据目录为 `outputs/lan-dev/pgdata`，数据库监听 `127.0.0.1:55432`。

附件二进制和表单记录也存到服务器。生成、导出或打开 Excel/Word 文件仍在当前操作电脑上进行；本地归档路径不能当作另一台电脑的共享目录。

早期 `wms_icebeauty_demo`、`wms_meijingling_demo` 查询试验库保留，原单机 SQLite 数据未迁移、未删除。不同产品不能交叉登录。联网模式在打开本地 SQLite 文件之前分流。

`outputs/lan-dev/credentials.json` 的 admin 字段为测试应用密码，其余字段为本机数据库凭据。整个 outputs 目录已被 Git 忽略，请勿分发给客户端。客户端只需要服务器地址和应用账号。

## 构建与运行

本机环境：Qt 6.5.3 / MinGW 11.2、PostgreSQL 17.12。PostgreSQL 解压在 `outputs/lan-dev/postgres/pgsql`。

```powershell
$env:PATH = 'D:/Qt/Tools/mingw1120_64/bin;D:/Qt/6.5.3/mingw_64/bin;' + $env:PATH
cmake -S . -B build-lan-business -G 'MinGW Makefiles' -DCMAKE_PREFIX_PATH=D:/Qt/6.5.3/mingw_64 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-lan-business --target WmsServer IceBeautyWms MeiJingLingWms --parallel 4
./scripts/start-lan-client.ps1 -Product IceBeauty -StartLocalServer
./scripts/start-lan-client.ps1 -Product MeiJingLing -StartLocalServer
```

只启动后台：`scripts/start-lan-dev.ps1 -Product IceBeauty`。停止开发后台：`scripts/stop-lan-dev.ps1`；加 `-StopDatabase` 同时停止 PostgreSQL。停止不会清空数据，切换后台构建版本前先停止旧后台。

只构建桌面程序可设置 `-DWMS_BUILD_SERVER=OFF`。单机入口保留，`--local` 直接进入原单机登录。联网客户端使用 `--server http://127.0.0.1:18080` 参数，另一套系统端口为 18081。

直接运行 EXE 需要 Qt 运行库在 PATH 中；双击入口已处理。第二台电脑需要带 Qt 运行库的部署包，不能只复制一个 EXE。

## 第二台电脑联调

客户端免安装测试包可用下面命令生成，输出到 outputs 下的新目录及 ZIP；包内有两个客户端、Qt 运行库和表单模板，不含后台、数据库及密码。

```powershell
./scripts/package-lan-client.ps1 -ServerAddress 192.168.0.16
```

上述 IP 是本次检测到的主机地址，重新打包时请按实际地址填写。主机将可信的公司网络设置为“专用网络”后，右键项目根目录的 `启用主机局域网测试.cmd`，选择“以管理员身份运行”。脚本重启两套开发后台使其监听局域网，并添加仅限专用/域网络、本地子网和指定后台程序的 18080/18081 入站规则；不会开放 PostgreSQL 端口。执行前暂停录单，之后前台重新登录。该步骤需在主机完成，不能在客户端执行。

将 ZIP 完整复制、解压到第二台 Windows 64 位电脑，双击包内 `启动冰美肌联网版.cmd` 或 `启动美鲸灵联网版.cmd`。服务器地址已填入。在主机软件的用户管理中新建业务账号，再用该账号登录第二台电脑。主机 IP 变化后修改包内 `server-address.txt`，不能填写 127.0.0.1。

运行客户端不需要安装 Qt、Git 或 PostgreSQL。生成旧版 .xls 表单仍需要 WPS 表格或 Microsoft Excel。主机必须开机且后台运行；一台录入测试入库，另一台点“刷新”核对单据和库存。连接不通时，在第二台浏览器打开 `http://主机IP:18080/health`（美鲸灵为 18081），检查后台、IP、端口和访客 Wi-Fi 隔离。

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
- 重复初始化拒绝覆盖已有业务库。正式 SQLite 全量迁移尚未实现，不能把本次测试库当作正式迁移结果。

## 验证

先启动开发 PostgreSQL，再执行：

```powershell
$env:PYTHONIOENCODING='utf-8'
python tests/business_integration.py
python tests/lan_integration.py
```

本次最终验证：完整业务 60 项、基础回归 49 项，共 109 项通过。

测试创建随机命名的独立数据库，结束后删除，不读写正式 SQLite 或人工测试库。覆盖双产品库存和生产流程、送检附件、修改撤销、调拨盘点、BOM、表单记录、权限、重复请求、并发出库和实际桌面 23 个页面。桌面验证也通过原 C++ 服务适配器执行提交、读取、修订和撤销。基础测试覆盖登录会话、产品隔离及单机初始化回归。

服务字段变更后使用 `scripts/generate-business-rpc.py` 更新协议及分派代码。`scripts/generate-postgres-schema.py` 仅用于从全新空 SQLite 基线生成静态结构，不是正式数据迁移工具。
