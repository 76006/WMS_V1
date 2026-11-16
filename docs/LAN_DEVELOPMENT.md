# 局域网改造：第一阶段

日期：2026-11-16。当前单机版源码已保存并推送到 `main` 的 `077c94e`；本阶段位于 `codex/lan-postgresql-foundation`。

## 已完成的范围

- 新增独立的 `WmsServer.exe`，使用 Qt HTTP Server、Qt SQL 的 QPSQL 驱动和 PostgreSQL。
- 两套桌面程序新增“局域网查询”启动入口，支持填写服务器地址、登录、分页查询库存和每 15 秒自动刷新。
- 请求异步执行，超时后显示连接错误并清空旧结果；权限撤销即时生效，账号停用后会话失效。
- 两套系统使用不同数据库和服务端口，启动、登录、查询均核对系统身份。
- 保留单机入口；`--local` 直接进入原单机流程。联网模式在打开任何本地 SQLite 文件之前分流。

本阶段是使用测试数据的联网查询试用版，尚未接入正式库存、完整角色管理、出入库、生产、盘点、附件和表单。测试库使用独立的 `wms_lan` schema，不能作为既有 SQLite 全量迁移结果。正式写入上线前需要完成库存行锁、编号并发、幂等提交、审计和完整迁移验证。

## 本机开发环境

本次验证使用 Qt 6.5.3 / MinGW 11.2 和 PostgreSQL 17.12 Windows x64。PostgreSQL 压缩包来自 [PostgreSQL 官方下载页推荐的 EDB 二进制发行渠道](https://www.enterprisedb.com/download-postgresql-binaries)。实际下载文件 SHA-256：

```text
A2CD8A774389347B70BE7748AC4A37EF43CB6EE539C5FC3E85AE264B15E4A04C
```

这是本次下载文件的校验记录，不是另行取得的发行商签名验证。运行文件解压到 `outputs/lan-dev/postgres/pgsql`，数据目录位于 `outputs/lan-dev/pgdata`。整个 `outputs` 目录已被 Git 忽略。

构建（在项目根目录执行）：

```powershell
$env:PATH = 'D:\Qt\Tools\mingw1120_64\bin;D:\Qt\6.5.3\mingw_64\bin;' + $env:PATH
cmake -S . -B build-lan -G 'MinGW Makefiles' -DCMAKE_PREFIX_PATH=D:/Qt/6.5.3/mingw_64 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-lan --target WmsServer IceBeautyWms MeiJingLingWms --parallel 4
```

只构建桌面程序且不安装 Qt HttpServer 时，可配置 `-DWMS_BUILD_SERVER=OFF`。联网客户端仍需要 Qt Network。

## 启动、使用和停止

```powershell
.\scripts\start-lan-dev.ps1 -Product IceBeauty
.\scripts\start-lan-dev.ps1 -Product MeiJingLing
```

脚本创建隔离的 PostgreSQL 开发实例（只监听 `127.0.0.1:55432`）、每个产品独立的数据库和非超级用户数据库账号，然后隐藏运行后台。首次初始化有一条数量为 100 的明确标注“测试物料”的记录。重复初始化会拒绝执行，避免覆盖已有数据。

| 产品 | 测试数据库 | 后台地址 |
|---|---|---|
| 冰美肌 | `wms_icebeauty_demo` | `http://127.0.0.1:18080` |
| 美鲸灵 | `wms_meijingling_demo` | `http://127.0.0.1:18081` |

测试应用用户名为 `admin`。随机生成的密码在 `outputs/lan-dev/credentials.json` 的 `admin` 字段；数据库凭据在同一文件中。该文件只用于开发，保留在主机，禁止提交或随客户端分发。客户端不需要 PostgreSQL 账号密码。

从 PowerShell 启动编译后的客户端时，将 Qt 的 `bin` 加入 PATH：

```powershell
$env:PATH = 'D:\Qt\6.5.3\mingw_64\bin;' + $env:PATH
.\build-lan\IceBeautyWms.exe --server http://127.0.0.1:18080
.\build-lan\MeiJingLingWms.exe --server http://127.0.0.1:18081
```

关闭前台不会结束后台。停止脚本按记录的 PID、进程路径和启动时间匹配，仅停止本项目启动的后台：

```powershell
.\scripts\stop-lan-dev.ps1 -Product IceBeauty
.\scripts\stop-lan-dev.ps1 -Product All -StopDatabase
```

开发脚本不安装 Windows 服务、不配置防火墙、不设置自动启动，也不生成发行包。

## 局域网试用

本机验证默认只监听回环地址。需要第二台电脑试用时，先停止对应后台，再用 `-AllowLan` 启动，使后台监听 `0.0.0.0`；客户端填写主机实际局域网 IP。仅对可信局域网测试数据开放对应的 18080 / 18081 端口，PostgreSQL 的 55432 端口保持本机可用。

`-AllowLan` 明确允许测试 HTTP 明文通信。正式部署使用 HTTPS；服务端支持 `--tls-cert` 和 `--tls-key`（PEM 证书和未加密 RSA 私钥，文件仅主机服务账号可读）。客户端使用系统证书验证，不跳过证书错误。正式服务、证书和安装包的部署属于后续阶段。

## 后台接口与配置

后台读取 `WMS_PG_HOST`、`WMS_PG_PORT`、`WMS_PG_DATABASE`、`WMS_PG_USER`、`WMS_PG_PASSWORD`。数据库名、用户名、密码必填。`--product` 支持 `ICE_BEAUTY` 和 `MEI_JING_LING`。首次 `--init-demo` 还要求 `WMS_DEMO_ADMIN_PASSWORD` 长度至少 12 个字符。

| 接口 | 用途 |
|---|---|
| `GET /health` | 检查数据库连接、产品身份和接口版本 |
| `POST /api/v1/login` | JSON：`product`、`username`、`password`，返回会话令牌 |
| `GET /api/v1/inventory` | Bearer 令牌；可选 `q`、`offset`、`limit`（1～200） |
| `POST /api/v1/logout` | 撤销当前令牌 |

令牌有效期 8 小时，只保存在服务端内存与客户端内存；服务重启后需要重新登录。每个来源 IP 每分钟最多 10 次登录尝试。库存数量以十进制字符串传输。后台目前在单个事件线程执行有超时限制的数据库查询；多工作线程和写事务并发属于后续阶段。

## 验证

先启动冰美肌开发环境，再执行：

```powershell
python tests/lan_integration.py
```

脚本使用独立随机命名的临时 PostgreSQL 数据库，结束时清理；临时 SQLite 回归文件保留在忽略目录。覆盖真实客户端登录查询、五个并发读取、权限撤销、账号停用、退出登录、查询参数、系统身份、数据库连接中断恢复和两套原单机初始化。本次执行 49 项检查全部通过，三个目标 Debug 构建成功；另行验证了开发环境完整停止、重启与重复启动识别。

本机多客户端测试不能替代两台实体电脑的网络、防火墙和证书验收。上线前还需实际第二台电脑验证和正式数据迁移对账。
