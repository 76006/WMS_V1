# Inbound Inspection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在在线入库单中增加是否送检分支，送检时可填写、打印送检单，并仅允许带合格附件的单据完成入库。

**Architecture:** 使用新的 `inbound_inspection_details` 表保存每张普通入库单的送检状态及附件关联。`InspectionDialog` 负责在线填写、附件选择和打印，`StockInPage` 负责流程控制，`InventoryService::postStockDocument` 在库存事务内同时保存送检资料和附件。

**Tech Stack:** C++17、Qt 6 Widgets/Sql/PrintSupport、SQLite、CMake

## Global Constraints

- 不恢复或新增 `tests` 目录和测试目标。
- 不执行构建或测试，由用户自行构建验证。
- 送检合格必须上传附件后才能入库；不送检保持原直接入库流程。

---

### Task 1: 数据库送检资料

**Files:**
- Create: `database/migrations/009_inbound_inspections.sql`
- Modify: `database/schema.sql`
- Modify: `src/database/SchemaMigrator.h`
- Modify: `src/database/SchemaMigrator.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `inbound_inspection_details(document_id, requires_inspection, inspection_no, inspection_date, inspector_name, inspection_result, conclusion, inspection_attachment_id)`

- [ ] 新增版本 9 迁移并在新数据库结构中加入同一张表。
- [ ] 将版本 9 迁移加入资源和数据库启动升级链路。

### Task 2: 入库事务保存送检资料

**Files:**
- Modify: `src/services/InventoryService.h`
- Modify: `src/services/InventoryServiceDocuments.cpp`

**Interfaces:**
- Produces: `InboundInspectionRequest`，作为 `StockDocumentRequest::inspection` 传入。

- [ ] 入库前验证送检结果：要求送检时必须为 `QUALIFIED` 且附件非空、文件名有效、大小不超过 50 MB。
- [ ] 创建入库单后，在同一事务写入附件及 `inbound_inspection_details`；不送检写入 `NOT_REQUIRED`。

### Task 3: 在线送检单与打印

**Files:**
- Create: `src/ui/dialogs/InspectionDialog.h`
- Create: `src/ui/dialogs/InspectionDialog.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: 当前 `QList<StockMovementRequest>` 和数据库中的物料、仓库资料。
- Produces: `InboundInspectionRequest InspectionDialog::inspection() const`。

- [ ] 创建送检单表单，提供送检编号、日期、检验员、检验结果、结论及附件选择。
- [ ] 在表单中显示本次入库产品明细。
- [ ] 使用 Qt PrintSupport 打印完整送检单；合格结果在应用时强制校验附件。

### Task 4: 入库页面流程接入

**Files:**
- Modify: `src/ui/pages/StockInPage.h`
- Modify: `src/ui/pages/StockInPage.cpp`

**Interfaces:**
- Consumes: `InspectionDialog` 和 `InboundInspectionRequest`。

- [ ] 增加“是否送检”下拉栏及“填写/查看送检单”按钮，选择“是”打开在线送检单。
- [ ] 提交入库时再次校验合格结果和附件，并将送检资料传入库存事务。
- [ ] 在近期入库单中显示是否送检、送检单号和检验结果，入库成功后清空本次送检状态。
