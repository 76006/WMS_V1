# Production Workflow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an atomic multi-material production issue, source-linked partial return, and finished-goods receipt workflow.

**Architecture:** Extend the existing `InventoryService` with document-level requests while preserving single-line APIs. Add schema version 2 for production runs and source links, then replace the three production placeholder pages with focused Qt Widgets pages that share stock-line selection behavior.

**Tech Stack:** C++17, Qt 6.5 Widgets/SQL/Test, SQLite, CMake, CTest

## Global Constraints

- Windows single-machine Qt Widgets application using QSQLITE.
- Inventory is never edited directly; every quantity change writes an inventory ledger row.
- A multi-line document commits atomically and never permits negative stock.
- Batch and SN rules remain enforced at service level.
- Posted records are corrected by reversal or production return, never direct deletion.
- No BOM calculation, approvals, financial data, scanning, attachments, or backup work in this increment.
- The current archive checkout has no `.git`; commit steps run only after Git metadata is restored.

---

### Task 1: Add schema version 2 migration

**Files:**
- Create: `database/migrations/002_production_workflow.sql`
- Modify: `src/database/SchemaMigrator.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/DatabaseTests.cpp`

**Interfaces:**
- Produces: `production_runs`, `business_documents.production_run_id`, `business_documents.submission_token`, and `business_document_items.source_item_id`.

- [x] **Step 1: Write migration tests**

Add a test that initializes a version-1 database, inserts a material, runs `SchemaMigrator::migrate`, and asserts `schema_migrations` contains version 2 plus the new table and columns via `PRAGMA table_info`.

- [x] **Step 2: Run the migration test and verify failure**

Run: `ctest --test-dir build --output-on-failure -R WmsDatabaseTests`

Expected: FAIL because version 2 and `production_runs` do not exist.

- [x] **Step 3: Add the migration resource and runner**

The SQL creates:

```sql
CREATE TABLE production_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    batch_no TEXT NOT NULL COLLATE NOCASE UNIQUE,
    product_material_id INTEGER NOT NULL REFERENCES materials(id),
    product_name TEXT NOT NULL,
    product_model TEXT NOT NULL DEFAULT '',
    planned_quantity NUMERIC NOT NULL CHECK (planned_quantity > 0),
    status TEXT NOT NULL DEFAULT 'OPEN' CHECK (status IN ('OPEN', 'COMPLETED')),
    created_by INTEGER NOT NULL REFERENCES users(id),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);
ALTER TABLE business_documents ADD COLUMN production_run_id INTEGER REFERENCES production_runs(id);
ALTER TABLE business_documents ADD COLUMN submission_token TEXT COLLATE NOCASE;
ALTER TABLE business_document_items ADD COLUMN source_item_id INTEGER REFERENCES business_document_items(id);
CREATE UNIQUE INDEX idx_documents_submission_token ON business_documents(submission_token) WHERE submission_token IS NOT NULL;
CREATE INDEX idx_documents_production_run ON business_documents(production_run_id, document_type, document_date);
CREATE INDEX idx_items_source_item ON business_document_items(source_item_id);
INSERT INTO schema_migrations(version) VALUES (2);
```

Load and execute this file only when version 2 is absent, inside one transaction.

- [x] **Step 4: Run migration tests**

Expected: PASS for fresh initialization, version-1 upgrade, and repeated migration.

- [ ] **Step 5: Commit**

```bash
git add database/migrations/002_production_workflow.sql src/database/SchemaMigrator.cpp CMakeLists.txt tests/DatabaseTests.cpp
git commit -m "feat: add production workflow migration"
```

### Task 2: Add atomic multi-line document posting

**Files:**
- Modify: `src/services/InventoryService.h`
- Create: `src/services/InventoryServiceProduction.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/DatabaseTests.cpp`

**Interfaces:**
- Produces: `StockDocumentRequest`, `ProductionRunRequest`, `postProductionIssue`, and `postFinishedGoodsInbound`.

- [x] **Step 1: Define request types**

```cpp
struct StockDocumentRequest {
    QString documentType;
    QDate documentDate;
    QString handlerName;
    QString purpose;
    QString notes;
    QString submissionToken;
    qlonglong productionRunId = 0;
    QList<StockMovementRequest> lines;
};

struct ProductionRunRequest {
    QString batchNo;
    qlonglong productMaterialId = 0;
    double plannedQuantity = 0.0;
};

bool postProductionIssue(const ProductionRunRequest &run,
                         const StockDocumentRequest &document,
                         PostedDocument *posted,
                         qlonglong *productionRunId,
                         QString *errorMessage = nullptr);
bool postFinishedGoodsInbound(const StockDocumentRequest &document,
                              PostedDocument *posted,
                              QString *errorMessage = nullptr);
```

