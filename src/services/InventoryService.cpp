#include "services/InventoryService.h"

#include <QRegularExpression>
#include <QSet>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <cmath>
#include <utility>

namespace {
constexpr double QuantityTolerance = 0.0000001;

void setError(QString *target, const QString &message)
{
    if (target) {
        *target = message;
    }
}

bool isWholeNumber(double value)
{
    return std::abs(value - std::round(value)) < QuantityTolerance;
}

QString databaseText(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isNull() ? QString::fromLatin1("", 0) : trimmed;
}

// SN一旦经撤销单（CX）从原单撤出，即使之后重新入库恢复为在库状态，
// 也不能再次参与该原单的撤销；仅排除 source_document_id 指向本原单的CX单。
// 片段内占位符固定位于SQL末尾，调用方必须在其余绑定之后绑定原单ID。
QString notReversedBySourceDocumentSql()
{
    return QStringLiteral(
        " AND NOT EXISTS(SELECT 1 FROM inventory_ledger_serials cx_ils "
        "JOIN inventory_ledger cx_l ON cx_l.id=cx_ils.ledger_id "
        "JOIN business_documents cx_d ON cx_d.id=cx_l.document_id "
        "WHERE cx_ils.serial_id=sn.id AND cx_d.document_type='CX' "
        "AND cx_d.source_document_id=?)");
}

QString serialStatusText(const QString &status)
{
    if (status == QStringLiteral("IN_STOCK")) return QStringLiteral("在库");
    if (status == QStringLiteral("OUTBOUND")) return QStringLiteral("已出库");
    if (status == QStringLiteral("CONSUMED")) return QStringLiteral("已消耗");
    if (status == QStringLiteral("SCRAPPED")) return QStringLiteral("已报废");
    if (status == QStringLiteral("VOIDED")) return QStringLiteral("已作废");
    return status;
}
}

InventoryService::InventoryService(QSqlDatabase database, qlonglong operatorId)
    : m_database(std::move(database)), m_operatorId(operatorId)
{
}

bool InventoryService::postInbound(const StockMovementRequest &request,
                                   PostedDocument *postedDocument,
                                   QString *errorMessage)
{
    return postMovement(request, true, postedDocument, errorMessage);
}

bool InventoryService::postOutbound(const StockMovementRequest &request,
                                    PostedDocument *postedDocument,
                                    QString *errorMessage)
{
    return postMovement(request, false, postedDocument, errorMessage);
}

bool InventoryService::postMovement(const StockMovementRequest &request,
                                    bool inbound,
                                    PostedDocument *postedDocument,
                                    QString *errorMessage)
{
    if (!m_database.isOpen() || m_operatorId <= 0) {
        setError(errorMessage, QStringLiteral("数据库未连接或当前用户无效。"));
        return false;
    }

    const QString type = request.documentType.trimmed().toUpper();
    if (inbound && type == QStringLiteral("CGRK")) {
        if (request.orderedQuantity < -QuantityTolerance
            || request.giftQuantity < -QuantityTolerance
            || request.giftQuantity > qMax(0.0, request.quantity - request.orderedQuantity)
                                          + QuantityTolerance) {
            setError(errorMessage, QStringLiteral(
                "采购数量或赠送数量无效，赠送数量不能超过多到货数量。"));
            return false;
        }
    } else if (std::abs(request.orderedQuantity) > QuantityTolerance
               || std::abs(request.giftQuantity) > QuantityTolerance) {
        setError(errorMessage, QStringLiteral("只有采购入库可以填写采购数量和赠送数量。"));
        return false;
    }

    const MaterialRules rules = materialRules(request.materialId, errorMessage);
    if (!rules.valid || !validateMovement(request, rules, errorMessage)
        || !validateLocation(request.warehouseId, request.locationId, errorMessage)) {
        return false;
    }

    if (!beginImmediate(errorMessage)) {
        return false;
    }

    const QString documentNumber = nextDocumentNumber(request.documentType, request.documentDate, errorMessage);
    if (documentNumber.isEmpty()) {
        rollback();
        return false;
    }

    const qlonglong documentId = createDocument(documentNumber,
                                                 request.documentType,
                                                 inbound ? QStringLiteral("IN") : QStringLiteral("OUT"),
                                                 request.documentDate,
                                                 request.handlerName,
                                                 request.purpose,
                                                 request.notes,
                                                 QVariant(),
                                                 errorMessage);
    if (documentId <= 0) {
        rollback();
        return false;
    }

    QSqlQuery supplierUpdate(m_database);
    if (inbound && type == QStringLiteral("CGRK")) {
        supplierUpdate.prepare(QStringLiteral("UPDATE business_documents SET supplier=? WHERE id=?"));
        supplierUpdate.addBindValue(databaseText(request.supplier));
        supplierUpdate.addBindValue(documentId);
        if (!supplierUpdate.exec()) {
            setError(errorMessage, supplierUpdate.lastError().text());
            rollback();
            return false;
        }
    }
    const qlonglong itemId = createItem(documentId, request, 0, 0, errorMessage);
    if (itemId <= 0) {
        rollback();
        return false;
    }

    double before = 0.0;
    double after = 0.0;
    const double delta = inbound ? request.quantity : -request.quantity;
    if (!changeBalance(request.materialId,
                       request.warehouseId,
                       request.locationId,
                       request.batchNo.trimmed(),
                       delta,
                       &before,
                       &after,
                       errorMessage)) {
        rollback();
        return false;
    }

    const qlonglong ledgerId = createLedger(documentId,
                                            itemId,
                                            request.documentType,
                                            request.materialId,
                                            request.batchNo.trimmed(),
                                            inbound ? request.quantity : 0.0,
                                            inbound ? 0.0 : request.quantity,
                                            before,
                                            after,
                                            request.warehouseId,
                                            request.locationId,
                                            request.notes,
                                            errorMessage);
    if (ledgerId <= 0) {
        rollback();
        return false;
    }

    if (!request.batchNo.trimmed().isEmpty()) {
        QSqlQuery batch(m_database);
        batch.prepare(QStringLiteral(
            "INSERT INTO batches(material_id,batch_no,supplier,first_in_at) VALUES(?,?,?,?) "
            "ON CONFLICT(material_id,batch_no) DO UPDATE SET supplier="
            "CASE WHEN excluded.supplier<>'' THEN excluded.supplier ELSE batches.supplier END"));
        batch.addBindValue(request.materialId);
        batch.addBindValue(databaseText(request.batchNo));
        batch.addBindValue(databaseText(request.supplier));
        batch.addBindValue(inbound ? QDateTime::currentDateTime().toString(Qt::ISODateWithMs) : QVariant());
        if (!batch.exec()) {
            setError(errorMessage, QStringLiteral("批次记录失败：%1").arg(batch.lastError().text()));
            rollback();
            return false;
        }
    }

    const bool serialsOk = inbound
        ? attachSerialsToInbound(request, documentId, ledgerId, errorMessage)
        : attachSerialsToOutbound(request, documentId, ledgerId, errorMessage);
    if (!serialsOk || !finalizeDocument(documentId, errorMessage)
        || !writeAudit(QStringLiteral("POST"), QStringLiteral("business_document"), documentId,
                       documentNumber, errorMessage)
        || !commit(errorMessage)) {
        rollback();
        return false;
    }

    if (postedDocument) {
        postedDocument->documentId = documentId;
        postedDocument->documentNumber = documentNumber;
    }
    return true;
}

