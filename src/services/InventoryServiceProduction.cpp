#include "services/InventoryService.h"

#include <QDateTime>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <cmath>

namespace {
constexpr double ProductionQuantityTolerance = 0.0000001;

void setProductionError(QString *target, const QString &message)
{
    if (target) {
        *target = message;
    }
}

QString normalizedDatabaseText(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isNull() ? QString::fromLatin1("", 0) : trimmed;
}

bool isProductionWholeNumber(double value)
{
    return std::abs(value - std::round(value)) < ProductionQuantityTolerance;
}

QString lineError(int lineNumber, const QString &message)
{
    return QStringLiteral("第 %1 行：%2").arg(lineNumber).arg(message);
}
}

qlonglong InventoryService::createDocumentItem(qlonglong documentId,
                                                int lineNumber,
                                                const StockMovementRequest &request,
                                                const QVariant &sourceItemId,
                                                QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO business_document_items(document_id, line_number, material_id, quantity, batch_no, "
        "warehouse_id, location_id, notes, source_item_id) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(documentId);
    query.addBindValue(lineNumber);
    query.addBindValue(request.materialId);
    query.addBindValue(request.quantity);
    query.addBindValue(normalizedDatabaseText(request.batchNo));
    query.addBindValue(request.warehouseId);
    query.addBindValue(request.locationId);
    query.addBindValue(normalizedDatabaseText(request.notes));
    query.addBindValue(sourceItemId);
    if (!query.exec()) {
        setProductionError(errorMessage,
                           QStringLiteral("创建业务明细失败：%1").arg(query.lastError().text()));
        return 0;
    }
    return query.lastInsertId().toLongLong();
}

bool InventoryService::setDocumentProductionContext(qlonglong documentId,
                                                     qlonglong productionRunId,
                                                     const QString &submissionToken,
                                                     QString *errorMessage)
{
    const QString token = submissionToken.trimmed();
    if (productionRunId <= 0 || token.isEmpty()) {
        setProductionError(errorMessage, QStringLiteral("生产批次或提交标识无效。"));
        return false;
    }

    QSqlQuery duplicate(m_database);
    duplicate.prepare(QStringLiteral("SELECT 1 FROM business_documents WHERE submission_token=?"));
    duplicate.addBindValue(token);
    if (!duplicate.exec()) {
        setProductionError(errorMessage, duplicate.lastError().text());
        return false;
    }
    if (duplicate.next()) {
        setProductionError(errorMessage, QStringLiteral("该单据已经提交，请勿重复操作。"));
        return false;
    }

    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE business_documents SET production_run_id=?, submission_token=? WHERE id=?"));
    update.addBindValue(productionRunId);
    update.addBindValue(token);
    update.addBindValue(documentId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        const QString detail = update.lastError().text();
        setProductionError(errorMessage,
                           detail.contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive)
                               ? QStringLiteral("该单据已经提交，请勿重复操作。")
                               : QStringLiteral("关联生产批次失败：%1").arg(detail));
        return false;
    }
    return true;
}

