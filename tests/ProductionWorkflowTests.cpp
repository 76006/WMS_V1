#include "core/DatabaseConfig.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"
#include "services/InventoryService.h"

#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QVariant scalar(QSqlDatabase database, const QString &sql)
{
    QSqlQuery query(database);
    if (!query.exec(sql) || !query.next()) {
        return {};
    }
    return query.value(0);
}

qlonglong insertMaterial(QSqlDatabase database,
                         const QString &code,
                         const QString &name,
                         const QString &categoryCode,
                         bool requireSerial,
                         qlonglong warehouseId,
                         qlonglong locationId)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO materials(code,name,specification,category_id,unit,minimum_stock,"
        "default_warehouse_id,default_location_id,require_batch,require_serial) "
        "VALUES(?,?,?,(SELECT id FROM material_categories WHERE code=?),'个',0,?,?,1,?)"));
    query.addBindValue(code);
    query.addBindValue(name);
    query.addBindValue(QStringLiteral("测试规格"));
    query.addBindValue(categoryCode);
    query.addBindValue(warehouseId);
    query.addBindValue(locationId);
    query.addBindValue(requireSerial);
    if (!query.exec()) {
        return 0;
    }
    return query.lastInsertId().toLongLong();
}
}

class ProductionWorkflowTests final : public QObject
{
    Q_OBJECT

private slots:
    void completeProductionInventoryLoop();
};