bool InventoryService::postTransfer(const TransferRequest &request,
                                    PostedDocument *postedDocument,
                                    QString *errorMessage)
{
    StockMovementRequest transferMovement = request;
    transferMovement.documentType = QStringLiteral("DB");
    const MaterialRules rules = materialRules(request.materialId, errorMessage);
    if (!rules.valid || !validateMovement(transferMovement, rules, errorMessage)
        || !validateLocation(request.warehouseId, request.locationId, errorMessage)
        || !validateLocation(request.targetWarehouseId, request.targetLocationId, errorMessage)) {
        return false;
    }
    if (request.warehouseId == request.targetWarehouseId
        && request.locationId == request.targetLocationId) {
        setError(errorMessage, QStringLiteral("原库位与目标库位不能相同。"));
        return false;
    }

    if (!beginImmediate(errorMessage)) {
        return false;
    }
    const QString number = nextDocumentNumber(QStringLiteral("DB"), request.documentDate, errorMessage);
    if (number.isEmpty()) {
        rollback();
        return false;
    }
    const qlonglong documentId = createDocument(number, QStringLiteral("DB"), QStringLiteral("TRANSFER"),
                                                 request.documentDate, request.handlerName, request.purpose,
                                                 request.notes, QVariant(), errorMessage);
    const qlonglong itemId = documentId > 0
        ? createItem(documentId, request, request.targetWarehouseId, request.targetLocationId, errorMessage)
        : 0;
    if (itemId <= 0) {
        rollback();
        return false;
    }

    double sourceBefore = 0.0;
    double sourceAfter = 0.0;
    double targetBefore = 0.0;
    double targetAfter = 0.0;
    if (!changeBalance(request.materialId, request.warehouseId, request.locationId,
                       request.batchNo.trimmed(), -request.quantity, &sourceBefore, &sourceAfter, errorMessage)
        || !changeBalance(request.materialId, request.targetWarehouseId, request.targetLocationId,
                          request.batchNo.trimmed(), request.quantity, &targetBefore, &targetAfter, errorMessage)) {
        rollback();
        return false;
    }

    const qlonglong outLedger = createLedger(documentId, itemId, QStringLiteral("DB-OUT"), request.materialId,
                                              request.batchNo.trimmed(), 0.0, request.quantity,
                                              sourceBefore, sourceAfter, request.warehouseId,
                                              request.locationId, request.notes, errorMessage);
    const qlonglong inLedger = outLedger > 0
        ? createLedger(documentId, itemId, QStringLiteral("DB-IN"), request.materialId,
                       request.batchNo.trimmed(), request.quantity, 0.0,
                       targetBefore, targetAfter, request.targetWarehouseId,
                       request.targetLocationId, request.notes, errorMessage)
        : 0;
    if (inLedger <= 0) {
        rollback();
        return false;
    }

    for (const QString &serial : request.serialNumbers) {
        QSqlQuery serialQuery(m_database);
        serialQuery.prepare(QStringLiteral(
            "SELECT id FROM serial_numbers WHERE serial_no = ? AND material_id = ? AND status = 'IN_STOCK' "
            "AND warehouse_id = ? AND location_id = ? AND batch_no = ?"));
        serialQuery.addBindValue(serial.trimmed());
        serialQuery.addBindValue(request.materialId);
        serialQuery.addBindValue(request.warehouseId);
        serialQuery.addBindValue(request.locationId);
        serialQuery.addBindValue(databaseText(request.batchNo));
        if (!serialQuery.exec() || !serialQuery.next()) {
            setError(errorMessage, QStringLiteral("SN %1 不在所选源库位中。").arg(serial));
            rollback();
            return false;
        }
        const qlonglong serialId = serialQuery.value(0).toLongLong();
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral(
            "UPDATE serial_numbers SET warehouse_id = ?, location_id = ?, last_document_id = ? WHERE id = ?"));
        update.addBindValue(request.targetWarehouseId);
        update.addBindValue(request.targetLocationId);
        update.addBindValue(documentId);
        update.addBindValue(serialId);
        if (!update.exec() || !linkLedgerSerial(outLedger, serialId, errorMessage)
            || !linkLedgerSerial(inLedger, serialId, errorMessage)) {
            if (errorMessage && errorMessage->isEmpty()) {
                *errorMessage = update.lastError().text();
            }
            rollback();
            return false;
        }
    }

    if (!finalizeDocument(documentId, errorMessage)
        || !writeAudit(QStringLiteral("POST"), QStringLiteral("business_document"), documentId, number, errorMessage)
        || !commit(errorMessage)) {
        rollback();
        return false;
    }
    if (postedDocument) {
        postedDocument->documentId = documentId;
        postedDocument->documentNumber = number;
    }
    return true;
}