qlonglong InventoryService::ensureProductionRun(const ProductionRunRequest &run,
                                                QString *errorMessage)
{
    const QString batchNo = run.batchNo.trimmed().toUpper();
    if (batchNo.isEmpty() || run.productMaterialId <= 0
        || run.plannedQuantity <= ProductionQuantityTolerance) {
        setProductionError(errorMessage, QStringLiteral("成品物料、生产批次和计划数量不能为空。"));
        return 0;
    }

    QSqlQuery existing(m_database);
    existing.prepare(QStringLiteral(
        "SELECT id, product_material_id, planned_quantity FROM production_runs WHERE batch_no=?"));
    existing.addBindValue(batchNo);
    if (!existing.exec()) {
        setProductionError(errorMessage, existing.lastError().text());
        return 0;
    }
    if (existing.next()) {
        if (existing.value(1).toLongLong() != run.productMaterialId
            || std::abs(existing.value(2).toDouble() - run.plannedQuantity)
                   > ProductionQuantityTolerance) {
            setProductionError(errorMessage,
                               QStringLiteral("该生产批次已存在，成品或计划数量与原记录不一致。"));
            return 0;
        }
        return existing.value(0).toLongLong();
    }

    QSqlQuery material(m_database);
    material.prepare(QStringLiteral(
        "SELECT m.name, m.specification FROM materials m "
        "JOIN material_categories c ON c.id=m.category_id "
        "WHERE m.id=? AND m.is_active=1 AND c.code='FINISHED'"));
    material.addBindValue(run.productMaterialId);
    if (!material.exec() || !material.next()) {
        setProductionError(errorMessage, QStringLiteral("所选成品不存在、已停用或不属于成品分类。"));
        return 0;
    }

    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO production_runs(batch_no, product_material_id, product_name, product_model, "
        "planned_quantity, created_by) VALUES(?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(batchNo);
    insert.addBindValue(run.productMaterialId);
    insert.addBindValue(material.value(0));
    insert.addBindValue(normalizedDatabaseText(material.value(1).toString()));
    insert.addBindValue(run.plannedQuantity);
    insert.addBindValue(m_operatorId);
    if (!insert.exec()) {
        setProductionError(errorMessage,
                           QStringLiteral("创建生产批次失败：%1").arg(insert.lastError().text()));
        return 0;
    }
    return insert.lastInsertId().toLongLong();
}

bool InventoryService::postProductionIssue(const ProductionRunRequest &run,
                                            const StockDocumentRequest &document,
                                            PostedDocument *postedDocument,
                                            qlonglong *productionRunId,
                                            QString *errorMessage)
{
    if (!m_database.isOpen() || m_operatorId <= 0 || !document.documentDate.isValid()
        || document.lines.isEmpty() || document.submissionToken.trimmed().isEmpty()) {
        setProductionError(errorMessage, QStringLiteral("生产领料单头、明细或当前用户无效。"));
        return false;
    }
    if (!beginImmediate(errorMessage)) {
        return false;
    }

    const qlonglong runId = ensureProductionRun(run, errorMessage);
    if (runId <= 0) {
        rollback();
        return false;
    }
    const QString number = nextDocumentNumber(QStringLiteral("SCLL"), document.documentDate, errorMessage);
    const qlonglong documentId = number.isEmpty()
        ? 0
        : createDocument(number, QStringLiteral("SCLL"), QStringLiteral("OUT"),
                         document.documentDate, document.handlerName, document.purpose,
                         document.notes, QVariant(), errorMessage);
    if (documentId <= 0
        || !setDocumentProductionContext(documentId, runId, document.submissionToken, errorMessage)) {
        rollback();
        return false;
    }

    QSet<QString> identities;
    for (int index = 0; index < document.lines.size(); ++index) {
        StockMovementRequest movement = document.lines.at(index);
        movement.documentType = QStringLiteral("SCLL");
        movement.documentDate = document.documentDate;
        const QString identity = QStringLiteral("%1|%2|%3|%4")
                                     .arg(movement.materialId)
                                     .arg(movement.warehouseId)
                                     .arg(movement.locationId)
                                     .arg(movement.batchNo.trimmed().toUpper());
        if (identities.contains(identity)) {
            setProductionError(errorMessage,
                               lineError(index + 1, QStringLiteral("物料、库位和批次组合重复。")));
            rollback();
            return false;
        }
        identities.insert(identity);

        QString detail;
        const MaterialRules rules = materialRules(movement.materialId, &detail);
        if (!rules.valid || !validateMovement(movement, rules, &detail)
            || !validateLocation(movement.warehouseId, movement.locationId, &detail)) {
            setProductionError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
        const qlonglong itemId = createDocumentItem(documentId, index + 1, movement,
                                                     QVariant(), &detail);
        double before = 0.0;
        double after = 0.0;
        if (itemId <= 0
            || !changeBalance(movement.materialId, movement.warehouseId, movement.locationId,
                              movement.batchNo.trimmed(), -movement.quantity,
                              &before, &after, &detail)) {
            setProductionError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
        const qlonglong ledgerId = createLedger(documentId, itemId, QStringLiteral("SCLL"),
                                                 movement.materialId, movement.batchNo.trimmed(),
                                                 0.0, movement.quantity, before, after,
                                                 movement.warehouseId, movement.locationId,
                                                 movement.notes, &detail);
        if (ledgerId <= 0
            || !attachSerialsToOutbound(movement, documentId, ledgerId, &detail)) {
            setProductionError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
    }

    if (!finalizeDocument(documentId, errorMessage)
        || !writeAudit(QStringLiteral("POST"), QStringLiteral("business_document"),
                       documentId, number, errorMessage)
        || !commit(errorMessage)) {
        rollback();
        return false;
    }
    if (postedDocument) {
        postedDocument->documentId = documentId;
        postedDocument->documentNumber = number;
    }
    if (productionRunId) {
        *productionRunId = runId;
    }
    return true;
}

bool InventoryService::postProductionReturn(const ProductionReturnRequest &request,
                                             PostedDocument *postedDocument,
                                             QString *errorMessage)
{
    if (!m_database.isOpen() || m_operatorId <= 0 || request.sourceDocumentId <= 0
        || !request.documentDate.isValid() || request.lines.isEmpty()
        || request.submissionToken.trimmed().isEmpty()) {
        setProductionError(errorMessage, QStringLiteral("生产退料单头、明细或当前用户无效。"));
        return false;
    }
    if (!beginImmediate(errorMessage)) {
        return false;
    }

    QSqlQuery sourceDocument(m_database);
    sourceDocument.prepare(QStringLiteral(
        "SELECT production_run_id FROM business_documents WHERE id=? AND document_type='SCLL' "
        "AND status IN ('POSTED','PARTIALLY_REVERSED')"));
    sourceDocument.addBindValue(request.sourceDocumentId);
    if (!sourceDocument.exec() || !sourceDocument.next()
        || sourceDocument.value(0).toLongLong() <= 0) {
        setProductionError(errorMessage, QStringLiteral("原生产领料单不存在或当前不可退料。"));
        rollback();
        return false;
    }
    const qlonglong runId = sourceDocument.value(0).toLongLong();
    const QString number = nextDocumentNumber(QStringLiteral("SCTL"), request.documentDate, errorMessage);
    const qlonglong documentId = number.isEmpty()
        ? 0
        : createDocument(number, QStringLiteral("SCTL"), QStringLiteral("IN"),
                         request.documentDate, request.handlerName, QString(), request.notes,
                         request.sourceDocumentId, errorMessage);
    if (documentId <= 0
        || !setDocumentProductionContext(documentId, runId, request.submissionToken, errorMessage)) {
        rollback();
        return false;
    }

    QSet<qlonglong> sourceItems;
    QSet<QString> selectedSerials;
    for (int index = 0; index < request.lines.size(); ++index) {
        const ProductionReturnLine returnLine = request.lines.at(index);
        QString detail;
        if (returnLine.sourceItemId <= 0 || sourceItems.contains(returnLine.sourceItemId)) {
            setProductionError(errorMessage,
                               lineError(index + 1, QStringLiteral("原领料明细无效或重复。")));
            rollback();
            return false;
        }
        sourceItems.insert(returnLine.sourceItemId);

        QSqlQuery sourceItem(m_database);
        sourceItem.prepare(QStringLiteral(
            "SELECT i.material_id, i.quantity, i.returned_quantity, i.reversed_quantity, "
            "i.batch_no, m.require_serial FROM business_document_items i "
            "JOIN materials m ON m.id=i.material_id "
            "WHERE i.id=? AND i.document_id=?"));
        sourceItem.addBindValue(returnLine.sourceItemId);
        sourceItem.addBindValue(request.sourceDocumentId);
        if (!sourceItem.exec() || !sourceItem.next()) {
            setProductionError(errorMessage, lineError(index + 1, QStringLiteral("找不到原领料明细。")));
            rollback();
            return false;
        }
        const qlonglong materialId = sourceItem.value(0).toLongLong();
        const double remaining = sourceItem.value(1).toDouble()
                                 - sourceItem.value(2).toDouble()
                                 - sourceItem.value(3).toDouble();
        const QString batchNo = sourceItem.value(4).toString();
        const bool requireSerial = sourceItem.value(5).toBool();
        if (returnLine.quantity <= ProductionQuantityTolerance
            || returnLine.quantity > remaining + ProductionQuantityTolerance) {
            setProductionError(errorMessage,
                               lineError(index + 1,
                                         QStringLiteral("退料数量超过剩余可退数量 %1。")
                                             .arg(remaining, 0, 'f', 6)));
            rollback();
            return false;
        }
        if (!validateLocation(returnLine.warehouseId, returnLine.locationId, &detail)) {
            setProductionError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
        if (requireSerial
            && (!isProductionWholeNumber(returnLine.quantity)
                || returnLine.serialNumbers.size()
                       != static_cast<int>(std::round(returnLine.quantity)))) {
            setProductionError(errorMessage,
                               lineError(index + 1, QStringLiteral("SN数量必须与退料数量一致。")));
            rollback();
            return false;
        }
        if (!requireSerial && !returnLine.serialNumbers.isEmpty()) {
            setProductionError(errorMessage,
                               lineError(index + 1, QStringLiteral("该物料未启用SN管理。")));
            rollback();
            return false;
        }

        StockMovementRequest movement;
        movement.documentType = QStringLiteral("SCTL");
        movement.documentDate = request.documentDate;
        movement.materialId = materialId;
        movement.quantity = returnLine.quantity;
        movement.batchNo = batchNo;
        movement.warehouseId = returnLine.warehouseId;
        movement.locationId = returnLine.locationId;
        movement.notes = returnLine.notes;
        movement.serialNumbers = returnLine.serialNumbers;
        const qlonglong itemId = createDocumentItem(documentId, index + 1, movement,
                                                     returnLine.sourceItemId, &detail);
        double before = 0.0;
        double after = 0.0;
        if (itemId <= 0
            || !changeBalance(materialId, returnLine.warehouseId, returnLine.locationId,
                              batchNo, returnLine.quantity, &before, &after, &detail)) {
            setProductionError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
        const qlonglong ledgerId = createLedger(documentId, itemId, QStringLiteral("SCTL"),
                                                 materialId, batchNo, returnLine.quantity, 0.0,
                                                 before, after, returnLine.warehouseId,
                                                 returnLine.locationId, returnLine.notes, &detail);
        if (ledgerId <= 0) {
            setProductionError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }

        for (const QString &serialValue : returnLine.serialNumbers) {
            const QString serial = serialValue.trimmed().toUpper();
            if (serial.isEmpty() || selectedSerials.contains(serial)) {
                setProductionError(errorMessage,
                                   lineError(index + 1, QStringLiteral("SN不能为空且不能重复。")));
                rollback();
                return false;
            }
            selectedSerials.insert(serial);
            QSqlQuery find(m_database);
            find.prepare(QStringLiteral(
                "SELECT sn.id FROM serial_numbers sn "
                "JOIN inventory_ledger_serials ils ON ils.serial_id=sn.id "
                "JOIN inventory_ledger l ON l.id=ils.ledger_id "
                "WHERE sn.serial_no=? AND sn.material_id=? AND sn.status='OUTBOUND' "
                "AND l.document_item_id=? AND l.business_type='SCLL'"));
            find.addBindValue(serial);
            find.addBindValue(materialId);
            find.addBindValue(returnLine.sourceItemId);
            if (!find.exec() || !find.next()) {
                setProductionError(errorMessage,
                                   lineError(index + 1,
                                             QStringLiteral("SN %1 不属于该原领料明细或已退回。")
                                                 .arg(serial)));
                rollback();
                return false;
            }
            const qlonglong serialId = find.value(0).toLongLong();
            QSqlQuery updateSerial(m_database);
            updateSerial.prepare(QStringLiteral(
                "UPDATE serial_numbers SET status='IN_STOCK', warehouse_id=?, location_id=?, "
                "outbound_at=NULL, last_document_id=? WHERE id=? AND status='OUTBOUND'"));
            updateSerial.addBindValue(returnLine.warehouseId);
            updateSerial.addBindValue(returnLine.locationId);
            updateSerial.addBindValue(documentId);
            updateSerial.addBindValue(serialId);
            if (!updateSerial.exec() || updateSerial.numRowsAffected() != 1
                || !linkLedgerSerial(ledgerId, serialId, &detail)) {
                setProductionError(errorMessage,
                                   lineError(index + 1,
                                             detail.isEmpty() ? updateSerial.lastError().text() : detail));
                rollback();
                return false;
            }
        }

        QSqlQuery updateSource(m_database);
        updateSource.prepare(QStringLiteral(
            "UPDATE business_document_items SET returned_quantity=returned_quantity+? "
            "WHERE id=? AND returned_quantity+?<=quantity-reversed_quantity+0.0000001"));
        updateSource.addBindValue(returnLine.quantity);
        updateSource.addBindValue(returnLine.sourceItemId);
        updateSource.addBindValue(returnLine.quantity);
        if (!updateSource.exec() || updateSource.numRowsAffected() != 1) {
            setProductionError(errorMessage,
                               lineError(index + 1, QStringLiteral("原领料明细可退数量已发生变化。")));
            rollback();
            return false;
        }
    }

    if (!finalizeDocument(documentId, errorMessage)
        || !writeAudit(QStringLiteral("POST"), QStringLiteral("business_document"),
                       documentId, number, errorMessage)
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

bool InventoryService::postFinishedGoodsInbound(const StockDocumentRequest &document,
                                                 PostedDocument *postedDocument,
                                                 QString *errorMessage)
{
    if (!m_database.isOpen() || m_operatorId <= 0 || document.productionRunId <= 0
        || !document.documentDate.isValid() || document.lines.size() != 1
        || document.submissionToken.trimmed().isEmpty()) {
        setProductionError(errorMessage, QStringLiteral("成品入库单头、明细或当前用户无效。"));
        return false;
    }
    if (!beginImmediate(errorMessage)) {
        return false;
    }

    QSqlQuery run(m_database);
    run.prepare(QStringLiteral(
        "SELECT product_material_id, batch_no FROM production_runs WHERE id=?"));
    run.addBindValue(document.productionRunId);
    if (!run.exec() || !run.next()) {
        setProductionError(errorMessage, QStringLiteral("找不到生产批次。"));
        rollback();
        return false;
    }
    const qlonglong productMaterialId = run.value(0).toLongLong();
    const QString productionBatch = run.value(1).toString();
    StockMovementRequest movement = document.lines.first();
    movement.documentType = QStringLiteral("CPRK");
    movement.documentDate = document.documentDate;
    movement.batchNo = productionBatch;
    if (movement.materialId != productMaterialId) {
        setProductionError(errorMessage, QStringLiteral("入库成品与生产批次中的成品不一致。"));
        rollback();
        return false;
    }

    QString detail;
    const MaterialRules rules = materialRules(movement.materialId, &detail);
    if (!rules.valid || !validateMovement(movement, rules, &detail)
        || !validateLocation(movement.warehouseId, movement.locationId, &detail)) {
        setProductionError(errorMessage, detail);
        rollback();
        return false;
    }

    const QString number = nextDocumentNumber(QStringLiteral("CPRK"), document.documentDate, errorMessage);
    const qlonglong documentId = number.isEmpty()
        ? 0
        : createDocument(number, QStringLiteral("CPRK"), QStringLiteral("IN"),
                         document.documentDate, document.handlerName, document.purpose,
                         document.notes, QVariant(), errorMessage);
    if (documentId <= 0
        || !setDocumentProductionContext(documentId, document.productionRunId,
                                          document.submissionToken, errorMessage)) {
        rollback();
        return false;
    }
    const qlonglong itemId = createDocumentItem(documentId, 1, movement, QVariant(), &detail);
    double before = 0.0;
    double after = 0.0;
    if (itemId <= 0
        || !changeBalance(movement.materialId, movement.warehouseId, movement.locationId,
                          productionBatch, movement.quantity, &before, &after, &detail)) {
        setProductionError(errorMessage, detail);
        rollback();
        return false;
    }
    const qlonglong ledgerId = createLedger(documentId, itemId, QStringLiteral("CPRK"),
                                             movement.materialId, productionBatch,
                                             movement.quantity, 0.0, before, after,
                                             movement.warehouseId, movement.locationId,
                                             movement.notes, &detail);
    if (ledgerId <= 0
        || !attachSerialsToInbound(movement, documentId, ledgerId, &detail)) {
        setProductionError(errorMessage, detail);
        rollback();
        return false;
    }
    if (!movement.serialNumbers.isEmpty()) {
        QSqlQuery serialBatch(m_database);
        serialBatch.prepare(QStringLiteral(
            "UPDATE serial_numbers SET production_batch=? WHERE last_document_id=?"));
        serialBatch.addBindValue(productionBatch);
        serialBatch.addBindValue(documentId);
        if (!serialBatch.exec()) {
            setProductionError(errorMessage, serialBatch.lastError().text());
            rollback();
            return false;
        }
    }
    QSqlQuery batch(m_database);
    batch.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO batches(material_id, batch_no, first_in_at) VALUES(?, ?, ?)"));
    batch.addBindValue(movement.materialId);
    batch.addBindValue(productionBatch);
    batch.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    if (!batch.exec() || !finalizeDocument(documentId, errorMessage)
        || !refreshProductionRunStatus(document.productionRunId, errorMessage)
        || !writeAudit(QStringLiteral("POST"), QStringLiteral("business_document"),
                       documentId, number, errorMessage)
        || !commit(errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = batch.lastError().text();
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

double InventoryService::productionRunReceivedQuantity(qlonglong productionRunId,
                                                       QString *errorMessage) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT COALESCE(SUM(i.quantity-i.reversed_quantity),0) "
        "FROM business_documents d JOIN business_document_items i ON i.document_id=d.id "
        "WHERE d.production_run_id=? AND d.document_type='CPRK' "
        "AND d.status IN ('POSTED','PARTIALLY_REVERSED')"));
    query.addBindValue(productionRunId);
    if (!query.exec() || !query.next()) {
        setProductionError(errorMessage,
                           QStringLiteral("统计成品入库数量失败：%1").arg(query.lastError().text()));
        return -1.0;
    }
    return query.value(0).toDouble();
}

bool InventoryService::refreshProductionRunStatus(qlonglong productionRunId,
                                                  QString *errorMessage)
{
    if (productionRunId <= 0) {
        return true;
    }
    QSqlQuery planned(m_database);
    planned.prepare(QStringLiteral("SELECT planned_quantity FROM production_runs WHERE id=?"));
    planned.addBindValue(productionRunId);
    if (!planned.exec() || !planned.next()) {
        setProductionError(errorMessage, QStringLiteral("找不到需要更新的生产批次。"));
        return false;
    }
    const double received = productionRunReceivedQuantity(productionRunId, errorMessage);
    if (received < -ProductionQuantityTolerance) {
        return false;
    }
    const QString status = received + ProductionQuantityTolerance >= planned.value(0).toDouble()
        ? QStringLiteral("COMPLETED") : QStringLiteral("OPEN");
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE production_runs SET status=?, updated_at=? WHERE id=?"));
    update.addBindValue(status);
    update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    update.addBindValue(productionRunId);
    if (!update.exec()) {
        setProductionError(errorMessage, update.lastError().text());
        return false;
    }
    return true;
}
