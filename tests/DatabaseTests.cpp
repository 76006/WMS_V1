#include "core/DatabaseConfig.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"
#include "services/InventoryService.h"

#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

class DatabaseTests final : public QObject
{
    Q_OBJECT

private slots:
    void initializeAndRunInventoryFlow();
    void serialNumberFlow();

private:
    static qlonglong scalarId(QSqlDatabase database, const QString &sql);
    static QVariant scalar(QSqlDatabase database, const QString &sql);
    static void createWarehouseMaterial(QSqlDatabase database,
                                        bool requireSerial,
                                        qlonglong *userId,
                                        qlonglong *warehouseId,
                                        qlonglong *locationId,
                                        qlonglong *materialId);
};

qlonglong DatabaseTests::scalarId(QSqlDatabase database, const QString &sql)
{
    QSqlQuery query(database);
    if (!query.exec(sql) || !query.next()) {
        return 0;
    }
    return query.value(0).toLongLong();
}

QVariant DatabaseTests::scalar(QSqlDatabase database, const QString &sql)
{
    QSqlQuery query(database);
    if (!query.exec(sql) || !query.next()) {
        return {};
    }
    return query.value(0);
}

void DatabaseTests::createWarehouseMaterial(QSqlDatabase database,
                                            bool requireSerial,
                                            qlonglong *userId,
                                            qlonglong *warehouseId,
                                            qlonglong *locationId,
                                            qlonglong *materialId)
{
    *userId = scalarId(database, QStringLiteral("SELECT id FROM users WHERE username='admin'"));
    QVERIFY(*userId > 0);

    QSqlQuery warehouse(database);
    QVERIFY(warehouse.exec(QStringLiteral("INSERT INTO warehouses(code,name) VALUES('WH01','测试仓')")));
    *warehouseId = warehouse.lastInsertId().toLongLong();
    QSqlQuery location(database);
    location.prepare(QStringLiteral("INSERT INTO locations(warehouse_id,code,name) VALUES(?,'A01','测试库位')"));
    location.addBindValue(*warehouseId);
    QVERIFY(location.exec());
    *locationId = location.lastInsertId().toLongLong();

    QSqlQuery material(database);
    material.prepare(QStringLiteral(
        "INSERT INTO materials(code,name,category_id,unit,minimum_stock,default_warehouse_id,"
        "default_location_id,require_batch,require_serial) "
        "VALUES(?,?,(SELECT id FROM material_categories WHERE code='RAW'),'个',2,?,?,1,?)"));
    material.addBindValue(requireSerial ? QStringLiteral("TEST-SN") : QStringLiteral("TEST-MAT"));
    material.addBindValue(requireSerial ? QStringLiteral("SN测试物料") : QStringLiteral("普通测试物料"));
    material.addBindValue(*warehouseId);
    material.addBindValue(*locationId);
    material.addBindValue(requireSerial);
    QVERIFY(material.exec());
    *materialId = material.lastInsertId().toLongLong();
}

void DatabaseTests::initializeAndRunInventoryFlow()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DatabaseConfig config;
    config.filePath = directory.filePath(QStringLiteral("inventory-flow.db"));
    DatabaseManager manager;
    QString error;
    QVERIFY2(manager.open(config, &error), qPrintable(error));
    QVERIFY2(SchemaMigrator::migrate(manager.database(), &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT COUNT(*) FROM roles")).toInt(), 4);
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT COUNT(*) FROM users")).toInt(), 1);

    qlonglong userId = 0;
    qlonglong warehouseId = 0;
    qlonglong locationId = 0;
    qlonglong materialId = 0;
    createWarehouseMaterial(manager.database(), false, &userId, &warehouseId, &locationId, &materialId);
    InventoryService service(manager.database(), userId);

    StockMovementRequest inbound;
    inbound.documentType = QStringLiteral("CGRK");
    inbound.documentDate = QDate::currentDate();
    inbound.handlerName = QStringLiteral("测试人员");
    inbound.materialId = materialId;
    inbound.quantity = 10.0;
    inbound.batchNo = QStringLiteral("B001");
    inbound.warehouseId = warehouseId;
    inbound.locationId = locationId;
    PostedDocument inboundDocument;
    QVERIFY2(service.postInbound(inbound, &inboundDocument, &error), qPrintable(error));
    QVERIFY(inboundDocument.documentNumber.startsWith(QStringLiteral("CGRK-")));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT quantity FROM stock_balances")).toDouble(), 10.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT COUNT(*) FROM inventory_ledger")).toInt(), 1);

    StockMovementRequest outbound = inbound;
    outbound.documentType = QStringLiteral("XSCK");
    outbound.quantity = 3.0;
    PostedDocument outboundDocument;
    QVERIFY2(service.postOutbound(outbound, &outboundDocument, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT quantity FROM stock_balances")).toDouble(), 7.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT COUNT(*) FROM inventory_ledger")).toInt(), 2);

    StockMovementRequest excessive = outbound;
    excessive.quantity = 8.0;
    QVERIFY(!service.postOutbound(excessive, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("库存不足")));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT quantity FROM stock_balances")).toDouble(), 7.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT COUNT(*) FROM inventory_ledger")).toInt(), 2);

    const qlonglong outboundItemId = scalarId(manager.database(), QStringLiteral(
        "SELECT i.id FROM business_document_items i JOIN business_documents d ON d.id=i.document_id "
        "WHERE d.document_no='%1'").arg(outboundDocument.documentNumber));
    QVERIFY(outboundItemId > 0);
    ReversalRequest partial;
    partial.sourceItemId = outboundItemId;
    partial.documentDate = QDate::currentDate();
    partial.quantity = 2.0;
    partial.handlerName = QStringLiteral("测试人员");
    partial.notes = QStringLiteral("测试部分撤销");
    QVERIFY2(service.reverseItem(partial, nullptr, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT quantity FROM stock_balances")).toDouble(), 9.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT status FROM business_documents WHERE id=%1").arg(outboundDocument.documentId)).toString(),
        QStringLiteral("PARTIALLY_REVERSED"));

    partial.quantity = 1.0;
    partial.notes = QStringLiteral("测试剩余撤销");
    QVERIFY2(service.reverseItem(partial, nullptr, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT quantity FROM stock_balances")).toDouble(), 10.0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT status FROM business_documents WHERE id=%1").arg(outboundDocument.documentId)).toString(),
        QStringLiteral("REVERSED"));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT COUNT(*) FROM inventory_ledger")).toInt(), 4);
}