bool InventoryService::reverseTransfer(const ReversalRequest &request,
                                       PostedDocument *postedDocument,
                                       QString *errorMessage)
{
    if (request.sourceItemId <= 0 || request.quantity <= QuantityTolerance
        || !request.documentDate.isValid()) {
        setError(errorMessage, QStringLiteral("撤销调拨明细、日期或数量无效。"));
        return false;
    }
    if (!beginImmediate(errorMessage)) {
        return false;
    }

    QSqlQuery source(m_database);
    source.prepare(QStringLiteral(
        "SELECT i.document_id,d.document_no,d.status,d.document_type,d.stock_direction,"
        "i.material_id,i.quantity,i.reversed_quantity,i.batch_no,i.warehouse_id,i.location_id,"
        "i.target_warehouse_id,i.target_location_id,m.require_serial "
        "FROM business_document_items i "
        "JOIN business_documents d ON d.id=i.document_id "
        "JOIN materials m ON m.id=i.material_id WHERE i.id=?"));
    source.addBindValue(request.sourceItemId);
    if (!source.exec() || !source.next()) {
        setError(errorMessage, QStringLiteral("找不到需要撤销的调拨明细。"));
        rollback();
        return false;
    }

    const qlonglong sourceDocumentId = source.value(0).toLongLong();
    const QString sourceNumber = source.value(1).toString();
    const QString sourceStatus = source.value(2).toString();
    const QString sourceType = source.value(3).toString();
    const QString sourceDirection = source.value(4).toString();
    const qlonglong materialId = source.value(5).toLongLong();
    const double originalQuantity = source.value(6).toDouble();
    const double reversedQuantity = source.value(7).toDouble();
    const QString batchNo = source.value(8).toString();
    const qlonglong originalWarehouseId = source.value(9).toLongLong();
    const qlonglong originalLocationId = source.value(10).toLongLong();
    const qlonglong targetWarehouseId = source.value(11).toLongLong();
    const qlonglong targetLocationId = source.value(12).toLongLong();
    const bool requireSerial = source.value(13).toBool();

    if (sourceType != QStringLiteral("DB") || sourceDirection != QStringLiteral("TRANSFER")
        || (sourceStatus != QStringLiteral("POSTED")
            && sourceStatus != QStringLiteral("PARTIALLY_REVERSED"))) {
        setError(errorMessage, QStringLiteral("所选记录不是可撤销的已生效调拨单。"));
        rollback();
        return false;
    }
    if (request.quantity > originalQuantity - reversedQuantity + QuantityTolerance) {
        setError(errorMessage, QStringLiteral("撤销数量超过该调拨明细的剩余可撤销数量。"));
        rollback();
        return false;
    }
    if (!validateLocation(targetWarehouseId, targetLocationId, errorMessage)
        || !validateLocation(originalWarehouseId, originalLocationId, errorMessage)) {
        rollback();
        return false;
    }

    QStringList serialNumbers;
    QSet<QString> uniqueSerials;
    for (const QString &value : request.serialNumbers) {
        const QString serial = value.trimmed().toUpper();
        if (serial.isEmpty() || uniqueSerials.contains(serial)) {
            setError(errorMessage, QStringLiteral("SN列表包含空值或重复值。"));
            rollback();
            return false;
        }
        uniqueSerials.insert(serial);
        serialNumbers.append(serial);
    }
    if (requireSerial
        && (!isWholeNumber(request.quantity)
            || serialNumbers.size() != static_cast<int>(std::round(request.quantity)))) {
        setError(errorMessage, QStringLiteral("SN管理物料必须选择与撤销数量一致的SN。"));
        rollback();
        return false;
    }
    if (!requireSerial && !serialNumbers.isEmpty()) {
        setError(errorMessage, QStringLiteral("普通物料撤销调拨时不应填写SN。"));
        rollback();
        return false;
    }

    const QString number = nextDocumentNumber(QStringLiteral("CX"), request.documentDate, errorMessage);
    if (number.isEmpty()) {
        rollback();
        return false;
    }
    const QString notes = QStringLiteral("撤销调拨 %1；%2").arg(sourceNumber, request.notes);
    const qlonglong documentId = createDocument(number, QStringLiteral("CX"),
                                                 QStringLiteral("TRANSFER"), request.documentDate,
                                                 request.handlerName, QStringLiteral("撤销调拨"),
                                                 notes, sourceDocumentId, errorMessage);
    if (documentId <= 0) {
        rollback();
        return false;
    }
    StockMovementRequest movement;
    movement.documentType = QStringLiteral("CX");
    movement.documentDate = request.documentDate;
    movement.materialId = materialId;
    movement.quantity = request.quantity;
    movement.batchNo = batchNo;
    movement.warehouseId = targetWarehouseId;
    movement.locationId = targetLocationId;
    movement.notes = notes;
    movement.serialNumbers = serialNumbers;
    const qlonglong itemId = createItem(documentId, movement, originalWarehouseId,
                                        originalLocationId, errorMessage);
    if (itemId <= 0) {
        rollback();
        return false;
    }
    QSqlQuery linkSource(m_database);
    linkSource.prepare(QStringLiteral(
        "UPDATE business_document_items SET source_item_id=? WHERE id=?"));
    linkSource.addBindValue(request.sourceItemId);
    linkSource.addBindValue(itemId);
    if (!linkSource.exec()) {
        setError(errorMessage, linkSource.lastError().text());
        rollback();
        return false;
    }

    double targetBefore = 0.0;
    double targetAfter = 0.0;
    double originalBefore = 0.0;
    double originalAfter = 0.0;
    if (!changeBalance(materialId, targetWarehouseId, targetLocationId, batchNo,
                       -request.quantity, &targetBefore, &targetAfter, errorMessage)
        || !changeBalance(materialId, originalWarehouseId, originalLocationId, batchNo,
                          request.quantity, &originalBefore, &originalAfter, errorMessage)) {
        rollback();
        return false;
    }
    const qlonglong outLedger = createLedger(documentId, itemId, QStringLiteral("CX-DB-OUT"),
                                              materialId, batchNo, 0.0, request.quantity,
                                              targetBefore, targetAfter, targetWarehouseId,
                                              targetLocationId, notes, errorMessage);
    const qlonglong inLedger = outLedger > 0
        ? createLedger(documentId, itemId, QStringLiteral("CX-DB-IN"), materialId, batchNo,
                       request.quantity, 0.0, originalBefore, originalAfter,
                       originalWarehouseId, originalLocationId, notes, errorMessage)
        : 0;
    if (inLedger <= 0) {
        rollback();
        return false;
    }

    QSqlQuery originalLedgers(m_database);
    originalLedgers.prepare(QStringLiteral(
        "SELECT id,business_type FROM inventory_ledger WHERE document_item_id=?"));
    originalLedgers.addBindValue(request.sourceItemId);
    if (!originalLedgers.exec()) {
        setError(errorMessage, originalLedgers.lastError().text());
        rollback();
        return false;
    }
    qlonglong originalOutLedger = 0;
    qlonglong originalInLedger = 0;
    while (originalLedgers.next()) {
        if (originalLedgers.value(1).toString() == QStringLiteral("DB-OUT"))
            originalOutLedger = originalLedgers.value(0).toLongLong();
        else if (originalLedgers.value(1).toString() == QStringLiteral("DB-IN"))
            originalInLedger = originalLedgers.value(0).toLongLong();
    }
    QSqlQuery linkLedger(m_database);
    linkLedger.prepare(QStringLiteral(
        "UPDATE inventory_ledger SET reversal_of_ledger_id=? WHERE id=?"));
    linkLedger.addBindValue(originalInLedger > 0 ? QVariant(originalInLedger) : QVariant());
    linkLedger.addBindValue(outLedger);
    if (!linkLedger.exec()) {
        setError(errorMessage, linkLedger.lastError().text());
        rollback();
        return false;
    }
    linkLedger.bindValue(0, originalOutLedger > 0 ? QVariant(originalOutLedger) : QVariant());
    linkLedger.bindValue(1, inLedger);
    if (!linkLedger.exec()) {
        setError(errorMessage, linkLedger.lastError().text());
        rollback();
        return false;
    }

    for (const QString &serial : serialNumbers) {
        QSqlQuery find(m_database);
        find.prepare(QStringLiteral(
            "SELECT sn.id FROM serial_numbers sn WHERE sn.serial_no=? AND sn.material_id=? "
            "AND sn.status='IN_STOCK' AND sn.warehouse_id=? AND sn.location_id=? AND sn.batch_no=? "
            "AND EXISTS(SELECT 1 FROM inventory_ledger_serials x "
            "JOIN inventory_ledger l ON l.id=x.ledger_id "
            "WHERE x.serial_id=sn.id AND l.document_item_id=? AND l.business_type='DB-IN')"));
        find.addBindValue(serial);
        find.addBindValue(materialId);
        find.addBindValue(targetWarehouseId);
        find.addBindValue(targetLocationId);
        find.addBindValue(databaseText(batchNo));
        find.addBindValue(request.sourceItemId);
        if (!find.exec() || !find.next()) {
            setError(errorMessage, QStringLiteral("SN %1 不在该调拨单的目标库位中。").arg(serial));
            rollback();
            return false;
        }
        const qlonglong serialId = find.value(0).toLongLong();
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral(
            "UPDATE serial_numbers SET warehouse_id=?,location_id=?,last_document_id=? WHERE id=?"));
        update.addBindValue(originalWarehouseId);
        update.addBindValue(originalLocationId);
        update.addBindValue(documentId);
        update.addBindValue(serialId);
        if (!update.exec() || !linkLedgerSerial(outLedger, serialId, errorMessage)
            || !linkLedgerSerial(inLedger, serialId, errorMessage)) {
            if (errorMessage && errorMessage->isEmpty()) *errorMessage = update.lastError().text();
            rollback();
            return false;
        }
    }

    QSqlQuery updateItem(m_database);
    updateItem.prepare(QStringLiteral(
        "UPDATE business_document_items SET reversed_quantity=reversed_quantity+? "
        "WHERE id=? AND reversed_quantity+?<=quantity+0.0000001"));
    updateItem.addBindValue(request.quantity);
    updateItem.addBindValue(request.sourceItemId);
    updateItem.addBindValue(request.quantity);
    if (!updateItem.exec() || updateItem.numRowsAffected() != 1) {
        setError(errorMessage, QStringLiteral("调拨可撤销数量已发生变化，请刷新后重试。"));
        rollback();
        return false;
    }
    QSqlQuery remaining(m_database);
    remaining.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM business_document_items WHERE document_id=? "
        "AND reversed_quantity<quantity-0.0000001"));
    remaining.addBindValue(sourceDocumentId);
    if (!remaining.exec() || !remaining.next()) {
        setError(errorMessage, remaining.lastError().text());
        rollback();
        return false;
    }
    QSqlQuery updateDocument(m_database);
    updateDocument.prepare(QStringLiteral(
        "UPDATE business_documents SET status=?,updated_at=? WHERE id=?"));
    updateDocument.addBindValue(remaining.value(0).toInt() == 0
                                    ? QStringLiteral("REVERSED")
                                    : QStringLiteral("PARTIALLY_REVERSED"));
    updateDocument.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    updateDocument.addBindValue(sourceDocumentId);
    if (!updateDocument.exec() || !finalizeDocument(documentId, errorMessage)
        || !writeAudit(QStringLiteral("REVERSE_TRANSFER"), QStringLiteral("business_document"),
                       sourceDocumentId, number, errorMessage)
        || !commit(errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) *errorMessage = updateDocument.lastError().text();
        rollback();
        return false;
    }
    if (postedDocument) {
        postedDocument->documentId = documentId;
        postedDocument->documentNumber = number;
    }
    return true;
}

