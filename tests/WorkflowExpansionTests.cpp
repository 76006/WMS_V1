#include "core/DatabaseConfig.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"
#include "import/LegacyInventoryImporter.h"
#include "services/InventoryService.h"

#include <QFileInfo>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QVariant scalar(QSqlDatabase database, const QString &sql)
{
    QSqlQuery query(database);
    return query.exec(sql) && query.next() ? query.value(0) : QVariant();
}

qlonglong insertMaterial(QSqlDatabase database, const QString &code,
                         qlonglong warehouseId, qlonglong locationId)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO materials(code,name,category_id,unit,default_warehouse_id,default_location_id) "
        "VALUES(?,?,(SELECT id FROM material_categories WHERE code='RAW'),'个',?,?)"));
    query.addBindValue(code);
    query.addBindValue(code + QStringLiteral("名称"));
    query.addBindValue(warehouseId);
    query.addBindValue(locationId);
    return query.exec() ? query.lastInsertId().toLongLong() : 0;
}
}

class WorkflowExpansionTests final : public QObject
{
    Q_OBJECT
private slots:
    void multiLineImportTransferAndCount();
    void parseProvidedLegacyWorkbook();
};

void WorkflowExpansionTests::multiLineImportTransferAndCount()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DatabaseConfig config;
    config.filePath = directory.filePath(QStringLiteral("workflow-expansion.db"));
    DatabaseManager manager;
    QString error;
    QVERIFY2(manager.open(config, &error), qPrintable(error));
    QVERIFY2(SchemaMigrator::migrate(manager.database(), &error), qPrintable(error));
    const qlonglong userId = scalar(manager.database(),
        QStringLiteral("SELECT id FROM users WHERE username='admin'")).toLongLong();

    QSqlQuery warehouse(manager.database());
    QVERIFY(warehouse.exec(QStringLiteral("INSERT INTO warehouses(code,name) VALUES('WH1','测试仓')")));
    const qlonglong warehouseId = warehouse.lastInsertId().toLongLong();
    QSqlQuery location(manager.database());
    location.prepare(QStringLiteral("INSERT INTO locations(warehouse_id,code,name) VALUES(?,'L1','库位1')"));
    location.addBindValue(warehouseId);
    QVERIFY(location.exec());
    const qlonglong locationId = location.lastInsertId().toLongLong();
    location.prepare(QStringLiteral("INSERT INTO locations(warehouse_id,code,name) VALUES(?,'L2','库位2')"));
    location.addBindValue(warehouseId);
    QVERIFY(location.exec());
    const qlonglong location2Id = location.lastInsertId().toLongLong();
    const qlonglong materialA = insertMaterial(manager.database(), QStringLiteral("MAT-A"),
                                               warehouseId, locationId);
    const qlonglong materialB = insertMaterial(manager.database(), QStringLiteral("MAT-B"),
                                               warehouseId, locationId);
    QVERIFY(materialA > 0 && materialB > 0 && userId > 0);

    InventoryService service(manager.database(), userId);
    StockMovementRequest lineA;
    lineA.materialId = materialA;
    lineA.quantity = 10;
    lineA.warehouseId = warehouseId;
    lineA.locationId = locationId;
    StockMovementRequest lineB = lineA;
    lineB.materialId = materialB;
    lineB.quantity = 5;
    StockDocumentRequest inbound;
    inbound.documentType = QStringLiteral("CGRK");
    inbound.documentDate = QDate::currentDate();
    inbound.submissionToken = QStringLiteral("multi-in-1");
    inbound.lines = {lineA, lineB};
    PostedDocument inboundDocument;
    QVERIFY2(service.postStockDocument(inbound, true, &inboundDocument, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT COUNT(*) FROM business_document_items WHERE document_id=%1")
        .arg(inboundDocument.documentId)).toInt(), 2);
    QVERIFY(!service.postStockDocument(inbound, true, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("重复")) || error.contains(QStringLiteral("已经提交")));

    StockDocumentRequest invalidOutbound = inbound;
    invalidOutbound.documentType = QStringLiteral("QTCK");
    invalidOutbound.submissionToken = QStringLiteral("multi-out-invalid");
    invalidOutbound.lines[0].quantity = 2;
    invalidOutbound.lines[1].quantity = 99;
    QVERIFY(!service.postStockDocument(invalidOutbound, false, nullptr, &error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT quantity FROM stock_balances WHERE material_id=%1 AND location_id=%2")
        .arg(materialA).arg(locationId)).toDouble(), 10.0);

    StockDocumentRequest outbound = invalidOutbound;
    outbound.submissionToken = QStringLiteral("multi-out-1");
    outbound.lines[1].quantity = 1;
    QVERIFY2(service.postStockDocument(outbound, false, nullptr, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT quantity FROM stock_balances WHERE material_id=%1 AND location_id=%2")
        .arg(materialA).arg(locationId)).toDouble(), 8.0);

    InitialInventoryRequest initial;
    initial.documentDate = QDate::currentDate();
    initial.sourceFile = QStringLiteral("legacy.xlsx");
    initial.submissionToken = QStringLiteral("legacy-file-hash-location");
    initial.warehouseId = warehouseId;
    initial.locationId = locationId;
    InitialInventoryLine imported;
    imported.materialCode = QStringLiteral("LEGACY-01");
    imported.materialName = QStringLiteral("旧库存物料");
    imported.categoryCode = QStringLiteral("RAW");
    imported.batchNo = QStringLiteral("OLD-BATCH");
    imported.quantity = 3;
    initial.lines = {imported};
    QVERIFY2(service.importInitialInventory(initial, nullptr, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT s.quantity FROM stock_balances s JOIN materials m ON m.id=s.material_id "
        "WHERE m.code='LEGACY-01'")).toDouble(), 3.0);
    QVERIFY(!service.importInitialInventory(initial, nullptr, &error));

    TransferRequest transfer;
    transfer.documentDate = QDate::currentDate();
    transfer.materialId = materialA;
    transfer.quantity = 1;
    transfer.warehouseId = warehouseId;
    transfer.locationId = locationId;
    transfer.targetWarehouseId = warehouseId;
    transfer.targetLocationId = location2Id;
    QVERIFY2(service.postTransfer(transfer, nullptr, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT quantity FROM stock_balances WHERE material_id=%1 AND location_id=%2")
        .arg(materialA).arg(location2Id)).toDouble(), 1.0);

    InventoryCountRequest count;
    count.documentDate = QDate::currentDate();
    count.submissionToken = QStringLiteral("count-1");
    InventoryCountLine countA;
    countA.materialId = materialA;
    countA.warehouseId = warehouseId;
    countA.locationId = locationId;
    countA.systemQuantity = 7;
    countA.actualQuantity = 6;
    countA.differenceReason = QStringLiteral("盘亏测试");
    InventoryCountLine countB;
    countB.materialId = materialB;
    countB.warehouseId = warehouseId;
    countB.locationId = locationId;
    countB.systemQuantity = 4;
    countB.actualQuantity = 6;
    countB.differenceReason = QStringLiteral("盘盈测试");
    count.lines = {countA, countB};
    PostedDocument countDocument;
    QVERIFY2(service.postInventoryCount(count, &countDocument, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT quantity FROM stock_balances WHERE material_id=%1 AND location_id=%2")
        .arg(materialA).arg(locationId)).toDouble(), 6.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT COUNT(*) FROM inventory_count_items WHERE inventory_count_id="
        "(SELECT id FROM inventory_counts WHERE document_id=%1)").arg(countDocument.documentId)).toInt(), 2);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT COUNT(*) FROM inventory_ledger WHERE document_id=%1")
        .arg(countDocument.documentId)).toInt(), 2);

    count.submissionToken = QStringLiteral("count-stale");
    QVERIFY(!service.postInventoryCount(count, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("已变化")));
}

void WorkflowExpansionTests::parseProvidedLegacyWorkbook()
{
#ifndef Q_OS_WIN
    QSKIP("旧库存解析器当前使用Windows内置解压能力。");
#else
    const QString path = QStringLiteral("D:/WMS/冰美肌库存-0629.xlsx");
    if (!QFileInfo::exists(path)) QSKIP("未提供旧库存工作簿。");
    QList<LegacyImportRow> rows;
    QString error;
    QVERIFY2(LegacyInventoryImporter::parseFile(path, &rows, &error), qPrintable(error));
    int ready = 0;
    int negative = 0;
    for (const LegacyImportRow &row : std::as_const(rows)) {
        if (row.status == LegacyImportStatus::Ready) ++ready;
        if (row.status == LegacyImportStatus::Error && row.quantity < 0) ++negative;
    }
    QVERIFY2(ready > 0, "应解析出可导入的正库存记录");
    QVERIFY2(negative > 0, "应识别旧表中的负库存记录");
#endif
}

QTEST_GUILESS_MAIN(WorkflowExpansionTests)
#include "WorkflowExpansionTests.moc"
