# WMS 全功能收尾实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** 完成开发进度中除第一阶段明确不开发的备份恢复外全部功能，生成可运行发布包并推送 GitHub。

**Architecture:** Excel 功能集中到无 Excel 依赖的 OOXML 数据交换服务；基础资料和系统管理写操作分别由服务层校验并记录审计。现有 SQLite 通过版本 3 迁移补充物料图片和细化权限数据，发布阶段使用 Qt 官方部署工具收集运行依赖。

**Tech Stack:** C++17、Qt 6.10 Widgets/SQL/XML、SQLite、CMake/CTest、PowerShell Zip、windeployqt。

## Global Constraints

- Windows 64 位单机应用，数据继续保存在本地 SQLite。
- 不依赖安装 Microsoft Excel，不引入网络服务。
- 不实现第一阶段已明确排除的备份恢复。
- 所有新增写操作必须校验权限并写审计日志。
- 每个批次全量构建并运行全部测试，失败直接修复。

---

### Task 1: Excel 数据交换

**Files:** Create `src/import/XlsxExporter.h/.cpp`, modify `LegacyInventoryImporter`, `ExcelImportPage`, `CMakeLists.txt`, add `tests/ExcelDataExchangeTests.cpp`.

- [x] 实现单工作表 OOXML 导出器，字符串使用 inlineStr，数量使用数值单元格。
- [x] 增加标准物料模板、通用期初库存模板和当前库存导出按钮。
- [x] 让导入器识别通用期初库存模板并保持警告/错误分级。
- [x] 自动生成并解析测试工作簿，验证中文、编码、批次和数值。

### Task 2: 附件和基础资料完善

**Files:** Modify `AttachmentPage`, `MaterialDialog/Page`, `WarehousePage`; create migration `003_final_features.sql`.

- [x] 附件支持图片/PDF应用内预览，并通过系统默认程序打开 Word/Excel；临时文件隔离命名。
- [x] 增加物料图片表、图片上传/查看/删除。
- [x] 增加仓库与库位编辑、启用和停用，禁止停用仍有库存的库位或仓库。
- [x] 添加迁移重复执行与业务约束测试。

### Task 3: 权限、编号和审计

**Files:** Modify `Session`, `LoginDialog`, `MainWindow`, `SystemSettingsPage`; create `AuditLogPage` and management service/tests.

- [x] 细分物料、库存业务、生产、附件、系统管理权限并应用到导航和按钮。
- [x] 系统设置支持查看和修改单据前缀、流水宽度，禁止非法或重复前缀。
- [x] 增加操作日志查询页，支持用户、动作、实体和日期筛选。
- [x] 覆盖非管理员拒绝、编号规则校验和审计查询测试。

### Task 4: 发布与最终验收

**Files:** Modify `CMakeLists.txt`, create `scripts/package-windows.ps1`, update `README.md` and `DEVELOPMENT_STATUS.md`.

- [x] 添加安装规则和 Windows 依赖部署脚本，生成便携发布目录与 zip 包。
- [x] 执行 Debug 全量构建、全部测试和 UI 冒烟测试。
- [x] 执行 Release 构建并生成发布包，验证发布目录程序可启动建库。
- [x] 清空尚未开发清单（仅保留明确排除项），提交并推送 GitHub `main`。