QStringList InventoryService::reversibleSerialNumbers(qlonglong sourceItemId,
                                                      QString *errorMessage) const
{
    if (errorMessage) errorMessage->clear();
    if (sourceItemId <= 0) {
        setError(errorMessage, QStringLiteral("找不到需要撤销的原业务明细。"));
        return {};
    }
    QSqlQuery source(m_database);
    source.prepare(QStringLiteral(
        "SELECT i.document_id, d.document_no, d.stock_direction, d.document_type, "
        "i.material_id, i.batch_no, i.warehouse_id, i.location_id, m.require_serial "
        "FROM business_document_items i "
        "JOIN business_documents d ON d.id=i.document_id "
        "JOIN materials m ON m.id=i.material_id WHERE i.id=?"));
    source.addBindValue(sourceItemId);
    if (!source.exec()) {
        setError(errorMessage, source.lastError().text());
        return {};
    }
    if (!source.next()) {
        setError(errorMessage, QStringLiteral("找不到需要撤销的原业务明细。"));
        return {};
    }
    const qlonglong sourceDocumentId = source.value(0).toLongLong();
    const QString sourceNumber = source.value(1).toString();
    const QString direction = source.value(2).toString();
    const QString sourceType = source.value(3).toString();
    const qlonglong materialId = source.value(4).toLongLong();
    const QString batchNo = source.value(5).toString();
    const qlonglong warehouseId = source.value(6).toLongLong();
    const qlonglong locationId = source.value(7).toLongLong();
    const bool requireSerial = source.value(8).toBool();

    // 以下判断与 reverseItem 保持一致，避免界面提供必然失败的可撤销SN。
    if (sourceType == QStringLiteral("CX")) {
        setError(errorMessage, QStringLiteral(
            "撤销单（%1）是反向单据，不能再次撤销；如需恢复请在原业务中重新办理。")
            .arg(sourceNumber));
        return {};
    }
    if (direction != QStringLiteral("IN") && direction != QStringLiteral("OUT")) {
        setError(errorMessage, QStringLiteral("当前版本仅支持入库和出库明细的部分撤销。"));
        return {};
    }
    if (!requireSerial) {
        setError(errorMessage, QStringLiteral("该明细不是SN管理物料，无需选择SN。"));
        return {};
    }

    QString sql;
    if (direction == QStringLiteral("OUT")) {
        if (sourceType == QStringLiteral("SCLL")) {
            // 生产领料：SN可能已被后续业务流转，必须沿原领料明细的流水关联回溯。
            sql = QStringLiteral(
                "SELECT sn.serial_no FROM serial_numbers sn "
                "JOIN inventory_ledger_serials ils ON ils.serial_id=sn.id "
                "JOIN inventory_ledger l ON l.id=ils.ledger_id "
                "WHERE sn.material_id=? AND sn.status='OUTBOUND' "
                "AND l.document_item_id=? AND l.business_type='SCLL'")
                + notReversedBySourceDocumentSql();
        } else {
            // 普通出库撤销：同一物料可能出现在多行，必须同时沿原明细的流水关联匹配。
            sql = QStringLiteral(
                "SELECT sn.serial_no FROM serial_numbers sn "
                "JOIN inventory_ledger_serials ils ON ils.serial_id=sn.id "
                "JOIN inventory_ledger l ON l.id=ils.ledger_id "
                "WHERE sn.material_id=? AND sn.status='OUTBOUND' "
                "AND sn.last_document_id=? AND l.document_item_id=?")
                + notReversedBySourceDocumentSql();
        }
    } else {
        // 入库撤销：SN必须在原仓库/库位/批次在库，且经流水关联到该原明细。
        sql = QStringLiteral(
            "SELECT sn.serial_no FROM serial_numbers sn "
            "JOIN inventory_ledger_serials ils ON ils.serial_id=sn.id "
            "JOIN inventory_ledger l ON l.id=ils.ledger_id "
            "WHERE sn.material_id=? AND sn.status='IN_STOCK' "
            "AND sn.warehouse_id=? AND sn.location_id=? AND sn.batch_no=? "
            "AND l.document_item_id=?")
            + notReversedBySourceDocumentSql();
    }
    QSqlQuery query(m_database);
    query.prepare(sql);
    query.addBindValue(materialId);
    if (direction == QStringLiteral("OUT")) {
        if (sourceType == QStringLiteral("SCLL")) {
            query.addBindValue(sourceItemId);
        } else {
            query.addBindValue(sourceDocumentId);
            query.addBindValue(sourceItemId);
        }
    } else {
        query.addBindValue(warehouseId);
        query.addBindValue(locationId);
        query.addBindValue(databaseText(batchNo));
        query.addBindValue(sourceItemId);
    }
    // notReversedBySourceDocumentSql 的占位符位于SQL末尾，必须在最后绑定。
    query.addBindValue(sourceDocumentId);
    if (!query.exec()) {
        setError(errorMessage, query.lastError().text());
        return {};
    }
    QSet<QString> seen;
    QStringList serials;
    while (query.next()) {
        const QString serial = query.value(0).toString().trimmed();
        if (serial.isEmpty()) continue;
        const QString key = serial.toUpper();
        if (seen.contains(key)) continue;
        seen.insert(key);
        serials.append(serial);
    }
    serials.sort(Qt::CaseInsensitive);
    return serials;
}