- [x] **Step 2: Write failing multi-line tests**

Cover a two-line success, duplicate line identity rejection, one-line insufficient-stock rollback, mixed ordinary/SN lines, and duplicate submission token rejection.

- [x] **Step 3: Implement production run creation and atomic issue posting**

Validate all request headers and duplicate identities, begin one immediate transaction, create or verify the production run, create one `SCLL` document, then create each item with sequential line numbers, decrement balances, write ledgers, and update SN states. Prefix line failures with `第 N 行：` and roll back the whole transaction.

- [x] **Step 4: Run focused tests**

Run: `ctest --test-dir build --output-on-failure -R WmsDatabaseTests`

Expected: all existing and new issue tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/services/InventoryService.h src/services/InventoryServiceProduction.cpp CMakeLists.txt tests/DatabaseTests.cpp
git commit -m "feat: post atomic production issue documents"
```

### Task 3: Implement source-linked production returns and reversal rules

**Files:**
- Modify: `src/services/InventoryService.h`
- Modify: `src/services/InventoryServiceProduction.cpp`
- Modify: `src/services/InventoryService.cpp`
- Modify: `src/ui/pages/LedgerPage.cpp`
- Modify: `tests/DatabaseTests.cpp`

**Interfaces:**
- Produces: `ProductionReturnLine`, `ProductionReturnRequest`, and `postProductionReturn`.

- [x] **Step 1: Define return request types**

```cpp
struct ProductionReturnLine {
    qlonglong sourceItemId = 0;
    double quantity = 0.0;
    qlonglong warehouseId = 0;
    qlonglong locationId = 0;
    QStringList serialNumbers;
    QString notes;
};

struct ProductionReturnRequest {
    qlonglong sourceDocumentId = 0;
    QDate documentDate;
    QString handlerName;
    QString notes;
    QString submissionToken;
    QList<ProductionReturnLine> lines;
};
```

- [x] **Step 2: Write failing return tests**

Test two partial returns, multi-line return, excessive quantity rejection, wrong-source item rejection, wrong SN rejection, and full rollback when a later line fails.

- [x] **Step 3: Implement `postProductionReturn`**

Inside one transaction query each source `SCLL` item, compute `quantity - returned_quantity - reversed_quantity`, create `SCTL` items with `source_item_id`, increase the selected return-location balance, write ledger rows, restore eligible SNs to `IN_STOCK`, and increment each source item's `returned_quantity`.

- [x] **Step 4: Correct generic reversal semantics**

Include `returned_quantity`, `document_type`, `production_run_id`, and `source_item_id` in the source query. Limit `SCLL` reversal by returned quantity; reversing `SCTL` decrements the original source item's returned quantity and restores returned SNs to `OUTBOUND`; reversing `CPRK` triggers production status recalculation. Update the ledger query's available reversal expression to subtract `returned_quantity` for `SCLL`.

- [x] **Step 5: Run return and reversal tests**

Expected: all return, reversal, legacy inventory, and SN tests PASS.

- [ ] **Step 6: Commit**

```bash
git add src/services/InventoryService.h src/services/InventoryService.cpp src/services/InventoryServiceProduction.cpp src/ui/pages/LedgerPage.cpp tests/DatabaseTests.cpp
git commit -m "feat: add source-linked production returns"
```

### Task 4: Implement finished-goods posting and run status

**Files:**
- Modify: `src/services/InventoryServiceProduction.cpp`
- Modify: `src/services/InventoryService.h`
- Modify: `tests/DatabaseTests.cpp`

**Interfaces:**
- Produces: `productionRunReceivedQuantity(qlonglong)` and internal `refreshProductionRunStatus`.

- [x] **Step 1: Write failing finished-goods tests**

Cover partial receipt remaining `OPEN`, cumulative completion, allowed overproduction, SN receipt, and reversal returning the run to `OPEN`.

- [x] **Step 2: Implement finished-goods validation and posting**

Require exactly one line whose material matches `production_runs.product_material_id`, force its batch to the production batch number, post `CPRK` inbound through the same transaction rules, then sum active receipt quantity minus reversed quantity and update the run status.

- [x] **Step 3: Run all database tests**

Expected: PASS with production run totals and statuses matching asserted values.

- [ ] **Step 4: Commit**

```bash
git add src/services/InventoryService.h src/services/InventoryServiceProduction.cpp tests/DatabaseTests.cpp
git commit -m "feat: add finished goods production receipts"
```

### Task 5: Build the reusable stock-line table and production issue page

**Files:**
- Create: `src/ui/widgets/StockLineTable.h`
- Create: `src/ui/widgets/StockLineTable.cpp`
- Create: `src/ui/pages/ProductionIssuePage.h`
- Create: `src/ui/pages/ProductionIssuePage.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- `StockLineTable::setDatabase(QSqlDatabase)`
- `StockLineTable::refreshReferenceData()`
- `StockLineTable::lines(QString *errorMessage) const -> QList<StockMovementRequest>`
- `ProductionIssuePage::stockChanged()`