void ProductionWorkflowTests::completeProductionInventoryLoop()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DatabaseConfig config;
    config.filePath = directory.filePath(QStringLiteral("production-flow.db"));
    DatabaseManager manager;
    QString error;
    QVERIFY2(manager.open(config, &error), qPrintable(error));
    QVERIFY2(SchemaMigrator::migrate(manager.database(), &error), qPrintable(error));
    QVERIFY2(SchemaMigrator::migrate(manager.database(), &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT MAX(version) FROM schema_migrations")).toInt(), 3);

    const qlonglong userId = scalar(manager.database(),
                                    QStringLiteral("SELECT id FROM users WHERE username='admin'"))
                                    .toLongLong();
    QVERIFY(userId > 0);
    QSqlQuery warehouse(manager.database());
    QVERIFY(warehouse.exec(QStringLiteral(
        "INSERT INTO warehouses(code,name) VALUES('PWH','生产测试仓')")));
    const qlonglong warehouseId = warehouse.lastInsertId().toLongLong();
    QSqlQuery location(manager.database());
    location.prepare(QStringLiteral(
        "INSERT INTO locations(warehouse_id,code,name) VALUES(?,'P01','生产库位')"));
    location.addBindValue(warehouseId);
    QVERIFY(location.exec());
    const qlonglong locationId = location.lastInsertId().toLongLong();

    const qlonglong rawId = insertMaterial(manager.database(), QStringLiteral("RAW-01"),
                                            QStringLiteral("普通原料"), QStringLiteral("RAW"),
                                            false, warehouseId, locationId);
    const qlonglong serialRawId = insertMaterial(manager.database(), QStringLiteral("KEY-01"),
                                                  QStringLiteral("关键部件"), QStringLiteral("RAW"),
                                                  true, warehouseId, locationId);
    const qlonglong productId = insertMaterial(manager.database(), QStringLiteral("FG-01"),
                                                QStringLiteral("测试成品"), QStringLiteral("FINISHED"),
                                                true, warehouseId, locationId);
    QVERIFY(rawId > 0);
    QVERIFY(serialRawId > 0);
    QVERIFY(productId > 0);

    InventoryService service(manager.database(), userId);
    StockMovementRequest inboundRaw;
    inboundRaw.documentType = QStringLiteral("CGRK");
    inboundRaw.documentDate = QDate::currentDate();
    inboundRaw.materialId = rawId;
    inboundRaw.quantity = 10.0;
    inboundRaw.batchNo = QStringLiteral("RAW-B01");
    inboundRaw.supplier = QStringLiteral("供应商A");
    inboundRaw.warehouseId = warehouseId;
    inboundRaw.locationId = locationId;
    QVERIFY2(service.postInbound(inboundRaw, nullptr, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT supplier FROM batches WHERE material_id=%1 AND batch_no='RAW-B01'").arg(rawId)).toString(),
        QStringLiteral("供应商A"));

    StockMovementRequest inboundSerial = inboundRaw;
    inboundSerial.materialId = serialRawId;
    inboundSerial.quantity = 2.0;
    inboundSerial.batchNo = QStringLiteral("KEY-B01");
    inboundSerial.serialNumbers = {QStringLiteral("KEY-0001"), QStringLiteral("KEY-0002")};
    QVERIFY2(service.postInbound(inboundSerial, nullptr, &error), qPrintable(error));

    ProductionRunRequest run;
    run.batchNo = QStringLiteral("PROD-202609-001");
    run.productMaterialId = productId;
    run.plannedQuantity = 2.0;
    StockDocumentRequest issue;
    issue.documentType = QStringLiteral("SCLL");
    issue.documentDate = QDate::currentDate();
    issue.handlerName = QStringLiteral("生产员");
    issue.submissionToken = QStringLiteral("issue-token-1");
    StockMovementRequest issueRaw = inboundRaw;
    issueRaw.quantity = 4.0;
    StockMovementRequest issueSerial = inboundSerial;
    issueSerial.quantity = 1.0;
    issueSerial.serialNumbers = {QStringLiteral("KEY-0001")};
    issue.lines = {issueRaw, issueSerial};

    PostedDocument issueDocument;
    qlonglong runId = 0;
    QVERIFY2(service.postProductionIssue(run, issue, &issueDocument, &runId, &error), qPrintable(error));
    QVERIFY(runId > 0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT COUNT(*) FROM business_document_items WHERE document_id=%1").arg(issueDocument.documentId)).toInt(), 2);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT quantity FROM stock_balances WHERE material_id=%1").arg(rawId)).toDouble(), 6.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT quantity FROM stock_balances WHERE material_id=%1").arg(serialRawId)).toDouble(), 1.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT production_batch FROM serial_numbers WHERE serial_no='KEY-0001'")).toString(),
        run.batchNo);

    QVERIFY(!service.postProductionIssue(run, issue, nullptr, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("重复")) || error.contains(QStringLiteral("已经提交")));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT quantity FROM stock_balances WHERE material_id=%1").arg(rawId)).toDouble(), 6.0);

    ProductionRunRequest rollbackRun = run;
    rollbackRun.batchNo = QStringLiteral("PROD-ROLLBACK");
    StockDocumentRequest invalidIssue = issue;
    invalidIssue.submissionToken = QStringLiteral("issue-token-rollback");
    invalidIssue.lines[0].quantity = 1.0;
    invalidIssue.lines[1].quantity = 5.0;
    invalidIssue.lines[1].serialNumbers = {
        QStringLiteral("KEY-0002"), QStringLiteral("MISSING-1"), QStringLiteral("MISSING-2"),
        QStringLiteral("MISSING-3"), QStringLiteral("MISSING-4")};
    QVERIFY(!service.postProductionIssue(rollbackRun, invalidIssue, nullptr, nullptr, &error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT COUNT(*) FROM production_runs WHERE batch_no='PROD-ROLLBACK'")).toInt(), 0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT quantity FROM stock_balances WHERE material_id=%1").arg(rawId)).toDouble(), 6.0);

    const qlonglong rawIssueItemId = scalar(manager.database(), QStringLiteral(
        "SELECT id FROM business_document_items WHERE document_id=%1 AND material_id=%2")
        .arg(issueDocument.documentId).arg(rawId)).toLongLong();
    const qlonglong serialIssueItemId = scalar(manager.database(), QStringLiteral(
        "SELECT id FROM business_document_items WHERE document_id=%1 AND material_id=%2")
        .arg(issueDocument.documentId).arg(serialRawId)).toLongLong();
    QVERIFY(rawIssueItemId > 0);
    QVERIFY(serialIssueItemId > 0);

    ProductionReturnRequest productionReturn;
    productionReturn.sourceDocumentId = issueDocument.documentId;
    productionReturn.documentDate = QDate::currentDate();
    productionReturn.handlerName = QStringLiteral("退料员");
    productionReturn.submissionToken = QStringLiteral("return-token-1");
    ProductionReturnLine returnRaw;
    returnRaw.sourceItemId = rawIssueItemId;
    returnRaw.quantity = 1.0;
    returnRaw.warehouseId = warehouseId;
    returnRaw.locationId = locationId;
    ProductionReturnLine returnSerial;
    returnSerial.sourceItemId = serialIssueItemId;
    returnSerial.quantity = 1.0;
    returnSerial.warehouseId = warehouseId;
    returnSerial.locationId = locationId;
    returnSerial.serialNumbers = {QStringLiteral("KEY-0001")};
    productionReturn.lines = {returnRaw, returnSerial};
    PostedDocument returnDocument;
    QVERIFY2(service.postProductionReturn(productionReturn, &returnDocument, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT returned_quantity FROM business_document_items WHERE id=%1").arg(rawIssueItemId)).toDouble(), 1.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT status FROM serial_numbers WHERE serial_no='KEY-0001'")).toString(), QStringLiteral("IN_STOCK"));

    ProductionReturnRequest excessiveReturn = productionReturn;
    excessiveReturn.submissionToken = QStringLiteral("return-token-excess");
    excessiveReturn.lines = {returnRaw};
    excessiveReturn.lines[0].quantity = 4.0;
    QVERIFY(!service.postProductionReturn(excessiveReturn, nullptr, &error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT returned_quantity FROM business_document_items WHERE id=%1").arg(rawIssueItemId)).toDouble(), 1.0);

    const qlonglong serialReturnItemId = scalar(manager.database(), QStringLiteral(
        "SELECT id FROM business_document_items WHERE document_id=%1 AND material_id=%2")
        .arg(returnDocument.documentId).arg(serialRawId)).toLongLong();
    ReversalRequest reverseReturn;
    reverseReturn.sourceItemId = serialReturnItemId;
    reverseReturn.documentDate = QDate::currentDate();
    reverseReturn.quantity = 1.0;
    reverseReturn.notes = QStringLiteral("测试撤销退料");
    reverseReturn.serialNumbers = {QStringLiteral("KEY-0001")};
    QVERIFY2(service.reverseItem(reverseReturn, nullptr, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT returned_quantity FROM business_document_items WHERE id=%1").arg(serialIssueItemId)).toDouble(), 0.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT status FROM serial_numbers WHERE serial_no='KEY-0001'")).toString(), QStringLiteral("OUTBOUND"));

    ReversalRequest reverseIssuedSerial;
    reverseIssuedSerial.sourceItemId = serialIssueItemId;
    reverseIssuedSerial.documentDate = QDate::currentDate();
    reverseIssuedSerial.quantity = 1.0;
    reverseIssuedSerial.notes = QStringLiteral("退料撤销后再撤销原领料");
    reverseIssuedSerial.serialNumbers = {QStringLiteral("KEY-0001")};
    QVERIFY2(service.reverseItem(reverseIssuedSerial, nullptr, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT status FROM serial_numbers WHERE serial_no='KEY-0001'")).toString(), QStringLiteral("IN_STOCK"));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT quantity FROM stock_balances WHERE material_id=%1").arg(serialRawId)).toDouble(), 2.0);

    const QStringList finishedSerials = service.previewSerialNumbers(productId, QStringLiteral("FG01"), 2, &error);
    QCOMPARE(finishedSerials.size(), 2);
    StockDocumentRequest receipt;
    receipt.documentType = QStringLiteral("CPRK");
    receipt.documentDate = QDate::currentDate();
    receipt.productionRunId = runId;
    receipt.submissionToken = QStringLiteral("receipt-token-1");
    StockMovementRequest productLine;
    productLine.materialId = productId;
    productLine.quantity = 1.0;
    productLine.warehouseId = warehouseId;
    productLine.locationId = locationId;
    productLine.serialNumbers = {finishedSerials.at(0)};
    receipt.lines = {productLine};
    QVERIFY2(service.postFinishedGoodsInbound(receipt, nullptr, &error), qPrintable(error));
    QCOMPARE(service.productionRunReceivedQuantity(runId, &error), 1.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT status FROM production_runs WHERE id=%1").arg(runId)).toString(), QStringLiteral("OPEN"));

    receipt.submissionToken = QStringLiteral("receipt-token-2");
    receipt.lines[0].serialNumbers = {finishedSerials.at(1)};
    PostedDocument secondReceipt;
    QVERIFY2(service.postFinishedGoodsInbound(receipt, &secondReceipt, &error), qPrintable(error));
    QCOMPARE(service.productionRunReceivedQuantity(runId, &error), 2.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT status FROM production_runs WHERE id=%1").arg(runId)).toString(), QStringLiteral("COMPLETED"));

    const qlonglong secondReceiptItemId = scalar(manager.database(), QStringLiteral(
        "SELECT id FROM business_document_items WHERE document_id=%1").arg(secondReceipt.documentId)).toLongLong();
    ReversalRequest reverseReceipt;
    reverseReceipt.sourceItemId = secondReceiptItemId;
    reverseReceipt.documentDate = QDate::currentDate();
    reverseReceipt.quantity = 1.0;
    reverseReceipt.notes = QStringLiteral("测试撤销成品入库");
    reverseReceipt.serialNumbers = {finishedSerials.at(1)};
    QVERIFY2(service.reverseItem(reverseReceipt, nullptr, &error), qPrintable(error));
    QCOMPARE(service.productionRunReceivedQuantity(runId, &error), 1.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT status FROM production_runs WHERE id=%1").arg(runId)).toString(), QStringLiteral("OPEN"));
}

QTEST_GUILESS_MAIN(ProductionWorkflowTests)
#include "ProductionWorkflowTests.moc"