bool InventoryService::reverseItem(const ReversalRequest &request,
                                   PostedDocument *postedDocument,
                                   QString *errorMessage)
{
    if (request.sourceItemId <= 0 || request.quantity <= QuantityTolerance
        || !request.documentDate.isValid()) {
        setError(errorMessage, QStringLiteral("撤销明细、日期或数量无效。"));
        return false;
    }
    if (!beginImmediate(errorMessage)) {
        return false;
    }

    QSqlQuery source(m_database);
    source.prepare(QStringLiteral(
        "SELECT i.document_id, d.document_no, d.stock_direction, d.status, d.document_type, "
        "d.production_run_id, i.material_id, i.quantity, i.reversed_quantity, i.returned_quantity, "
        "i.batch_no, i.warehouse_id, i.location_id, m.require_serial, i.source_item_id, "
        "i.gift_quantity, i.reversed_gift_quantity "
        "FROM business_document_items i "
        "JOIN business_documents d ON d.id=i.document_id "
        "JOIN materials m ON m.id=i.material_id WHERE i.id=?"));
    source.addBindValue(request.sourceItemId);
    if (!source.exec() || !source.next()) {
        setError(errorMessage, QStringLiteral("找不到需要撤销的原业务明细。"));
        rollback();
        return false;
    }

    const qlonglong sourceDocumentId = source.value(0).toLongLong();
    const QString sourceNumber = source.value(1).toString();
    const QString direction = source.value(2).toString();
    const QString sourceStatus = source.value(3).toString();
    const QString sourceType = source.value(4).toString();
    const qlonglong productionRunId = source.value(5).toLongLong();
    const qlonglong materialId = source.value(6).toLongLong();
    const double originalQuantity = source.value(7).toDouble();
    const double reversedQuantity = source.value(8).toDouble();
    const double returnedQuantity = source.value(9).toDouble();
    const QString batchNo = source.value(10).toString();
    const qlonglong warehouseId = source.value(11).toLongLong();
    const qlonglong locationId = source.value(12).toLongLong();
    const bool requireSerial = source.value(13).toBool();
    const qlonglong originalSourceItemId = source.value(14).toLongLong();
    const double originalGiftQuantity = source.value(15).toDouble();
    const double reversedGiftQuantity = source.value(16).toDouble();

    // 撤销单不能再撤销：必须在任何库存或单据变更之前拦截，避免出现循环反向业务。
    if (sourceType == QStringLiteral("CX")) {
        setError(errorMessage, QStringLiteral(
            "撤销单（%1）是反向单据，不能再次撤销；如需恢复请在原业务中重新办理。")
            .arg(sourceNumber));
        rollback();
        return false;
    }
    if (direction != QStringLiteral("IN") && direction != QStringLiteral("OUT")) {
        setError(errorMessage, QStringLiteral("当前版本仅支持入库和出库明细的部分撤销。"));
        rollback();
        return false;
    }
    const double unavailableForReversal =
        reversedQuantity + (sourceType == QStringLiteral("SCLL") ? returnedQuantity : 0.0);
    if (sourceStatus == QStringLiteral("REVERSED")
        || request.quantity > originalQuantity - unavailableForReversal + QuantityTolerance) {
        setError(errorMessage, QStringLiteral("撤销数量超过原明细的剩余可撤销数量。"));
        rollback();
        return false;
    }
    const bool purchaseInbound = sourceType == QStringLiteral("CGRK")
        && direction == QStringLiteral("IN");
    const double remainingGiftQuantity = originalGiftQuantity - reversedGiftQuantity;
    if (request.giftQuantity < -QuantityTolerance
        || request.giftQuantity > request.quantity + QuantityTolerance
        || request.giftQuantity > remainingGiftQuantity + QuantityTolerance
        || (!purchaseInbound && request.giftQuantity > QuantityTolerance)) {
        setError(errorMessage, QStringLiteral("撤销赠送数量无效或超过剩余赠送数量。"));
        rollback();
        return false;
    }
    if (requireSerial
        && (!isWholeNumber(request.quantity)
            || request.serialNumbers.size() != static_cast<int>(std::round(request.quantity)))) {
        setError(errorMessage, QStringLiteral("SN管理物料必须选择与撤销数量一致的SN。"));
        rollback();
        return false;
    }

    const QString number = nextDocumentNumber(QStringLiteral("CX"), request.documentDate, errorMessage);
    if (number.isEmpty()) {
        rollback();
        return false;
    }

    const bool reversalInbound = direction == QStringLiteral("OUT");
    const qlonglong documentId = createDocument(number, QStringLiteral("CX"),
                                                 reversalInbound ? QStringLiteral("IN")
                                                                 : QStringLiteral("OUT"),
                                                 request.documentDate, request.handlerName, QString(),
                                                 request.notes, sourceDocumentId, errorMessage);
    if (documentId <= 0) {
        rollback();
        return false;
    }
    if (productionRunId > 0) {
        QSqlQuery context(m_database);
        context.prepare(QStringLiteral(
            "UPDATE business_documents SET production_run_id=? WHERE id=?"));
        context.addBindValue(productionRunId);
        context.addBindValue(documentId);
        if (!context.exec()) {
            setError(errorMessage, context.lastError().text());
            rollback();
            return false;
        }
    }

    StockMovementRequest itemRequest;
    itemRequest.documentType = QStringLiteral("CX");
    itemRequest.documentDate = request.documentDate;
    itemRequest.materialId = materialId;
    itemRequest.quantity = request.quantity;
    itemRequest.giftQuantity = request.giftQuantity;
    itemRequest.batchNo = batchNo;
    itemRequest.warehouseId = warehouseId;
    itemRequest.locationId = locationId;
    itemRequest.notes = request.notes;
    itemRequest.serialNumbers = request.serialNumbers;
    const qlonglong itemId = createItem(documentId, itemRequest, 0, 0, errorMessage);
    if (itemId <= 0) {
        rollback();
        return false;
    }

    double before = 0.0;
    double after = 0.0;
    const double delta = reversalInbound ? request.quantity : -request.quantity;
    if (!changeBalance(materialId, warehouseId, locationId, batchNo, delta,
                       &before, &after, errorMessage)) {
        rollback();
        return false;
    }
    const qlonglong ledgerId = createLedger(
        documentId, itemId, QStringLiteral("CX"), materialId, batchNo,
        reversalInbound ? request.quantity : 0.0,
        reversalInbound ? 0.0 : request.quantity,
        before, after, warehouseId, locationId,
        QStringLiteral("撤销原单 %1；%2").arg(sourceNumber, request.notes),
        errorMessage);
    if (ledgerId <= 0) {
        rollback();
        return false;
    }

    for (const QString &serial : request.serialNumbers) {
        QSqlQuery find(m_database);
        if (reversalInbound && sourceType == QStringLiteral("SCLL")) {
            find.prepare(QStringLiteral(
                "SELECT sn.id FROM serial_numbers sn "
                "JOIN inventory_ledger_serials ils ON ils.serial_id=sn.id "
                "JOIN inventory_ledger l ON l.id=ils.ledger_id "
                "WHERE sn.serial_no=? AND sn.material_id=? AND sn.status='OUTBOUND' "
                "AND l.document_item_id=? AND l.business_type='SCLL'")
                + notReversedBySourceDocumentSql());
        } else if (reversalInbound) {
            // 普通出库撤销：除最后单据外，还必须匹配原明细的流水关联，
            // 否则同一物料多行时可能撤销到无关SN。与 reversibleSerialNumbers 保持一致。
            find.prepare(QStringLiteral(
                "SELECT sn.id FROM serial_numbers sn "
                "JOIN inventory_ledger_serials ils ON ils.serial_id=sn.id "
                "JOIN inventory_ledger l ON l.id=ils.ledger_id "
                "WHERE sn.serial_no=? AND sn.material_id=? AND sn.status='OUTBOUND' "
                "AND sn.last_document_id=? AND l.document_item_id=?")
                + notReversedBySourceDocumentSql());
        } else {
            // 入库撤销必须同时校验SN经流水关联到该原明细，避免撤销同库位同批次的无关SN。
            find.prepare(QStringLiteral(
                "SELECT sn.id FROM serial_numbers sn "
                "JOIN inventory_ledger_serials ils ON ils.serial_id=sn.id "
                "JOIN inventory_ledger l ON l.id=ils.ledger_id "
                "WHERE sn.serial_no=? AND sn.material_id=? "
                "AND sn.status='IN_STOCK' AND sn.warehouse_id=? AND sn.location_id=? "
                "AND sn.batch_no=? AND l.document_item_id=?")
                + notReversedBySourceDocumentSql());
        }
        find.addBindValue(serial.trimmed().toUpper());
        find.addBindValue(materialId);
        if (reversalInbound && sourceType == QStringLiteral("SCLL")) {
            find.addBindValue(request.sourceItemId);
        } else if (reversalInbound) {
            find.addBindValue(sourceDocumentId);
            find.addBindValue(request.sourceItemId);
        } else {
            find.addBindValue(warehouseId);
            find.addBindValue(locationId);
            find.addBindValue(databaseText(batchNo));
            find.addBindValue(request.sourceItemId);
        }
        // notReversedBySourceDocumentSql 的占位符位于SQL末尾，必须在最后绑定。
        find.addBindValue(sourceDocumentId);
        if (!find.exec() || !find.next()) {
            setError(errorMessage, QStringLiteral("SN %1 当前状态不允许撤销。").arg(serial));
            rollback();
            return false;
        }

        const qlonglong serialId = find.value(0).toLongLong();
        QSqlQuery update(m_database);
        if (reversalInbound) {
            update.prepare(QStringLiteral(
                "UPDATE serial_numbers SET status='IN_STOCK', warehouse_id=?, location_id=?, "
                "outbound_at=NULL, last_document_id=? WHERE id=?"));
            update.addBindValue(warehouseId);
            update.addBindValue(locationId);
            update.addBindValue(documentId);
            update.addBindValue(serialId);
        } else if (sourceType == QStringLiteral("SCTL")) {
            update.prepare(QStringLiteral(
                "UPDATE serial_numbers SET status='OUTBOUND', warehouse_id=NULL, location_id=NULL, "
                "outbound_at=?, last_document_id=? WHERE id=?"));
            update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
            update.addBindValue(documentId);
            update.addBindValue(serialId);
        } else {
            update.prepare(QStringLiteral(
                "UPDATE serial_numbers SET status='VOIDED', warehouse_id=NULL, location_id=NULL, "
                "last_document_id=? WHERE id=?"));
            update.addBindValue(documentId);
            update.addBindValue(serialId);
        }
        if (!update.exec() || !linkLedgerSerial(ledgerId, serialId, errorMessage)) {
            if (errorMessage && errorMessage->isEmpty()) {
                *errorMessage = update.lastError().text();
            }
            rollback();
            return false;
        }
    }

    QSqlQuery updateItem(m_database);
    updateItem.prepare(QStringLiteral(
        "UPDATE business_document_items SET reversed_quantity=reversed_quantity+?, "
        "reversed_gift_quantity=reversed_gift_quantity+? "
        "WHERE id=? AND reversed_quantity+?<=quantity-CASE WHEN ?='SCLL' "
        "THEN returned_quantity ELSE 0 END+0.0000001 "
        "AND reversed_gift_quantity+?<=gift_quantity+0.0000001"));
    updateItem.addBindValue(request.quantity);
    updateItem.addBindValue(request.giftQuantity);
    updateItem.addBindValue(request.sourceItemId);
    updateItem.addBindValue(request.quantity);
    updateItem.addBindValue(sourceType);
    updateItem.addBindValue(request.giftQuantity);
    if (!updateItem.exec() || updateItem.numRowsAffected() != 1) {
        setError(errorMessage, QStringLiteral("可撤销数量已发生变化，请刷新后重试。"));
        rollback();
        return false;
    }

    if (sourceType == QStringLiteral("SCTL") && originalSourceItemId > 0) {
        QSqlQuery updateReturned(m_database);
        updateReturned.prepare(QStringLiteral(
            "UPDATE business_document_items SET returned_quantity=returned_quantity-? "
            "WHERE id=? AND returned_quantity>=?"));
        updateReturned.addBindValue(request.quantity);
        updateReturned.addBindValue(originalSourceItemId);
        updateReturned.addBindValue(request.quantity);
        if (!updateReturned.exec() || updateReturned.numRowsAffected() != 1) {
            setError(errorMessage, QStringLiteral("原领料明细的已退数量更新失败。"));
            rollback();
            return false;
        }
    }

    QSqlQuery remaining(m_database);
    remaining.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM business_document_items WHERE document_id=? "
        "AND reversed_quantity<quantity-CASE WHEN ?='SCLL' "
        "THEN returned_quantity ELSE 0 END-0.0000001"));
    remaining.addBindValue(sourceDocumentId);
    remaining.addBindValue(sourceType);
    if (!remaining.exec() || !remaining.next()) {
        setError(errorMessage, remaining.lastError().text());
        rollback();
        return false;
    }
    const QString newStatus = remaining.value(0).toInt() == 0
        ? QStringLiteral("REVERSED") : QStringLiteral("PARTIALLY_REVERSED");
    QSqlQuery updateSource(m_database);
    updateSource.prepare(QStringLiteral(
        "UPDATE business_documents SET status=?, updated_at=? WHERE id=?"));
    updateSource.addBindValue(newStatus);
    updateSource.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    updateSource.addBindValue(sourceDocumentId);

    if (!updateSource.exec() || !finalizeDocument(documentId, errorMessage)
        || (sourceType == QStringLiteral("CPRK")
            && !refreshProductionRunStatus(productionRunId, errorMessage))
        || !writeAudit(QStringLiteral("REVERSE"), QStringLiteral("business_document"),
                       sourceDocumentId,
                       QStringLiteral("%1 -> %2").arg(sourceNumber, number), errorMessage)
        || !commit(errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()
            && !updateSource.lastError().text().isEmpty()) {
            *errorMessage = updateSource.lastError().text();
        }
        rollback();
        return false;
    }

    if (postedDocument) {
        postedDocument->documentId = documentId;
        postedDocument->documentNumber = number;
    }
    return true;
}