- [x] **Step 1: Create table component**

Use a `QTableWidget` with columns for material, warehouse, location, batch, available stock, quantity, SN summary, and remove action. Keep SN selection in a panel below the table for the current row and reject duplicate stock identities.

- [x] **Step 2: Create production issue page**

Add product material, batch, planned quantity, date, handler, notes, the reusable table, confirmation summary, submission-token generation with `QUuid`, and a recent `SCLL` list. Disable submit controls for users without warehouse-management permission.

- [x] **Step 3: Build the application**

Run: `cmake --build build`

Expected: `IceBeautyWms.exe` builds without warnings introduced by these files.

- [ ] **Step 4: Commit**

```bash
git add src/ui/widgets/StockLineTable.* src/ui/pages/ProductionIssuePage.* CMakeLists.txt
git commit -m "feat: add multi-material production issue page"
```

### Task 6: Build production return and finished-goods pages

**Files:**
- Create: `src/ui/pages/ProductionReturnPage.h`
- Create: `src/ui/pages/ProductionReturnPage.cpp`
- Create: `src/ui/pages/FinishedGoodsInPage.h`
- Create: `src/ui/pages/FinishedGoodsInPage.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- `ProductionReturnPage::stockChanged()`
- `FinishedGoodsInPage::stockChanged()`

- [x] **Step 1: Create production return page**

Provide production-run and source-document selectors, a checkable source-line table with return quantity and destination controls, an SN panel limited to the selected source item, confirmation, duplicate-submit token, and recent `SCTL` documents.

- [x] **Step 2: Create finished-goods page**

Provide production-run selection, product snapshot, planned/received/remaining display, date/location/operator fields, SN entry and generation, overproduction confirmation, duplicate-submit token, and recent `CPRK` documents.

- [x] **Step 3: Build and smoke construct pages**

Run: `cmake --build build` then `ctest --test-dir build --output-on-failure -R WmsAppSmoke`.

Expected: build and application smoke test PASS.

- [ ] **Step 4: Commit**

```bash
git add src/ui/pages/ProductionReturnPage.* src/ui/pages/FinishedGoodsInPage.* CMakeLists.txt
git commit -m "feat: add production return and receipt pages"
```

### Task 7: Integrate navigation, refresh, permissions, and documentation

**Files:**
- Modify: `src/ui/MainWindow.h`
- Modify: `src/ui/MainWindow.cpp`
- Modify: `src/ui/pages/LedgerPage.cpp`
- Modify: `DEVELOPMENT_STATUS.md`

**Interfaces:**
- Consumes: the three pages' `stockChanged()` signals and `refreshReferenceData()` slots.

- [x] **Step 1: Replace placeholders**

Instantiate the three production pages, retain view access for all authenticated users, connect stock-change signals to `refreshInventoryViews`, and refresh the pages after material, warehouse, or inventory changes.

- [x] **Step 2: Complete ledger filtering and reversal display**

Add `SCTL` and `CPRK` filter labels and show correct remaining reversible quantity for production documents.

- [x] **Step 3: Run the full verification suite**

Run:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=D:\Qt\6.5.3\mingw_64
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: configuration, build, and every CTest target PASS.

- [x] **Step 4: Update progress documentation**

Move production issue, linked partial return, and finished-goods receipt to completed; record atomic multi-line support, migration version 2, test counts, and any verified UI limitations.

- [ ] **Step 5: Commit**

```bash
git add src/ui/MainWindow.* src/ui/pages/LedgerPage.cpp DEVELOPMENT_STATUS.md
git commit -m "feat: complete production inventory workflow"
```