void DatabaseTests::serialNumberFlow()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DatabaseConfig config;
    config.filePath = directory.filePath(QStringLiteral("serial-flow.db"));
    DatabaseManager manager;
    QString error;
    QVERIFY2(manager.open(config, &error), qPrintable(error));
    QVERIFY2(SchemaMigrator::migrate(manager.database(), &error), qPrintable(error));

    qlonglong userId = 0;
    qlonglong warehouseId = 0;
    qlonglong locationId = 0;
    qlonglong materialId = 0;
    createWarehouseMaterial(manager.database(), true, &userId, &warehouseId, &locationId, &materialId);
    InventoryService service(manager.database(), userId);
    const QStringList serials = service.previewSerialNumbers(materialId, QStringLiteral("MJ6"), 2, &error);
    QCOMPARE(serials.size(), 2);
    QVERIFY(serials.at(0).startsWith(QStringLiteral("MJ6-")));

    StockMovementRequest inbound;
    inbound.documentType = QStringLiteral("CPRK");
    inbound.documentDate = QDate::currentDate();
    inbound.materialId = materialId;
    inbound.quantity = 2.0;
    inbound.batchNo = QStringLiteral("PROD-001");
    inbound.warehouseId = warehouseId;
    inbound.locationId = locationId;
    inbound.serialNumbers = serials;
    QVERIFY2(service.postInbound(inbound, nullptr, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT COUNT(*) FROM serial_numbers WHERE status='IN_STOCK'")).toInt(), 2);

    StockMovementRequest outbound = inbound;
    outbound.documentType = QStringLiteral("XSCK");
    outbound.quantity = 1.0;
    outbound.serialNumbers = {serials.first()};
    QVERIFY2(service.postOutbound(outbound, nullptr, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT COUNT(*) FROM serial_numbers WHERE status='OUTBOUND'")).toInt(), 1);
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT quantity FROM stock_balances")).toDouble(), 1.0);

    StockMovementRequest invalid = outbound;
    invalid.quantity = 2.0;
    QVERIFY(!service.postOutbound(invalid, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("SN数量")));

    QSqlQuery secondLocation(manager.database());
    secondLocation.prepare(QStringLiteral(
        "INSERT INTO locations(warehouse_id,code,name) VALUES(?,'L02','SN目标库位')"));
    secondLocation.addBindValue(warehouseId);
    QVERIFY(secondLocation.exec());
    const qlonglong targetLocationId = secondLocation.lastInsertId().toLongLong();
    TransferRequest transfer;
    transfer.documentDate = QDate::currentDate();
    transfer.materialId = materialId;
    transfer.quantity = 1.0;
    transfer.batchNo = QStringLiteral("PROD-001");
    transfer.warehouseId = warehouseId;
    transfer.locationId = locationId;
    transfer.targetWarehouseId = warehouseId;
    transfer.targetLocationId = targetLocationId;
    transfer.serialNumbers = {serials.at(1)};
    PostedDocument transferDocument;
    QVERIFY2(service.postTransfer(transfer, &transferDocument, &error), qPrintable(error));
    const qlonglong transferItemId = scalar(manager.database(), QStringLiteral(
        "SELECT id FROM business_document_items WHERE document_id=%1")
        .arg(transferDocument.documentId)).toLongLong();
    ReversalRequest reverseTransfer;
    reverseTransfer.sourceItemId = transferItemId;
    reverseTransfer.documentDate = QDate::currentDate();
    reverseTransfer.quantity = 1.0;
    reverseTransfer.notes = QStringLiteral("SN调拨撤销测试");
    reverseTransfer.serialNumbers = {serials.at(1)};
    QVERIFY2(service.reverseTransfer(reverseTransfer, nullptr, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT location_id FROM serial_numbers WHERE serial_no='%1'").arg(serials.at(1))).toLongLong(),
        locationId);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT quantity FROM stock_balances WHERE material_id=%1 AND location_id=%2")
        .arg(materialId).arg(locationId)).toDouble(), 1.0);
}

QTEST_GUILESS_MAIN(DatabaseTests)
#include "DatabaseTests.moc"