QStringList InventoryService::previewSerialNumbers(qlonglong materialId,
                                                   const QString &prefix,
                                                   int count,
                                                   QString *errorMessage) const
{
    if (materialId <= 0 || count <= 0 || count > 10000) {
        setError(errorMessage, QStringLiteral("SN生成数量必须在1到10000之间。"));
        return {};
    }
    QString cleanPrefix = prefix.trimmed().toUpper();
    cleanPrefix.remove(QRegularExpression(QStringLiteral("[^A-Z0-9_-]")));
    if (cleanPrefix.isEmpty()) {
        setError(errorMessage, QStringLiteral("SN前缀不能为空。"));
        return {};
    }
    const QString month = QDate::currentDate().toString(QStringLiteral("yyyyMM"));
    const QString base = QStringLiteral("%1-%2-").arg(cleanPrefix, month);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT serial_no FROM serial_numbers WHERE material_id=? AND serial_no LIKE ? "
        "ORDER BY serial_no DESC LIMIT 1"));
    query.addBindValue(materialId);
    query.addBindValue(base + QStringLiteral("%"));
    if (!query.exec()) {
        setError(errorMessage, query.lastError().text());
        return {};
    }
    int next = 1;
    if (query.next()) {
        bool ok = false;
        const int existing = query.value(0).toString().section(QLatin1Char('-'), -1).toInt(&ok);
        if (ok) {
            next = existing + 1;
        }
    }
    QStringList serials;
    serials.reserve(count);
    for (int i = 0; i < count; ++i) {
        serials.append(base + QStringLiteral("%1").arg(next + i, 5, 10, QLatin1Char('0')));
    }
    return serials;
}

bool InventoryService::beginImmediate(QString *errorMessage)
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("BEGIN IMMEDIATE TRANSACTION"))) {
        setError(errorMessage, QStringLiteral("无法开始库存事务：%1").arg(query.lastError().text()));
        return false;
    }
    return true;
}

void InventoryService::rollback()
{
    QSqlQuery(m_database).exec(QStringLiteral("ROLLBACK"));
}

bool InventoryService::commit(QString *errorMessage)
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("COMMIT"))) {
        setError(errorMessage, QStringLiteral("库存事务提交失败：%1").arg(query.lastError().text()));
        return false;
    }
    return true;
}

InventoryService::MaterialRules InventoryService::materialRules(qlonglong materialId,
                                                                 QString *errorMessage) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT code, require_batch, require_serial FROM materials WHERE id=? AND is_active=1"));
    query.addBindValue(materialId);
    if (!query.exec() || !query.next()) {
        setError(errorMessage, QStringLiteral("所选物料不存在或已停用。"));
        return {};
    }
    return {true, query.value(1).toBool(), query.value(2).toBool(), query.value(0).toString()};
}

bool InventoryService::validateLocation(qlonglong warehouseId,
                                        qlonglong locationId,
                                        QString *errorMessage) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM locations l JOIN warehouses w ON w.id=l.warehouse_id "
        "WHERE l.id=? AND w.id=? AND l.is_active=1 AND w.is_active=1"));
    query.addBindValue(locationId);
    query.addBindValue(warehouseId);
    if (!query.exec() || !query.next()) {
        setError(errorMessage, QStringLiteral("仓库与库位不匹配或已停用。"));
        return false;
    }
    return true;
}

