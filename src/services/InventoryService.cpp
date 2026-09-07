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
            "INSERT OR IGNORE INTO batches(material_id, batch_no, first_in_at) VALUES(?, ?, ?)"));
        batch.addBindValue(request.materialId);
        batch.addBindValue(databaseText(request.batchNo));
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
    const MaterialRules rules = materialRules(request.materialId, errorMessage);
    if (!rules.valid || !validateMovement(request, rules, errorMessage)
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

bool InventoryService::reverseItem(const ReversalRequest &request,
                                   PostedDocument *postedDocument,
                                   QString *errorMessage)
{
    if (request.sourceItemId <= 0 || request.quantity <= QuantityTolerance || !request.documentDate.isValid()) {
        setError(errorMessage, QStringLiteral("撤销明细、日期或数量无效。"));
        return false;
    }

    QSqlQuery source(m_database);
    source.prepare(QStringLiteral(
        "SELECT i.document_id, d.document_no, d.stock_direction, d.status, i.material_id, i.quantity, "
        "i.reversed_quantity, i.batch_no, i.warehouse_id, i.location_id, m.require_serial "
        "FROM business_document_items i "
        "JOIN business_documents d ON d.id = i.document_id "
        "JOIN materials m ON m.id = i.material_id WHERE i.id = ?"));
    source.addBindValue(request.sourceItemId);
    if (!source.exec() || !source.next()) {
        setError(errorMessage, QStringLiteral("找不到需要撤销的原业务明细。"));
        return false;
    }

    const qlonglong sourceDocumentId = source.value(0).toLongLong();
    const QString sourceNumber = source.value(1).toString();
    const QString direction = source.value(2).toString();
    const QString sourceStatus = source.value(3).toString();
    const qlonglong materialId = source.value(4).toLongLong();
    const double originalQuantity = source.value(5).toDouble();
    const double reversedQuantity = source.value(6).toDouble();
    const QString batchNo = source.value(7).toString();
    const qlonglong warehouseId = source.value(8).toLongLong();
    const qlonglong locationId = source.value(9).toLongLong();
    const bool requireSerial = source.value(10).toBool();

    if (direction != QStringLiteral("IN") && direction != QStringLiteral("OUT")) {
        setError(errorMessage, QStringLiteral("当前版本仅支持入库和出库明细的部分撤销。"));
        return false;
    }
    if (sourceStatus == QStringLiteral("REVERSED")
        || request.quantity > originalQuantity - reversedQuantity + QuantityTolerance) {
        setError(errorMessage, QStringLiteral("撤销数量超过原明细的剩余可撤销数量。"));
        return false;
    }
    if (requireSerial
        && (!isWholeNumber(request.quantity)
            || request.serialNumbers.size() != static_cast<int>(std::round(request.quantity)))) {
        setError(errorMessage, QStringLiteral("SN管理物料必须选择与撤销数量一致的SN。"));
        return false;
    }

    if (!beginImmediate(errorMessage)) {
        return false;
    }
    const QString number = nextDocumentNumber(QStringLiteral("CX"), request.documentDate, errorMessage);
    if (number.isEmpty()) {
        rollback();
        return false;
    }

    const bool reversalInbound = direction == QStringLiteral("OUT");
    const qlonglong documentId = createDocument(number, QStringLiteral("CX"),
                                                 reversalInbound ? QStringLiteral("IN") : QStringLiteral("OUT"),
                                                 request.documentDate, request.handlerName, QString(), request.notes,
                                                 sourceDocumentId, errorMessage);
    StockMovementRequest itemRequest;
    itemRequest.documentType = QStringLiteral("CX");
    itemRequest.documentDate = request.documentDate;
    itemRequest.materialId = materialId;
    itemRequest.quantity = request.quantity;
    itemRequest.batchNo = batchNo;
    itemRequest.warehouseId = warehouseId;
    itemRequest.locationId = locationId;
    itemRequest.notes = request.notes;
    itemRequest.serialNumbers = request.serialNumbers;
    const qlonglong itemId = documentId > 0 ? createItem(documentId, itemRequest, 0, 0, errorMessage) : 0;
    if (itemId <= 0) {
        rollback();
        return false;
    }

    double before = 0.0;
    double after = 0.0;
    const double delta = reversalInbound ? request.quantity : -request.quantity;
    if (!changeBalance(materialId, warehouseId, locationId, batchNo, delta, &before, &after, errorMessage)) {
        rollback();
        return false;
    }
    const qlonglong ledgerId = createLedger(documentId, itemId, QStringLiteral("CX"), materialId, batchNo,
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
        if (reversalInbound) {
            find.prepare(QStringLiteral(
                "SELECT id FROM serial_numbers WHERE serial_no = ? AND material_id = ? "
                "AND status = 'OUTBOUND' AND last_document_id = ?"));
        } else {
            find.prepare(QStringLiteral(
                "SELECT id FROM serial_numbers WHERE serial_no = ? AND material_id = ? "
                "AND status = 'IN_STOCK' AND warehouse_id = ? AND location_id = ? AND batch_no = ?"));
        }
        find.addBindValue(serial.trimmed());
        find.addBindValue(materialId);
        if (reversalInbound) {
            find.addBindValue(sourceDocumentId);
        } else {
            find.addBindValue(warehouseId);
            find.addBindValue(locationId);
            find.addBindValue(databaseText(batchNo));
        }
        if (!find.exec() || !find.next()) {
            setError(errorMessage, QStringLiteral("SN %1 当前状态不允许撤销。").arg(serial));
            rollback();
            return false;
        }
        const qlonglong serialId = find.value(0).toLongLong();
        QSqlQuery update(m_database);
        update.prepare(reversalInbound
                           ? QStringLiteral("UPDATE serial_numbers SET status='IN_STOCK', warehouse_id=?, location_id=?, outbound_at=NULL, last_document_id=? WHERE id=?")
                           : QStringLiteral("UPDATE serial_numbers SET status='VOIDED', warehouse_id=NULL, location_id=NULL, last_document_id=? WHERE id=?"));
        if (reversalInbound) {
            update.addBindValue(warehouseId);
            update.addBindValue(locationId);
        }
        update.addBindValue(documentId);
        update.addBindValue(serialId);
        if (!update.exec() || !linkLedgerSerial(ledgerId, serialId, errorMessage)) {
            setError(errorMessage, update.lastError().text());
            rollback();
            return false;
        }
    }

    QSqlQuery updateItem(m_database);
    updateItem.prepare(QStringLiteral(
        "UPDATE business_document_items SET reversed_quantity = reversed_quantity + ? WHERE id = ?"));
    updateItem.addBindValue(request.quantity);
    updateItem.addBindValue(request.sourceItemId);
    if (!updateItem.exec()) {
        setError(errorMessage, updateItem.lastError().text());
        rollback();
        return false;
    }

    QSqlQuery remaining(m_database);
    remaining.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM business_document_items WHERE document_id = ? "
        "AND reversed_quantity < quantity - 0.0000001"));
    remaining.addBindValue(sourceDocumentId);
    if (!remaining.exec() || !remaining.next()) {
        setError(errorMessage, remaining.lastError().text());
        rollback();
        return false;
    }
    const QString newStatus = remaining.value(0).toInt() == 0
        ? QStringLiteral("REVERSED") : QStringLiteral("PARTIALLY_REVERSED");
    QSqlQuery updateSource(m_database);
    updateSource.prepare(QStringLiteral("UPDATE business_documents SET status=?, updated_at=? WHERE id=?"));
    updateSource.addBindValue(newStatus);
    updateSource.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    updateSource.addBindValue(sourceDocumentId);

    if (!updateSource.exec() || !finalizeDocument(documentId, errorMessage)
        || !writeAudit(QStringLiteral("REVERSE"), QStringLiteral("business_document"), sourceDocumentId,
                       QStringLiteral("%1 -> %2").arg(sourceNumber, number), errorMessage)
        || !commit(errorMessage)) {
        if (!updateSource.lastError().text().isEmpty()) {
            setError(errorMessage, updateSource.lastError().text());
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
    return QStringLiteral("%1-%2-%3")
        .arg(prefix, documentDate.toString(QStringLiteral("yyyyMMdd")),
             QStringLiteral("%1").arg(sequence, width, 10, QLatin1Char('0')));
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
        "INSERT INTO business_document_items(document_id, line_number, material_id, quantity, batch_no, "
        "warehouse_id, location_id, target_warehouse_id, target_location_id, notes) "
        "VALUES(?, 1, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(documentId);
    query.addBindValue(request.materialId);
    query.addBindValue(request.quantity);
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
    for (const QString &serial : request.serialNumbers) {
        QSqlQuery insert(m_database);
        insert.prepare(QStringLiteral(
            "INSERT INTO serial_numbers(material_id, serial_no, batch_no, status, warehouse_id, location_id, "
            "inbound_at, last_document_id) VALUES(?, ?, ?, 'IN_STOCK', ?, ?, ?, ?)"));
        insert.addBindValue(request.materialId);
        insert.addBindValue(serial.trimmed().toUpper());
        insert.addBindValue(databaseText(request.batchNo));
        insert.addBindValue(request.warehouseId);
        insert.addBindValue(request.locationId);
        insert.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        insert.addBindValue(documentId);
        if (!insert.exec()) {
            setError(errorMessage, QStringLiteral("SN %1 入库失败，可能已经存在：%2")
                                       .arg(serial, insert.lastError().text()));
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
