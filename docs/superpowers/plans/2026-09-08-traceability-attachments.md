# 追溯与附件功能实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 用真实可操作页面替换批次查询、SN 查询和附件管理占位页。

**Architecture:** 批次与 SN 页面只读查询现有库存、流水和序列号关系；附件写操作集中到 `AttachmentService`，统一大小限制、摘要、软删除和审计日志。主窗口负责页面刷新联动。

**Tech Stack:** C++17、Qt 6 Widgets/SQL/Core、SQLite、CMake、Qt Test。

## Global Constraints

- 保持 Windows 单机和 SQLite 本地数据架构。
- 附件以 BLOB 保存到现有 `attachments` 表，不引入第三方依赖。
- 删除采用软删除，可恢复，不直接清除文件数据。
- 所有写操作记录当前用户并生成审计日志。

---

### Task 1: 附件服务

**Files:**
- Create: `src/services/AttachmentService.h`
- Create: `src/services/AttachmentService.cpp`
- Test: `tests/TraceabilityAttachmentTests.cpp`

**Interfaces:**
- Produces: `AttachmentService::uploadDocumentAttachment(...)`
- Produces: `AttachmentService::loadAttachment(...)`
- Produces: `AttachmentService::setDeleted(...)`

- [x] 添加测试：上传附件后校验文件名、大小、SHA-256 和 BLOB 内容。
- [x] 添加测试：软删除后 `is_deleted=1`，恢复后回到 `0`，审计日志同时生成。
- [x] 实现 50 MB 大小限制、业务单据存在性校验和事务提交。
- [x] 运行新测试目标并确认通过。

### Task 2: 批次与 SN 追溯页面

**Files:**
- Create: `src/ui/pages/BatchTracePage.h`
- Create: `src/ui/pages/BatchTracePage.cpp`
- Create: `src/ui/pages/SerialTracePage.h`
- Create: `src/ui/pages/SerialTracePage.cpp`

**Interfaces:**
- Produces: `BatchTracePage::refresh()`，按物料、批次、仓库筛选余额与流水。
- Produces: `SerialTracePage::refresh()`，按 SN、物料、状态、仓库筛选当前位置与历史流水。

- [x] 实现批次汇总表，展示总库存、库位数、首次入库和最后变动时间。
- [x] 实现批次选中后的入出库流水明细。
- [x] 实现 SN 当前状态、位置、最近单据和时间筛选。
- [x] 实现 SN 选中后的全生命周期流水明细。

### Task 3: 附件管理页面与集成

**Files:**
- Create: `src/ui/pages/AttachmentPage.h`
- Create: `src/ui/pages/AttachmentPage.cpp`
- Modify: `src/ui/MainWindow.h`
- Modify: `src/ui/MainWindow.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `AttachmentService` 的上传、下载读取、软删除和恢复接口。
- Produces: 主窗口三个真实页面入口。

- [x] 实现业务单据搜索与选择。
- [x] 实现附件上传、下载、显示已删除、软删除和恢复。
- [x] 替换批次、SN、附件三个占位页面并接入刷新。
- [x] 全量构建并运行全部测试。
- [x] 更新 README 与开发进度，提交并推送 GitHub `main`。