bool InventoryService::validateMovement(const StockMovementRequest &request,
                                        const MaterialRules &rules,
                                        QString *errorMessage) const
{
    if (!request.documentDate.isValid() || request.documentType.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("业务类型或日期无效。"));
        return false;
    }
    if (request.quantity <= QuantityTolerance) {
        setError(errorMessage, QStringLiteral("数量必须大于0。"));
        return false;
    }
    if (rules.requireBatch && request.batchNo.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("该物料启用了批次管理，必须填写批次号。"));
        return false;
    }
    if (rules.requireSerial) {
        if (!isWholeNumber(request.quantity)) {
            setError(errorMessage, QStringLiteral("SN管理物料的数量必须是整数。"));
            return false;
        }
        if (request.serialNumbers.size() != static_cast<int>(std::round(request.quantity))) {
            setError(errorMessage, QStringLiteral("SN数量必须与业务数量一致。"));
            return false;
        }
        QSet<QString> unique;
        for (const QString &serial : request.serialNumbers) {
            const QString normalized = serial.trimmed().toUpper();
            if (normalized.isEmpty() || unique.contains(normalized)) {
                setError(errorMessage, QStringLiteral("SN不能为空且不能重复。"));
                return false;
            }
            unique.insert(normalized);
        }
    } else if (!request.serialNumbers.isEmpty()) {
        setError(errorMessage, QStringLiteral("该物料未启用SN管理，请清空SN列表。"));
        return false;
    }
    return true;
}

QString InventoryService::nextDocumentNumber(const QString &documentType,
                                             const QDate &documentDate,
                                             QString *errorMessage)
{
    const QString type = documentType.trimmed().toUpper();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT prefix, sequence_date, current_sequence, sequence_width "
        "FROM number_rules WHERE document_type=?"));
    query.addBindValue(type);
    if (!query.exec()) {
        setError(errorMessage, query.lastError().text());
        return {};
    }
    QString prefix = type;
    QString sequenceDate;
    int sequence = 0;
    int width = 4;
    if (query.next()) {
        prefix = query.value(0).toString();
        sequenceDate = query.value(1).toString();
        sequence = query.value(2).toInt();
        width = query.value(3).toInt();
    } else {
        QSqlQuery insert(m_database);
        insert.prepare(QStringLiteral(
            "INSERT INTO number_rules(document_type, prefix, sequence_width) VALUES(?, ?, 4)"));
        insert.addBindValue(type);
        insert.addBindValue(type);
        if (!insert.exec()) {
            setError(errorMessage, insert.lastError().text());
            return {};
        }
    }

    const QString isoDate = documentDate.toString(Qt::ISODate);
    sequence = sequenceDate == isoDate ? sequence + 1 : 1;
    int maximumSequence = 1;
    for (int digit = 0; digit < width; ++digit) maximumSequence *= 10;
    --maximumSequence;
    if (sequence > maximumSequence) {
        setError(errorMessage,
                 QStringLiteral("%1 在 %2 的%3位流水号已用完。")
                     .arg(type, documentDate.toString(QStringLiteral("yyyy-MM-dd")))
                     .arg(width));
        return {};
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE number_rules SET sequence_date=?, current_sequence=? WHERE document_type=?"));
    update.addBindValue(isoDate);
    update.addBindValue(sequence);
    update.addBindValue(type);
    if (!update.exec()) {
        setError(errorMessage, update.lastError().text());
        return {};
    }
    const QString datePart = documentDate.toString(QStringLiteral("yyyyMMdd"));
    const QString sequencePart = QStringLiteral("%1").arg(
        sequence, width, 10, QLatin1Char('0'));
    if (type == QStringLiteral("SCLL")) {
        return prefix + datePart + sequencePart;
    }
    return QStringLiteral("%1-%2-%3").arg(prefix, datePart, sequencePart);
}

qlonglong InventoryService::createDocument(const QString &number,
                                            const QString &type,
                                            const QString &direction,
                                            const QDate &date,
                                            const QString &handler,
                                            const QString &purpose,
                                            const QString &notes,
                                            const QVariant &sourceDocumentId,
                                            QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO business_documents(document_no, document_type, stock_direction, document_date, "
        "status, source_document_id, handler_name, purpose, notes, created_by) "
        "VALUES(?, ?, ?, ?, 'DRAFT', ?, ?, ?, ?, ?)"));
    query.addBindValue(number);
    query.addBindValue(type);
    query.addBindValue(direction);
    query.addBindValue(date.toString(Qt::ISODate));
    query.addBindValue(sourceDocumentId);
    query.addBindValue(databaseText(handler));
    query.addBindValue(databaseText(purpose));
    query.addBindValue(databaseText(notes));
    query.addBindValue(m_operatorId);
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("创建业务单据失败：%1").arg(query.lastError().text()));
        return 0;
    }
    return query.lastInsertId().toLongLong();
}

qlonglong InventoryService::createItem(qlonglong documentId,
                                       const StockMovementRequest &request,
                                       qlonglong targetWarehouseId,
                                       qlonglong targetLocationId,
                                       QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO business_document_items(document_id,line_number,material_id,quantity,"
        "ordered_quantity,gift_quantity,batch_no,warehouse_id,location_id,"
        "target_warehouse_id,target_location_id,notes) "
        "VALUES(?,1,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(documentId);
    query.addBindValue(request.materialId);
    query.addBindValue(request.quantity);
    query.addBindValue(request.orderedQuantity);
    query.addBindValue(request.giftQuantity);
    query.addBindValue(databaseText(request.batchNo));
    query.addBindValue(request.warehouseId);
    query.addBindValue(request.locationId);
    query.addBindValue(targetWarehouseId > 0 ? QVariant(targetWarehouseId) : QVariant());
    query.addBindValue(targetLocationId > 0 ? QVariant(targetLocationId) : QVariant());
    query.addBindValue(databaseText(request.notes));
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("创建业务明细失败：%1").arg(query.lastError().text()));
        return 0;
    }
    return query.lastInsertId().toLongLong();
}

bool InventoryService::changeBalance(qlonglong materialId,
                                     qlonglong warehouseId,
                                     qlonglong locationId,
                                     const QString &batchNo,
                                     double delta,
                                     double *quantityBefore,
                                     double *quantityAfter,
                                     QString *errorMessage)
{
    QSqlQuery select(m_database);
    select.prepare(QStringLiteral(
        "SELECT id, quantity FROM stock_balances "
        "WHERE material_id=? AND warehouse_id=? AND location_id=? AND batch_no=?"));
    select.addBindValue(materialId);
    select.addBindValue(warehouseId);
    select.addBindValue(locationId);
    select.addBindValue(databaseText(batchNo));
    if (!select.exec()) {
        setError(errorMessage, select.lastError().text());
        return false;
    }

    qlonglong balanceId = 0;
    double before = 0.0;
    if (select.next()) {
        balanceId = select.value(0).toLongLong();
        before = select.value(1).toDouble();
    }
    double after = before + delta;
    if (after < -QuantityTolerance) {
        setError(errorMessage, QStringLiteral("库存不足。当前库存为 %1，本次需要 %2。")
                                   .arg(before, 0, 'f', 6)
                                   .arg(std::abs(delta), 0, 'f', 6));
        return false;
    }
    if (std::abs(after) < QuantityTolerance) {
        after = 0.0;
    }

    QSqlQuery update(m_database);
    if (balanceId > 0) {
        update.prepare(QStringLiteral(
            "UPDATE stock_balances SET quantity=?, updated_at=? WHERE id=?"));
        update.addBindValue(after);
        update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        update.addBindValue(balanceId);
    } else {
        update.prepare(QStringLiteral(
            "INSERT INTO stock_balances(material_id, warehouse_id, location_id, batch_no, quantity) "
            "VALUES(?, ?, ?, ?, ?)"));
        update.addBindValue(materialId);
        update.addBindValue(warehouseId);
        update.addBindValue(locationId);
        update.addBindValue(databaseText(batchNo));
        update.addBindValue(after);
    }
    if (!update.exec()) {
        setError(errorMessage, QStringLiteral("更新库存失败：%1").arg(update.lastError().text()));
        return false;
    }
    if (quantityBefore) {
        *quantityBefore = before;
    }
    if (quantityAfter) {
        *quantityAfter = after;
    }
    return true;
}

