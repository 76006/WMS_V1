# Business Document Reversal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a safe whole-document revoke action to every historical business-document table, synchronizing document status, inventory, serial numbers, generated-form records, and archived files.

**Architecture:** The inventory service owns the transactional reversal by rebuilding the selected posted document as `REVERSED` without active stock ledgers, after rejecting partial or downstream-linked documents. A shared table action routes the selected document to `MainWindow`, while the template service moves generated local files into a recoverable `已撤销` archive and hides their database attachments from active files.

**Tech Stack:** C++17, Qt 6 Widgets, Qt SQL/SQLite, existing `InventoryService`, `OfficeTemplateService`, and `TableExcelExport` utilities.

## Global Constraints

- Do not run a build or automated tests; the user will compile and verify locally.
- Do not modify or add test-related files.
- Preserve the existing untracked BOM conversion directory and inventory workbook.
- Reversal must be atomic for database stock, serial-number, document-status, and generated-attachment changes.
- Reversal must be blocked for draft, already reversed, partially reversed, or downstream-linked documents.
- Local generated files must be preserved in a recoverable `已撤销` subdirectory rather than deleted.

---

### Task 1: Transactional whole-document reversal

**Files:**
- Modify: `src/services/InventoryService.h`
- Modify: `src/services/InventoryServiceRevision.cpp`

**Interfaces:**
- Consumes: `loadPostedDocument(qlonglong, PostedDocumentEdit *, QString *)` and `revisePostedDocument(const PostedDocumentEdit &, QString *)`.
- Produces: `reversePostedDocument(qlonglong documentId, const QString &handlerName, const QString &reason, QString *documentNumber, QString *errorMessage)`.

- [x] **Step 1: Add the public whole-document reversal interface**

Declare a service method accepting the selected document id, operator-entered handler, and reason, and returning the source document number for confirmation.

- [x] **Step 2: Validate that the document can be fully reversed**

Reject unsupported states and query every line for non-zero `reversed_quantity` or `returned_quantity`; also reject active downstream `source_item_id` references so an existing child transaction cannot be orphaned.

- [x] **Step 3: Rebuild the document as reversed in one transaction**

Pass the unchanged full document snapshot to the existing revision pipeline with `status = 'REVERSED'`, append the structured reversal reason, skip ledger recreation, update related quantities, and soft-delete generated form attachments before commit.

- [x] **Step 4: Preserve auditability**

Ensure the existing full-document revision audit records the before/after state and that the source document remains queryable with status `REVERSED`.

### Task 2: Generated-file reversal archive

**Files:**
- Modify: `src/services/OfficeTemplateService.h`
- Modify: `src/services/OfficeTemplateService.cpp`

**Interfaces:**
- Consumes: stored `document_forms.payload` and `archiveFilePath(const OfficeTemplateDocument &)`, plus the database attachment state changed in Task 1.
- Produces: `archiveReversedDocumentForms(QSqlDatabase database, qlonglong documentId, QStringList *archivedPaths, QStringList *errors)`.

- [x] **Step 1: Load every generated form payload for the reversed document**

Parse each persisted template payload with the existing document parser and calculate its current archive path.

- [x] **Step 2: Move files without deleting evidence**

Create an `已撤销` directory beside each source file and rename it to `已撤销-<原文件名>`; skip already archived files and report locked or inaccessible files as warnings.

- [x] **Step 3: Return precise file results**

Return native Windows paths for moved/already-moved files and collect per-file warnings so the completion dialog can show exactly what happened.

### Task 3: Shared revoke action and confirmation dialog

**Files:**
- Modify: `src/ui/widgets/TableExcelExport.h`
- Modify: `src/ui/widgets/TableExcelExport.cpp`
- Modify: `src/ui/MainWindow.h`
- Modify: `src/ui/MainWindow.cpp`

**Interfaces:**
- Consumes: the selected business-document id stored by existing table models and Task 1/Task 2 service methods.
- Produces: `TableExcelExport::reverseSelectedBusinessDocument(...)` and invokable `MainWindow::reverseDocumentById(qlonglong, QWidget *)`.

- [x] **Step 1: Route the selected row through the shared table helper**

Use the same selected-row id lookup as document editing and display a clear selection warning when no valid row is selected.

- [x] **Step 2: Add a destructive confirmation dialog**

Show the source number/type/status, require a non-empty reversal reason, preserve or edit the handler name, and state that inventory and generated files will be synchronized.

- [x] **Step 3: Enforce warehouse permissions and ownership**

Allow warehouse managers and administrators according to the existing session policy, and retain the current creator/admin restriction used by inventory reversal screens.

- [x] **Step 4: Refresh every affected page**

After success, refresh inventory views, invoke local-file archival, and show the source document number plus archived paths or warnings.

- [x] **Step 5: Add the action to full-screen history views**

For every table marked `businessDocumentTable`, place `撤销单据` next to `修改单据` in the full-screen toolbar and reload/filter after completion.

### Task 4: Revoke buttons above all historical document tables

**Files:**
- Modify: `src/ui/widgets/TableExcelExport.cpp`

**Interfaces:**
- Consumes: `TableExcelExport::reverseSelectedBusinessDocument(...)`.
- Produces: a visible `撤销单据` button above every table carrying `businessDocumentTable = true`.

- [x] **Step 1: Add a shared action bar immediately above each marked table**

Insert the destructive action through the shared installer so every current and future marked business-document table receives it without page-specific duplication.

- [x] **Step 2: Connect each button to its table and the main-window refresh path**

Route each selected row to the main window; its inventory refresh updates every page, while the full-screen action preserves filtering.

- [x] **Step 3: Cover tables without an existing document toolbar**

Insert the shared bar directly before the table, including attachment and shipment-query pages whose existing action rows differ.

### Task 5: Independent inspection-notice reversal

**Files:**
- Modify: `src/services/InspectionService.h`
- Modify: `src/services/InspectionService.cpp`
- Modify: `src/ui/pages/InspectionPage.h`
- Modify: `src/ui/pages/InspectionPage.cpp`

**Interfaces:**
- Consumes: existing `InspectionService::readNotice(...)`, attachment soft deletion, and stored `archive_path`.
- Produces: reasoned `cancelNotice(...)` plus normal/full-screen `撤销通知单` actions.

- [x] **Step 1: Add cancellation actions above pending and historical notice tables**

Require a selected notice, warehouse permission, creator/admin ownership, a reason, and final confirmation.

- [x] **Step 2: Protect linked inventory documents**

Block notices already linked to inbound inventory and direct the operator to revoke the inbound document first.

- [x] **Step 3: Synchronize notice attachments and files**

Soft-delete inspection-notice attachments in the same cancellation transaction and move the local Excel file into a recoverable `已撤销` directory.

### Task 6: Static review and handoff

**Files:**
- Review: all files modified in Tasks 1-4

**Interfaces:**
- Consumes: completed service and UI changes.
- Produces: a clean, reviewable working-tree diff without build/test execution.

- [x] **Step 1: Inspect the final diff**

Check for accidental changes, stale names, duplicate buttons, missing includes, and preservation of user-owned untracked files.

- [x] **Step 2: Verify coverage by source inspection**

Search every `businessDocumentTable` assignment and confirm both normal and full-screen revoke entry points exist.

- [x] **Step 3: Report manual verification points**

Hand off the exact changed behavior and tell the user to compile, select a clean posted document, click `撤销单据`, then verify status, stock, SN state, and the `已撤销` file directory.