qlonglong InventoryService::createLedger(qlonglong documentId,
                                         qlonglong itemId,
                                         const QString &businessType,
                                         qlonglong materialId,
                                         const QString &batchNo,
                                         double quantityIn,
                                         double quantityOut,
                                         double quantityBefore,
                                         double quantityAfter,
                                         qlonglong warehouseId,
                                         qlonglong locationId,
                                         const QString &notes,
                                         QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO inventory_ledger(business_type, document_id, document_item_id, material_id, batch_no, "
        "quantity_in, quantity_out, quantity_before, quantity_after, warehouse_id, location_id, operator_id, notes) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(businessType);
    query.addBindValue(documentId);
    query.addBindValue(itemId);
    query.addBindValue(materialId);
    query.addBindValue(databaseText(batchNo));
    query.addBindValue(quantityIn);
    query.addBindValue(quantityOut);
    query.addBindValue(quantityBefore);
    query.addBindValue(quantityAfter);
    query.addBindValue(warehouseId);
    query.addBindValue(locationId);
    query.addBindValue(m_operatorId);
    query.addBindValue(databaseText(notes));
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("生成库存流水失败：%1").arg(query.lastError().text()));
        return 0;
    }
    return query.lastInsertId().toLongLong();
}

bool InventoryService::attachSerialsToInbound(const StockMovementRequest &request,
                                              qlonglong documentId,
                                              qlonglong ledgerId,
                                              QString *errorMessage)
{
    const QString inboundAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    for (const QString &serial : request.serialNumbers) {
        const QString serialNo = serial.trimmed().toUpper();
        // 全局SN唯一：先查已有记录。不存在则按原逻辑新增；存在时只允许“同物料且已作废”
        // 的SN复用（例如入库被撤销后重新入库），保留原SN主键和历史流水关联。
        QSqlQuery existing(m_database);
        existing.prepare(QStringLiteral(
            "SELECT id,material_id,status FROM serial_numbers WHERE serial_no=?"));
        existing.addBindValue(serialNo);
        if (!existing.exec()) {
            setError(errorMessage, existing.lastError().text());
            return false;
        }
        if (existing.next()) {
            const qlonglong serialId = existing.value(0).toLongLong();
            if (existing.value(1).toLongLong() != request.materialId) {
                setError(errorMessage,
                         QStringLiteral("SN %1 已属于其他物料，不能用于本次入库。").arg(serialNo));
                return false;
            }
            const QString status = existing.value(2).toString();
            if (status != QStringLiteral("VOIDED")) {
                setError(errorMessage,
                         QStringLiteral("SN %1 已存在且状态为“%2”，只有已作废的SN才能重新入库。")
                             .arg(serialNo, serialStatusText(status)));
                return false;
            }
            QSqlQuery reuse(m_database);
            reuse.prepare(QStringLiteral(
                "UPDATE serial_numbers SET status='IN_STOCK', batch_no=?, warehouse_id=?, "
                "location_id=?, production_batch='', inbound_at=?, outbound_at=NULL, "
                "last_document_id=? WHERE id=? AND material_id=? AND status='VOIDED'"));
            reuse.addBindValue(databaseText(request.batchNo));
            reuse.addBindValue(request.warehouseId);
            reuse.addBindValue(request.locationId);
            reuse.addBindValue(inboundAt);
            reuse.addBindValue(documentId);
            reuse.addBindValue(serialId);
            reuse.addBindValue(request.materialId);
            // 影响行数校验用于防止并发下SN状态被其他操作改变。
            if (!reuse.exec() || reuse.numRowsAffected() != 1) {
                setError(errorMessage,
                         QStringLiteral("SN %1 的状态已被其他操作改变，请刷新后重试。").arg(serialNo));
                return false;
            }
            if (!linkLedgerSerial(ledgerId, serialId, errorMessage)) {
                return false;
            }
            continue;
        }
        QSqlQuery insert(m_database);
        insert.prepare(QStringLiteral(
            "INSERT INTO serial_numbers(material_id, serial_no, batch_no, status, warehouse_id, location_id, "
            "inbound_at, last_document_id) VALUES(?, ?, ?, 'IN_STOCK', ?, ?, ?, ?)"));
        insert.addBindValue(request.materialId);
        insert.addBindValue(serialNo);
        insert.addBindValue(databaseText(request.batchNo));
        insert.addBindValue(request.warehouseId);
        insert.addBindValue(request.locationId);
        insert.addBindValue(inboundAt);
        insert.addBindValue(documentId);
        if (!insert.exec()) {
            setError(errorMessage, QStringLiteral("SN %1 入库失败：%2")
                                       .arg(serialNo, insert.lastError().text()));
            return false;
        }
        if (!linkLedgerSerial(ledgerId, insert.lastInsertId().toLongLong(), errorMessage)) {
            return false;
        }
    }
    return true;
}

bool InventoryService::attachSerialsToOutbound(const StockMovementRequest &request,
                                               qlonglong documentId,
                                               qlonglong ledgerId,
                                               QString *errorMessage)
{
    for (const QString &serial : request.serialNumbers) {
        QSqlQuery select(m_database);
        select.prepare(QStringLiteral(
            "SELECT id FROM serial_numbers WHERE serial_no=? AND material_id=? AND batch_no=? "
            "AND status='IN_STOCK' AND warehouse_id=? AND location_id=?"));
        select.addBindValue(serial.trimmed().toUpper());
        select.addBindValue(request.materialId);
        select.addBindValue(databaseText(request.batchNo));
        select.addBindValue(request.warehouseId);
        select.addBindValue(request.locationId);
        if (!select.exec() || !select.next()) {
            setError(errorMessage, QStringLiteral("SN %1 不在所选批次和库位的可用库存中。").arg(serial));
            return false;
        }
        const qlonglong serialId = select.value(0).toLongLong();
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral(
            "UPDATE serial_numbers SET status='OUTBOUND', warehouse_id=NULL, location_id=NULL, "
            "outbound_at=?, last_document_id=? WHERE id=?"));
        update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        update.addBindValue(documentId);
        update.addBindValue(serialId);
        if (!update.exec()) {
            setError(errorMessage, update.lastError().text());
            return false;
        }
        if (!linkLedgerSerial(ledgerId, serialId, errorMessage)) {
            return false;
        }
    }
    return true;
}

bool InventoryService::linkLedgerSerial(qlonglong ledgerId,
                                        qlonglong serialId,
                                        QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO inventory_ledger_serials(ledger_id, serial_id) VALUES(?, ?)"));
    query.addBindValue(ledgerId);
    query.addBindValue(serialId);
    if (!query.exec()) {
        setError(errorMessage, query.lastError().text());
        return false;
    }
    return true;
}

bool InventoryService::finalizeDocument(qlonglong documentId, QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE business_documents SET status='POSTED', posted_at=?, updated_at=? WHERE id=? AND status='DRAFT'"));
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(documentId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(errorMessage, QStringLiteral("业务单据生效失败：%1").arg(query.lastError().text()));
        return false;
    }
    return true;
}

bool InventoryService::writeAudit(const QString &action,
                                  const QString &entityType,
                                  qlonglong entityId,
                                  const QString &detail,
                                  QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id, action, entity_type, entity_id, detail) VALUES(?, ?, ?, ?, ?)"));
    query.addBindValue(m_operatorId);
    query.addBindValue(action);
    query.addBindValue(entityType);
    query.addBindValue(entityId);
    query.addBindValue(databaseText(detail));
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("记录操作日志失败：%1").arg(query.lastError().text()));
        return false;
    }
    return true;
}
