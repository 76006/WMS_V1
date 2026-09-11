#include "services/InventoryService.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <cmath>
#include <utility>

namespace {
constexpr double QuantityTolerance = 0.0000001;

void setError(QString *target, const QString &message)
{
    if (target) *target = message;
}

QString normalized(const QString &value)
{
    const QString result = value.trimmed();
    return result.isNull() ? QString::fromLatin1("", 0) : result;
}

QString revisedLedgerTime(QString value, const QDate &oldDate, const QDate &newDate)
{
    // SQLite 的空格分隔时间和 Qt 的 ISO 时间都按同一种精度解析、保存。
    if (value.size() > 10 && value.at(10) == QLatin1Char(' ')) value[10] = QLatin1Char('T');
    QDateTime time = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!time.isValid()) time = QDateTime::currentDateTime();
    time = time.toLocalTime();
    if (oldDate != newDate) time.setDate(newDate);
    return time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

QString ledgerTimeOrder(const QString &column)
{
    // SQLite 默认时间是不带时区的本地时间；旧版 ISO 流水可能带 Z/偏移量。
    // 只对显式带时区的记录转换到本地时间，避免同一天的流水被平移八小时。
    return QStringLiteral(
        "strftime('%Y-%m-%d %H:%M:%f',%1,CASE WHEN substr(%1,-1)='Z' "
        "OR instr(substr(%1,20),'+')>0 OR instr(substr(%1,20),'-')>0 "
        "THEN 'localtime' ELSE '+0 seconds' END)").arg(column);
}

QString placeholders(int count)
{
    return QStringList(count, QStringLiteral("?")).join(QLatin1Char(','));
}

QStringList normalizedSerials(const QStringList &values)
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &value : values) {
        const QString serial = value.trimmed().toUpper();
        if (serial.isEmpty() || seen.contains(serial)) continue;
        seen.insert(serial);
        result.append(serial);
    }
    return result;
}

QJsonObject auditSnapshot(const PostedDocumentEdit &document)
{
    QJsonObject value;
    value.insert(QStringLiteral("documentId"), QString::number(document.documentId));
    value.insert(QStringLiteral("documentNumber"), document.documentNumber);
    value.insert(QStringLiteral("documentType"), document.documentType);
    value.insert(QStringLiteral("stockDirection"), document.stockDirection);
    value.insert(QStringLiteral("status"), document.status);
    value.insert(QStringLiteral("documentDate"), document.documentDate.toString(Qt::ISODate));
    value.insert(QStringLiteral("deliveryDate"), document.deliveryDate.toString(Qt::ISODate));
    value.insert(QStringLiteral("sourceDocumentId"), QString::number(document.sourceDocumentId));
    value.insert(QStringLiteral("productionRunId"), QString::number(document.productionRunId));
    value.insert(QStringLiteral("inspectionNoticeId"), QString::number(document.inspectionNoticeId));
    value.insert(QStringLiteral("handlerName"), document.handlerName);
    value.insert(QStringLiteral("purpose"), document.purpose);
    value.insert(QStringLiteral("supplier"), document.supplier);
    value.insert(QStringLiteral("notes"), document.notes);
    value.insert(QStringLiteral("customerCompany"), document.customerCompany);
    value.insert(QStringLiteral("destination"), document.destination);
    value.insert(QStringLiteral("customerContact"), document.customerContact);
    value.insert(QStringLiteral("customerPhone"), document.customerPhone);
    value.insert(QStringLiteral("salesOrderNumber"), document.salesOrderNumber);
    value.insert(QStringLiteral("logisticsCompany"), document.logisticsCompany);
    value.insert(QStringLiteral("trackingNumber"), document.trackingNumber);
    QJsonArray lines;
    for (const PostedDocumentEditLine &line : document.lines) {
        QJsonObject item;
        item.insert(QStringLiteral("itemId"), QString::number(line.itemId));
        item.insert(QStringLiteral("materialId"), QString::number(line.materialId));
        item.insert(QStringLiteral("quantity"), line.quantity);
        item.insert(QStringLiteral("orderedQuantity"), line.orderedQuantity);
        item.insert(QStringLiteral("giftQuantity"), line.giftQuantity);
        item.insert(QStringLiteral("batchNo"), line.batchNo);
        item.insert(QStringLiteral("warehouseId"), QString::number(line.warehouseId));
        item.insert(QStringLiteral("locationId"), QString::number(line.locationId));
        item.insert(QStringLiteral("targetWarehouseId"), QString::number(line.targetWarehouseId));
        item.insert(QStringLiteral("targetLocationId"), QString::number(line.targetLocationId));
        item.insert(QStringLiteral("sourceItemId"), QString::number(line.sourceItemId));
        item.insert(QStringLiteral("movementDirection"), line.movementDirection);
        item.insert(QStringLiteral("serialNumbers"),
                    QJsonArray::fromStringList(normalizedSerials(line.serialNumbers)));
        item.insert(QStringLiteral("notes"), line.notes);
        lines.append(item);
    }
    value.insert(QStringLiteral("lines"), lines);
    return value;
}

bool queryLocation(QSqlDatabase database, qlonglong warehouseId, qlonglong locationId,
                   QString *errorMessage)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM warehouses w JOIN locations l ON l.warehouse_id=w.id "
        "WHERE w.id=? AND l.id=?"));
    query.addBindValue(warehouseId);
    query.addBindValue(locationId);
    if (!query.exec() || !query.next()) {
        setError(errorMessage, QStringLiteral("仓库或库位不存在，或库位不属于所选仓库。"));
        return false;
    }
    return true;
}

struct MaterialRule
{
    bool valid = false;
    bool requireBatch = false;
    bool requireSerial = false;
};

MaterialRule readMaterialRule(QSqlDatabase database, qlonglong materialId)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT require_batch,require_serial FROM materials WHERE id=?"));
    query.addBindValue(materialId);
    MaterialRule rule;
    if (query.exec() && query.next()) {
        rule.valid = true;
        rule.requireBatch = query.value(0).toBool();
        rule.requireSerial = query.value(1).toBool();
    }
    return rule;
}

bool setNoticeLink(QSqlDatabase database, qlonglong documentId, qlonglong oldNoticeId,
                   qlonglong newNoticeId, qlonglong operatorId, QString *errorMessage)
{
    if (oldNoticeId > 0 && oldNoticeId != newNoticeId) {
        QSqlQuery release(database);
        release.prepare(QStringLiteral(
            "UPDATE inspection_notices SET status=inspection_result,linked_document_id=NULL,"
            "updated_by=?,updated_at=? WHERE id=? AND linked_document_id=?"));
        release.addBindValue(operatorId);
        release.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        release.addBindValue(oldNoticeId);
        release.addBindValue(documentId);
        if (!release.exec()) {
            setError(errorMessage, QStringLiteral("解除原送检通知关联失败：%1")
                                       .arg(release.lastError().text()));
            return false;
        }
    }
    if (newNoticeId <= 0) return true;
    QSqlQuery claim(database);
    claim.prepare(QStringLiteral(
        "UPDATE inspection_notices SET status='USED',linked_document_id=?,updated_by=?,updated_at=? "
        "WHERE id=? AND (linked_document_id=? "
        "OR (inspection_result='QUALIFIED' AND linked_document_id IS NULL))"));
    claim.addBindValue(documentId);
    claim.addBindValue(operatorId);
    claim.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    claim.addBindValue(newNoticeId);
    claim.addBindValue(documentId);
    if (!claim.exec() || claim.numRowsAffected() != 1) {
        setError(errorMessage, QStringLiteral("关联的送检通知不存在、尚未合格或已被其他入库单使用。"));
        return false;
    }
    return true;
}

bool rebuildBalances(QSqlDatabase database, QString *errorMessage)
{
    struct Key {
        qlonglong materialId = 0;
        qlonglong warehouseId = 0;
        qlonglong locationId = 0;
        QString batchNo;
    };
    QList<Key> knownKeys;
    QSqlQuery old(database);
    if (!old.exec(QStringLiteral(
            "SELECT material_id,warehouse_id,location_id,batch_no FROM stock_balances"))) {
        setError(errorMessage, QStringLiteral("读取现有库存键失败：%1").arg(old.lastError().text()));
        return false;
    }
    while (old.next()) {
        knownKeys.append({old.value(0).toLongLong(), old.value(1).toLongLong(),
                          old.value(2).toLongLong(), old.value(3).toString()});
    }
    QSqlQuery clear(database);
    if (!clear.exec(QStringLiteral("DELETE FROM stock_balances"))) {
        setError(errorMessage, QStringLiteral("准备重算库存失败：%1").arg(clear.lastError().text()));
        return false;
    }
    QSqlQuery zero(database);
    zero.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO stock_balances(material_id,warehouse_id,location_id,batch_no,quantity) "
        "VALUES(?,?,?,?,0)"));
    for (const Key &key : std::as_const(knownKeys)) {
        zero.bindValue(0, key.materialId);
        zero.bindValue(1, key.warehouseId);
        zero.bindValue(2, key.locationId);
        zero.bindValue(3, normalized(key.batchNo));
        if (!zero.exec()) {
            setError(errorMessage, QStringLiteral("恢复零库存物料失败：%1").arg(zero.lastError().text()));
            return false;
        }
    }

    QMap<QString, double> balances;
    QSqlQuery ledger(database);
    if (!ledger.exec(QStringLiteral(
            "SELECT id,material_id,warehouse_id,location_id,batch_no,quantity_in,quantity_out "
            "FROM inventory_ledger ORDER BY %1,id")
                         .arg(ledgerTimeOrder(QStringLiteral("occurred_at"))))) {
        setError(errorMessage, QStringLiteral("读取库存流水失败：%1").arg(ledger.lastError().text()));
        return false;
    }
    QSqlQuery updateLedger(database);
    updateLedger.prepare(QStringLiteral(
        "UPDATE inventory_ledger SET quantity_before=?,quantity_after=? WHERE id=?"));
    while (ledger.next()) {
        const QString key = QStringLiteral("%1\x1f%2\x1f%3\x1f%4")
                                .arg(ledger.value(1).toLongLong())
                                .arg(ledger.value(2).toLongLong())
                                .arg(ledger.value(3).toLongLong())
                                .arg(ledger.value(4).toString());
        const double before = balances.value(key, 0.0);
        double after = before + ledger.value(5).toDouble() - ledger.value(6).toDouble();
        if (after < -QuantityTolerance) {
            setError(errorMessage,
                     QStringLiteral("修改后的历史流水会导致负库存（流水ID %1，差额 %2）。")
                         .arg(ledger.value(0).toLongLong()).arg(after, 0, 'g', 12));
            return false;
        }
        if (std::abs(after) < QuantityTolerance) after = 0.0;
        balances.insert(key, after);
        updateLedger.bindValue(0, before);
        updateLedger.bindValue(1, after);
        updateLedger.bindValue(2, ledger.value(0));
        if (!updateLedger.exec()) {
            setError(errorMessage, QStringLiteral("回写库存流水余额失败：%1")
                                       .arg(updateLedger.lastError().text()));
            return false;
        }
    }

    QSqlQuery aggregate(database);
    if (!aggregate.exec(QStringLiteral(
            "SELECT material_id,warehouse_id,location_id,batch_no,"
            "SUM(quantity_in-quantity_out) FROM inventory_ledger "
            "GROUP BY material_id,warehouse_id,location_id,batch_no"))) {
        setError(errorMessage, QStringLiteral("汇总库存失败：%1").arg(aggregate.lastError().text()));
        return false;
    }
    QSqlQuery upsert(database);
    upsert.prepare(QStringLiteral(
        "INSERT INTO stock_balances(material_id,warehouse_id,location_id,batch_no,quantity,updated_at) "
        "VALUES(?,?,?,?,?,?) ON CONFLICT(material_id,warehouse_id,location_id,batch_no) "
        "DO UPDATE SET quantity=excluded.quantity,updated_at=excluded.updated_at"));
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    while (aggregate.next()) {
        double quantity = aggregate.value(4).toDouble();
        if (quantity < -QuantityTolerance) {
            setError(errorMessage, QStringLiteral("修改后的库存汇总出现负数。"));
            return false;
        }
        if (std::abs(quantity) < QuantityTolerance) quantity = 0.0;
        for (int index = 0; index < 4; ++index) upsert.bindValue(index, aggregate.value(index));
        upsert.bindValue(4, quantity);
        upsert.bindValue(5, now);
        if (!upsert.exec()) {
            setError(errorMessage, QStringLiteral("保存重算库存失败：%1").arg(upsert.lastError().text()));
            return false;
        }
    }
    return true;
}

bool rebuildSerialStates(QSqlDatabase database,
                         const QList<qlonglong> &affectedSerialIds,
                         QString *errorMessage)
{
    if (affectedSerialIds.isEmpty()) return true;
    QSqlQuery reset(database);
    reset.prepare(QStringLiteral(
        "UPDATE serial_numbers SET status='VOIDED',warehouse_id=NULL,location_id=NULL,"
        "last_document_id=NULL,outbound_at=NULL WHERE id IN (%1)")
                      .arg(placeholders(affectedSerialIds.size())));
    for (qlonglong id : affectedSerialIds) reset.addBindValue(id);
    if (!reset.exec()) {
        setError(errorMessage, QStringLiteral("准备重算SN失败：%1").arg(reset.lastError().text()));
        return false;
    }
    struct State {
        bool inStock = false;
        qlonglong materialId = 0;
        qlonglong warehouseId = 0;
        qlonglong locationId = 0;
        QString batchNo;
        QString status = QStringLiteral("VOIDED");
        qlonglong lastDocumentId = 0;
        QString inboundAt;
        QString outboundAt;
    };
    QHash<qlonglong, State> states;
    QSqlQuery event(database);
    event.prepare(QStringLiteral(
            "SELECT sn.id,sn.serial_no,sn.material_id,l.material_id,l.batch_no,l.warehouse_id,"
            "l.location_id,l.quantity_in,l.quantity_out,l.document_id,l.business_type,"
            "COALESCE(sd.document_type,''),COALESCE(sd.stock_direction,''),l.occurred_at "
            "FROM inventory_ledger_serials x JOIN serial_numbers sn ON sn.id=x.serial_id "
            "JOIN inventory_ledger l ON l.id=x.ledger_id "
            "JOIN business_documents d ON d.id=l.document_id "
            "LEFT JOIN business_documents sd ON sd.id=d.source_document_id "
            "WHERE sn.id IN (%1) ORDER BY %2,l.id")
                      .arg(placeholders(affectedSerialIds.size()), ledgerTimeOrder(QStringLiteral("l.occurred_at"))));
    for (qlonglong id : affectedSerialIds) event.addBindValue(id);
    if (!event.exec()) {
        setError(errorMessage, QStringLiteral("读取SN流水失败：%1").arg(event.lastError().text()));
        return false;
    }
    while (event.next()) {
        const qlonglong serialId = event.value(0).toLongLong();
        const QString serialNo = event.value(1).toString();
        const qlonglong serialMaterialId = event.value(2).toLongLong();
        const qlonglong ledgerMaterialId = event.value(3).toLongLong();
        if (serialMaterialId != ledgerMaterialId) {
            setError(errorMessage, QStringLiteral("SN %1 的物料与库存流水不一致。").arg(serialNo));
            return false;
        }
        State state = states.value(serialId);
        if (event.value(7).toDouble() > QuantityTolerance) {
            if (state.inStock) {
                setError(errorMessage, QStringLiteral("SN %1 在修改后的流水中被重复入库。").arg(serialNo));
                return false;
            }
            state.inStock = true;
            state.status = QStringLiteral("IN_STOCK");
            state.materialId = ledgerMaterialId;
            state.batchNo = event.value(4).toString();
            state.warehouseId = event.value(5).toLongLong();
            state.locationId = event.value(6).toLongLong();
            state.inboundAt = event.value(13).toString();
            state.outboundAt.clear();
        } else {
            if (!state.inStock || state.materialId != ledgerMaterialId
                || state.warehouseId != event.value(5).toLongLong()
                || state.locationId != event.value(6).toLongLong()
                || state.batchNo != event.value(4).toString()) {
                setError(errorMessage,
                         QStringLiteral("SN %1 在修改后的流水中没有对应库位的可用入库记录。")
                             .arg(serialNo));
                return false;
            }
            state.inStock = false;
            const bool voided = event.value(10).toString() == QStringLiteral("CX")
                && event.value(11).toString() != QStringLiteral("SCTL")
                && event.value(12).toString() == QStringLiteral("IN");
            state.status = voided ? QStringLiteral("VOIDED") : QStringLiteral("OUTBOUND");
            state.warehouseId = 0;
            state.locationId = 0;
            state.outboundAt = event.value(13).toString();
        }
        state.lastDocumentId = event.value(9).toLongLong();
        states.insert(serialId, state);
    }

    QSqlQuery update(database);
    update.prepare(QStringLiteral(
        "UPDATE serial_numbers SET status=?,batch_no=?,warehouse_id=?,location_id=?,"
        "last_document_id=?,inbound_at=?,outbound_at=? WHERE id=?"));
    for (auto it = states.cbegin(); it != states.cend(); ++it) {
        const State &state = it.value();
        update.bindValue(0, state.status);
        update.bindValue(1, normalized(state.batchNo));
        update.bindValue(2, state.inStock ? QVariant(state.warehouseId) : QVariant());
        update.bindValue(3, state.inStock ? QVariant(state.locationId) : QVariant());
        update.bindValue(4, state.lastDocumentId > 0 ? QVariant(state.lastDocumentId) : QVariant());
        update.bindValue(5, state.inboundAt.isEmpty() ? QVariant() : QVariant(state.inboundAt));
        update.bindValue(6, state.status == QStringLiteral("OUTBOUND")
                                ? QVariant(state.outboundAt) : QVariant());
        update.bindValue(7, it.key());
        if (!update.exec()) {
            setError(errorMessage, QStringLiteral("保存SN重算结果失败：%1").arg(update.lastError().text()));
            return false;
        }
    }
    return true;
}
}

bool InventoryService::loadPostedDocument(qlonglong documentId,
                                          PostedDocumentEdit *document,
                                          QString *errorMessage) const
{
    if (!document || documentId <= 0 || !m_database.isOpen()) {
        setError(errorMessage, QStringLiteral("业务单据参数无效。"));
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT d.id,d.document_no,d.document_type,d.stock_direction,d.status,d.document_date,"
        "d.source_document_id,d.production_run_id,d.handler_name,d.purpose,d.supplier,d.notes,"
        "COALESCE(s.delivery_date,''),COALESCE(s.customer_company,''),COALESCE(s.destination,''),"
        "COALESCE(s.contact_name,''),COALESCE(s.contact_phone,''),COALESCE(s.sales_order_no,''),"
        "COALESCE(s.logistics_company,''),COALESCE(s.tracking_no,''),"
        "COALESCE(q.inspection_notice_id,0) "
        "FROM business_documents d LEFT JOIN sales_outbound_details s ON s.document_id=d.id "
        "LEFT JOIN inbound_inspection_details q ON q.document_id=d.id WHERE d.id=?"));
    query.addBindValue(documentId);
    if (!query.exec() || !query.next()) {
        setError(errorMessage, QStringLiteral("读取业务单据失败：%1").arg(query.lastError().text()));
        return false;
    }
    PostedDocumentEdit result;
    result.documentId = query.value(0).toLongLong();
    result.documentNumber = query.value(1).toString();
    result.documentType = query.value(2).toString();
    result.stockDirection = query.value(3).toString();
    result.status = query.value(4).toString();
    result.documentDate = QDate::fromString(query.value(5).toString(), Qt::ISODate);
    result.sourceDocumentId = query.value(6).toLongLong();
    result.productionRunId = query.value(7).toLongLong();
    result.handlerName = query.value(8).toString();
    result.purpose = query.value(9).toString();
    result.supplier = query.value(10).toString();
    result.notes = query.value(11).toString();
    result.deliveryDate = QDate::fromString(query.value(12).toString(), Qt::ISODate);
    result.customerCompany = query.value(13).toString();
    result.destination = query.value(14).toString();
    result.customerContact = query.value(15).toString();
    result.customerPhone = query.value(16).toString();
    result.salesOrderNumber = query.value(17).toString();
    result.logisticsCompany = query.value(18).toString();
    result.trackingNumber = query.value(19).toString();
    result.inspectionNoticeId = query.value(20).toLongLong();

    QSqlQuery lines(m_database);
    lines.prepare(QStringLiteral(
        "SELECT i.id,i.material_id,i.quantity,i.ordered_quantity,i.gift_quantity,i.batch_no,"
        "i.warehouse_id,i.location_id,i.target_warehouse_id,i.target_location_id,i.source_item_id,i.notes,"
        "CASE WHEN SUM(l.quantity_in)>SUM(l.quantity_out) THEN 'IN' "
        "WHEN SUM(l.quantity_out)>SUM(l.quantity_in) THEN 'OUT' ELSE d.stock_direction END "
        "FROM business_document_items i JOIN business_documents d ON d.id=i.document_id "
        "LEFT JOIN inventory_ledger l ON l.document_item_id=i.id WHERE i.document_id=? "
        "GROUP BY i.id ORDER BY i.line_number,i.id"));
    lines.addBindValue(documentId);
    if (!lines.exec()) {
        setError(errorMessage, QStringLiteral("读取业务明细失败：%1").arg(lines.lastError().text()));
        return false;
    }
    while (lines.next()) {
        PostedDocumentEditLine line;
        line.itemId = lines.value(0).toLongLong();
        line.materialId = lines.value(1).toLongLong();
        line.quantity = lines.value(2).toDouble();
        line.orderedQuantity = lines.value(3).toDouble();
        line.giftQuantity = lines.value(4).toDouble();
        line.batchNo = lines.value(5).toString();
        line.warehouseId = lines.value(6).toLongLong();
        line.locationId = lines.value(7).toLongLong();
        line.targetWarehouseId = lines.value(8).toLongLong();
        line.targetLocationId = lines.value(9).toLongLong();
        line.sourceItemId = lines.value(10).toLongLong();
        line.notes = lines.value(11).toString();
        line.movementDirection = lines.value(12).toString();
        QSqlQuery serials(m_database);
        serials.prepare(QStringLiteral(
            "SELECT DISTINCT sn.serial_no FROM inventory_ledger l "
            "JOIN inventory_ledger_serials x ON x.ledger_id=l.id "
            "JOIN serial_numbers sn ON sn.id=x.serial_id WHERE l.document_item_id=? "
            "ORDER BY sn.serial_no COLLATE NOCASE"));
        serials.addBindValue(line.itemId);
        if (!serials.exec()) {
            setError(errorMessage, QStringLiteral("读取业务明细SN失败：%1").arg(serials.lastError().text()));
            return false;
        }
        while (serials.next()) line.serialNumbers.append(serials.value(0).toString());
        result.lines.append(line);
    }
    *document = result;
    return true;
}

bool InventoryService::revisePostedDocument(const PostedDocumentEdit &document,
                                            QString *errorMessage)
{
    static const QSet<QString> directions = {
        QStringLiteral("IN"), QStringLiteral("OUT"), QStringLiteral("TRANSFER"),
        QStringLiteral("ADJUST")};
    static const QSet<QString> statuses = {
        QStringLiteral("DRAFT"), QStringLiteral("POSTED"),
        QStringLiteral("PARTIALLY_REVERSED"), QStringLiteral("REVERSED")};
    const QString direction = document.stockDirection.trimmed().toUpper();
    const QString status = document.status.trimmed().toUpper();
    if (document.documentId <= 0 || document.documentNumber.trimmed().isEmpty()
        || document.documentType.trimmed().isEmpty() || !directions.contains(direction)
        || !statuses.contains(status) || !document.documentDate.isValid()
        || document.lines.isEmpty() || m_operatorId <= 0 || !m_database.isOpen()) {
        setError(errorMessage, QStringLiteral("单号、类型、日期、状态、库存方向或明细不完整。"));
        return false;
    }

    QSet<QString> documentSerials;
    for (int index = 0; index < document.lines.size(); ++index) {
        const PostedDocumentEditLine &line = document.lines.at(index);
        if (line.materialId <= 0 || !std::isfinite(line.quantity)
            || line.quantity <= QuantityTolerance || !std::isfinite(line.orderedQuantity)
            || line.orderedQuantity < 0 || !std::isfinite(line.giftQuantity)
            || line.giftQuantity < 0 || line.giftQuantity > line.quantity + QuantityTolerance) {
            setError(errorMessage, QStringLiteral("第 %1 行物料或数量无效。").arg(index + 1));
            return false;
        }
        const MaterialRule rule = readMaterialRule(m_database, line.materialId);
        if (!rule.valid) {
            setError(errorMessage, QStringLiteral("第 %1 行物料不存在。").arg(index + 1));
            return false;
        }
        if (rule.requireBatch && line.batchNo.trimmed().isEmpty()) {
            setError(errorMessage, QStringLiteral("第 %1 行物料必须填写批次。").arg(index + 1));
            return false;
        }
        if (!queryLocation(m_database, line.warehouseId, line.locationId, errorMessage)) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 行：%2").arg(index + 1).arg(*errorMessage);
            return false;
        }
        if (direction == QStringLiteral("TRANSFER")
            && !queryLocation(m_database, line.targetWarehouseId, line.targetLocationId, errorMessage)) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 行目标库位：%2").arg(index + 1).arg(*errorMessage);
            return false;
        }
        const QStringList serials = normalizedSerials(line.serialNumbers);
        if (rule.requireSerial
            && (std::abs(line.quantity - std::round(line.quantity)) > QuantityTolerance
                || serials.size() != static_cast<int>(std::round(line.quantity)))) {
            setError(errorMessage, QStringLiteral("第 %1 行SN数量必须与整数业务数量一致。").arg(index + 1));
            return false;
        }
        if (!rule.requireSerial && !serials.isEmpty()) {
            setError(errorMessage, QStringLiteral("第 %1 行物料不启用SN管理，不能填写SN。").arg(index + 1));
            return false;
        }
        for (const QString &serial : serials) {
            if (documentSerials.contains(serial)) {
                setError(errorMessage, QStringLiteral("SN %1 在本单据中重复。").arg(serial));
                return false;
            }
            documentSerials.insert(serial);
        }
    }

    PostedDocumentEdit before;
    if (!loadPostedDocument(document.documentId, &before, errorMessage)
        || !beginImmediate(errorMessage)) return false;
    const auto fail = [this](QString *target, const QString &message) {
        setError(target, message);
        rollback();
        return false;
    };

    QSqlQuery exists(m_database);
    exists.prepare(QStringLiteral("SELECT 1 FROM business_documents WHERE id=?"));
    exists.addBindValue(document.documentId);
    if (!exists.exec() || !exists.next())
        return fail(errorMessage, QStringLiteral("业务单据已不存在，请刷新后重试。"));

    QString occurredAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    QSqlQuery firstLedger(m_database);
    firstLedger.prepare(QStringLiteral(
        "SELECT occurred_at FROM inventory_ledger WHERE document_id=? "
        "ORDER BY %1,id LIMIT 1").arg(ledgerTimeOrder(QStringLiteral("occurred_at"))));
    firstLedger.addBindValue(document.documentId);
    if (!firstLedger.exec())
        return fail(errorMessage, QStringLiteral("读取原库存流水时间失败：%1")
                                      .arg(firstLedger.lastError().text()));
    if (firstLedger.next() && !firstLedger.value(0).toString().isEmpty())
        occurredAt = firstLedger.value(0).toString();
    occurredAt = revisedLedgerTime(occurredAt, before.documentDate, document.documentDate);

    QHash<qlonglong, PostedDocumentEditLine> originalItems;
    QHash<qlonglong, PostedDocumentEditLine> retainedItems;
    for (const auto &line : before.lines) originalItems.insert(line.itemId, line);
    for (const auto &line : document.lines) {
        if (line.itemId <= 0) continue;
        if (!originalItems.contains(line.itemId) || retainedItems.contains(line.itemId))
            return fail(errorMessage, QStringLiteral("明细编号已变化或重复，请重新打开单据。"));
        retainedItems.insert(line.itemId, line);
    }

    struct DependentItemLink {
        qlonglong dependentItemId = 0;
        qlonglong sourceItemId = 0;
        QString documentType;
        double quantity = 0.0;
        double giftQuantity = 0.0;
    };
    QList<DependentItemLink> dependentLinks;
    QSqlQuery dependents(m_database);
    dependents.prepare(QStringLiteral(
        "SELECT child.id,source.id,d.document_type,"
        "CASE WHEN d.status='DRAFT' THEN 0 WHEN d.document_type='SCTL' "
        "THEN child.quantity-child.reversed_quantity ELSE child.quantity END,"
        "CASE WHEN d.status='DRAFT' THEN 0 ELSE child.gift_quantity END "
        "FROM business_document_items child "
        "JOIN business_document_items source ON source.id=child.source_item_id "
        "JOIN business_documents d ON d.id=child.document_id "
        "WHERE source.document_id=?"));
    dependents.addBindValue(document.documentId);
    if (!dependents.exec())
        return fail(errorMessage, QStringLiteral("读取原单据的撤销/退料关联失败：%1")
                                      .arg(dependents.lastError().text()));
    while (dependents.next())
        dependentLinks.append({dependents.value(0).toLongLong(), dependents.value(1).toLongLong(),
                               dependents.value(2).toString(), dependents.value(3).toDouble(),
                               dependents.value(4).toDouble()});

    QHash<qlonglong, double> linkedQuantities;
    QHash<qlonglong, double> linkedGifts;
    for (const auto &link : std::as_const(dependentLinks)) {
        if (!retainedItems.contains(link.sourceItemId))
            return fail(errorMessage, QStringLiteral("物料 %1 的明细已关联退料/撤销单，不能删除后丢失来源；请先调整对应关联单据。")
                                          .arg(originalItems.value(link.sourceItemId).materialId));
        const auto &line = retainedItems[link.sourceItemId];
        const auto &original = originalItems[link.sourceItemId];
        if (line.materialId != original.materialId || line.batchNo.trimmed() != original.batchNo.trimmed())
            return fail(errorMessage, QStringLiteral("已有退料/撤销关联的明细不能改成另一物料或批次；请先调整对应关联单据。"));
        if (link.documentType == QStringLiteral("CX") || link.documentType == QStringLiteral("SCTL"))
            linkedQuantities[link.sourceItemId] += link.quantity;
        if (link.documentType == QStringLiteral("CX")) linkedGifts[link.sourceItemId] += link.giftQuantity;
    }
    for (auto it = linkedQuantities.cbegin(); it != linkedQuantities.cend(); ++it) {
        const auto &line = retainedItems[it.key()];
        if (it.value() > line.quantity + QuantityTolerance
            || linkedGifts.value(it.key()) > line.giftQuantity + QuantityTolerance)
            return fail(errorMessage, QStringLiteral("修改数量小于该明细已撤销及实际已退数量，请先调整对应关联单据。"));
    }

    QList<qlonglong> oldLedgerIds;
    struct OriginalLedger { qlonglong id; QString time; };
    QHash<qlonglong, QList<OriginalLedger>> originalLedgers;
    QList<qlonglong> affectedSerialIds;
    QSqlQuery ledgerIds(m_database);
    ledgerIds.prepare(QStringLiteral("SELECT id,document_item_id,occurred_at FROM inventory_ledger WHERE document_id=? ORDER BY id"));
    ledgerIds.addBindValue(document.documentId);
    if (!ledgerIds.exec())
        return fail(errorMessage, QStringLiteral("读取原库存流水失败：%1")
                                      .arg(ledgerIds.lastError().text()));
    while (ledgerIds.next()) {
        const qlonglong id = ledgerIds.value(0).toLongLong();
        oldLedgerIds.append(id);
        originalLedgers[ledgerIds.value(1).toLongLong()].append({id, ledgerIds.value(2).toString()});
    }
    QSqlQuery oldSerials(m_database);
    oldSerials.prepare(QStringLiteral(
        "SELECT DISTINCT x.serial_id FROM inventory_ledger_serials x "
        "JOIN inventory_ledger l ON l.id=x.ledger_id WHERE l.document_id=?"));
    oldSerials.addBindValue(document.documentId);
    if (!oldSerials.exec())
        return fail(errorMessage, QStringLiteral("读取原单据SN失败：%1")
                                      .arg(oldSerials.lastError().text()));
    while (oldSerials.next()) affectedSerialIds.append(oldSerials.value(0).toLongLong());
    QMap<qlonglong, qlonglong> reversalLinks;
    QSqlQuery readReversalLinks(m_database);
    if (!readReversalLinks.exec(QStringLiteral(
            "SELECT id,reversal_of_ledger_id FROM inventory_ledger WHERE reversal_of_ledger_id IS NOT NULL")))
        return fail(errorMessage, QStringLiteral("读取撤销流水关联失败：%1").arg(readReversalLinks.lastError().text()));
    while (readReversalLinks.next())
        reversalLinks.insert(readReversalLinks.value(0).toLongLong(), readReversalLinks.value(1).toLongLong());
    if (!oldLedgerIds.isEmpty()) {
        QSqlQuery detach(m_database);
        detach.prepare(QStringLiteral("UPDATE inventory_ledger SET reversal_of_ledger_id=NULL WHERE reversal_of_ledger_id IN (%1)")
                           .arg(placeholders(oldLedgerIds.size())));
        for (qlonglong id : std::as_const(oldLedgerIds)) detach.addBindValue(id);
        if (!detach.exec())
            return fail(errorMessage, QStringLiteral("解除原流水撤销关联失败：%1")
                                          .arg(detach.lastError().text()));
        QSqlQuery links(m_database);
        links.prepare(QStringLiteral("DELETE FROM inventory_ledger_serials WHERE ledger_id IN (%1)")
                          .arg(placeholders(oldLedgerIds.size())));
        for (qlonglong id : std::as_const(oldLedgerIds)) links.addBindValue(id);
        if (!links.exec())
            return fail(errorMessage, QStringLiteral("清理原SN流水关联失败：%1")
                                          .arg(links.lastError().text()));
    }
    QSqlQuery detachItems(m_database);
    detachItems.prepare(QStringLiteral(
        "UPDATE business_document_items SET source_item_id=NULL WHERE source_item_id IN "
        "(SELECT id FROM business_document_items WHERE document_id=?)"));
    detachItems.addBindValue(document.documentId);
    if (!detachItems.exec())
        return fail(errorMessage, QStringLiteral("解除原明细引用失败：%1")
                                      .arg(detachItems.lastError().text()));
    QSqlQuery removeLedger(m_database);
    removeLedger.prepare(QStringLiteral("DELETE FROM inventory_ledger WHERE document_id=?"));
    removeLedger.addBindValue(document.documentId);
    QSqlQuery removeItems(m_database);
    removeItems.prepare(QStringLiteral("DELETE FROM business_document_items WHERE document_id=?"));
    removeItems.addBindValue(document.documentId);
    if (!removeLedger.exec() || !removeItems.exec())
        return fail(errorMessage, QStringLiteral("清理原单据明细失败：%1 / %2")
                                      .arg(removeLedger.lastError().text(), removeItems.lastError().text()));

    QSqlQuery updateDocument(m_database);
    updateDocument.prepare(QStringLiteral(
        "UPDATE business_documents SET document_no=?,document_type=?,stock_direction=?,"
        "document_date=?,status=?,source_document_id=?,production_run_id=?,handler_name=?,"
        "purpose=?,supplier=?,notes=?,posted_at=CASE WHEN ?='DRAFT' THEN NULL "
        "ELSE COALESCE(posted_at,?) END,updated_at=? WHERE id=?"));
    updateDocument.addBindValue(normalized(document.documentNumber));
    updateDocument.addBindValue(normalized(document.documentType).toUpper());
    updateDocument.addBindValue(direction);
    updateDocument.addBindValue(document.documentDate.toString(Qt::ISODate));
    updateDocument.addBindValue(status);
    updateDocument.addBindValue(document.sourceDocumentId > 0 ? QVariant(document.sourceDocumentId) : QVariant());
    updateDocument.addBindValue(document.productionRunId > 0 ? QVariant(document.productionRunId) : QVariant());
    updateDocument.addBindValue(normalized(document.handlerName));
    updateDocument.addBindValue(normalized(document.purpose));
    updateDocument.addBindValue(normalized(document.supplier));
    updateDocument.addBindValue(normalized(document.notes));
    const QString revisionTime = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    updateDocument.addBindValue(status);
    updateDocument.addBindValue(revisionTime);
    updateDocument.addBindValue(revisionTime);
    updateDocument.addBindValue(document.documentId);
    if (!updateDocument.exec() || updateDocument.numRowsAffected() != 1)
        return fail(errorMessage, QStringLiteral("保存单头失败：%1")
                                      .arg(updateDocument.lastError().text()));

    QSqlQuery removeSales(m_database);
    removeSales.prepare(QStringLiteral("DELETE FROM sales_outbound_details WHERE document_id=?"));
    removeSales.addBindValue(document.documentId);
    if (!removeSales.exec())
        return fail(errorMessage, QStringLiteral("清理原销售资料失败：%1")
                                      .arg(removeSales.lastError().text()));
    const bool hasSales = document.documentType.trimmed().toUpper() == QStringLiteral("XSCK")
        || !document.customerCompany.trimmed().isEmpty() || !document.destination.trimmed().isEmpty()
        || !document.customerContact.trimmed().isEmpty() || !document.customerPhone.trimmed().isEmpty()
        || !document.salesOrderNumber.trimmed().isEmpty() || !document.logisticsCompany.trimmed().isEmpty()
        || !document.trackingNumber.trimmed().isEmpty();
    if (hasSales) {
        QSqlQuery sales(m_database);
        sales.prepare(QStringLiteral(
            "INSERT INTO sales_outbound_details(document_id,customer_company,destination,contact_name,"
            "contact_phone,sales_order_no,logistics_company,tracking_no,delivery_date) "
            "VALUES(?,?,?,?,?,?,?,?,?)"));
        sales.addBindValue(document.documentId);
        sales.addBindValue(normalized(document.customerCompany));
        sales.addBindValue(normalized(document.destination));
        sales.addBindValue(normalized(document.customerContact));
        sales.addBindValue(normalized(document.customerPhone));
        sales.addBindValue(normalized(document.salesOrderNumber));
        sales.addBindValue(normalized(document.logisticsCompany));
        sales.addBindValue(normalized(document.trackingNumber));
        sales.addBindValue((document.deliveryDate.isValid() ? document.deliveryDate
                                                             : document.documentDate).toString(Qt::ISODate));
        if (!sales.exec())
            return fail(errorMessage, QStringLiteral("保存销售资料失败：%1")
                                          .arg(sales.lastError().text()));
    }

    qlonglong oldNoticeId = before.inspectionNoticeId;
    if (!setNoticeLink(m_database, document.documentId, oldNoticeId,
                       direction == QStringLiteral("IN") ? document.inspectionNoticeId : 0,
                       m_operatorId, errorMessage)) {
        rollback();
        return false;
    }
    const bool preserveLegacyInspection = direction == QStringLiteral("IN")
        && before.stockDirection == QStringLiteral("IN")
        && before.inspectionNoticeId <= 0 && document.inspectionNoticeId <= 0;
    QSqlQuery removeInspection(m_database);
    removeInspection.prepare(QStringLiteral("DELETE FROM inbound_inspection_details WHERE document_id=?"));
    removeInspection.addBindValue(document.documentId);
    if (!preserveLegacyInspection && !removeInspection.exec())
        return fail(errorMessage, QStringLiteral("清理原入库检验资料失败：%1")
                                      .arg(removeInspection.lastError().text()));
    if (direction == QStringLiteral("IN")) {
        QSqlQuery inspection(m_database);
        if (document.inspectionNoticeId > 0) {
            inspection.prepare(QStringLiteral(
                "INSERT INTO inbound_inspection_details(document_id,requires_inspection,inspection_no,"
                "inspection_date,inspector_name,inspection_result,conclusion,inspection_attachment_id,"
                "inspection_notice_id) SELECT ?,1,inspection_no,inspection_date,inspector_name,"
                "inspection_result,conclusion,inspection_attachment_id,id FROM inspection_notices WHERE id=?"));
            inspection.addBindValue(document.documentId);
            inspection.addBindValue(document.inspectionNoticeId);
        } else {
            inspection.prepare(QStringLiteral(
                "INSERT INTO inbound_inspection_details(document_id,requires_inspection,inspection_result) "
                "VALUES(?,0,'NOT_REQUIRED') ON CONFLICT(document_id) DO NOTHING"));
            inspection.addBindValue(document.documentId);
        }
        if (!inspection.exec() || (!preserveLegacyInspection && inspection.numRowsAffected() != 1))
            return fail(errorMessage, QStringLiteral("保存入库检验关联失败：%1")
                                          .arg(inspection.lastError().text()));
    }

    QSqlQuery insertItem(m_database);
    insertItem.prepare(QStringLiteral(
        "INSERT INTO business_document_items(document_id,line_number,material_id,quantity,"
        "ordered_quantity,gift_quantity,batch_no,warehouse_id,location_id,target_warehouse_id,"
        "target_location_id,source_item_id,notes,id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    QSqlQuery insertLedger(m_database);
    insertLedger.prepare(QStringLiteral(
        "INSERT INTO inventory_ledger(occurred_at,business_type,document_id,document_item_id,"
        "material_id,batch_no,quantity_in,quantity_out,quantity_before,quantity_after,"
        "warehouse_id,location_id,operator_id,notes,id) VALUES(?,?,?,?,?,?,?,?,0,0,?,?,?,?,?)"));
    QSqlQuery findSerial(m_database);
    findSerial.prepare(QStringLiteral("SELECT id,material_id FROM serial_numbers WHERE serial_no=?"));
    QSqlQuery createSerial(m_database);
    createSerial.prepare(QStringLiteral(
        "INSERT INTO serial_numbers(material_id,serial_no,batch_no,status,notes) "
        "VALUES(?,?,?,'VOIDED','由单据修订创建')"));
    QSqlQuery linkSerial(m_database);
    linkSerial.prepare(QStringLiteral(
        "INSERT INTO inventory_ledger_serials(ledger_id,serial_id) VALUES(?,?)"));
    for (int index = 0; index < document.lines.size(); ++index) {
        const PostedDocumentEditLine &line = document.lines.at(index);
        insertItem.bindValue(0, document.documentId);
        insertItem.bindValue(1, index + 1);
        insertItem.bindValue(2, line.materialId);
        insertItem.bindValue(3, line.quantity);
        insertItem.bindValue(4, line.orderedQuantity);
        insertItem.bindValue(5, line.giftQuantity);
        insertItem.bindValue(6, normalized(line.batchNo));
        insertItem.bindValue(7, line.warehouseId);
        insertItem.bindValue(8, line.locationId);
        insertItem.bindValue(9, line.targetWarehouseId > 0 ? QVariant(line.targetWarehouseId) : QVariant());
        insertItem.bindValue(10, line.targetLocationId > 0 ? QVariant(line.targetLocationId) : QVariant());
        insertItem.bindValue(11, line.sourceItemId > 0 ? QVariant(line.sourceItemId) : QVariant());
        insertItem.bindValue(12, normalized(line.notes));
        insertItem.bindValue(13, line.itemId > 0 ? QVariant(line.itemId) : QVariant());
        if (!insertItem.exec())
            return fail(errorMessage, QStringLiteral("保存第 %1 行明细失败：%2")
                                          .arg(index + 1).arg(insertItem.lastError().text()));
        const qlonglong itemId = insertItem.lastInsertId().toLongLong();

        QList<qlonglong> ledgerIdsForLine;
        const QString lineDirection = direction == QStringLiteral("ADJUST")
            ? line.movementDirection.trimmed().toUpper() : direction;
        const auto addLedger = [&](const QString &businessType, double quantityIn,
                                   double quantityOut, qlonglong warehouseId,
                                   qlonglong locationId) -> bool {
            const auto original = originalLedgers.value(line.itemId);
            const int position = ledgerIdsForLine.size();
            const bool reuse = position < original.size();
            insertLedger.bindValue(0, reuse
                ? revisedLedgerTime(original.at(position).time, before.documentDate, document.documentDate)
                : occurredAt);
            insertLedger.bindValue(1, businessType);
            insertLedger.bindValue(2, document.documentId);
            insertLedger.bindValue(3, itemId);
            insertLedger.bindValue(4, line.materialId);
            insertLedger.bindValue(5, normalized(line.batchNo));
            insertLedger.bindValue(6, quantityIn);
            insertLedger.bindValue(7, quantityOut);
            insertLedger.bindValue(8, warehouseId);
            insertLedger.bindValue(9, locationId);
            insertLedger.bindValue(10, m_operatorId);
            insertLedger.bindValue(11, normalized(line.notes));
            insertLedger.bindValue(12, reuse ? QVariant(original.at(position).id) : QVariant());
            if (!insertLedger.exec()) return false;
            ledgerIdsForLine.append(insertLedger.lastInsertId().toLongLong());
            return true;
        };
        if (status != QStringLiteral("DRAFT")) {
            if (lineDirection == QStringLiteral("TRANSFER")) {
                if (!addLedger(QStringLiteral("DB-OUT"), 0.0, line.quantity,
                               line.warehouseId, line.locationId)
                    || !addLedger(QStringLiteral("DB-IN"), line.quantity, 0.0,
                                  line.targetWarehouseId, line.targetLocationId)) {
                    return fail(errorMessage, QStringLiteral("生成第 %1 行调拨流水失败：%2")
                                              .arg(index + 1).arg(insertLedger.lastError().text()));
                }
            } else if (lineDirection == QStringLiteral("IN")) {
                if (!addLedger(document.documentType.trimmed().toUpper(), line.quantity, 0.0,
                               line.warehouseId, line.locationId))
                    return fail(errorMessage, QStringLiteral("生成第 %1 行入库流水失败：%2")
                                              .arg(index + 1).arg(insertLedger.lastError().text()));
            } else if (lineDirection == QStringLiteral("OUT")) {
                if (!addLedger(document.documentType.trimmed().toUpper(), 0.0, line.quantity,
                               line.warehouseId, line.locationId))
                    return fail(errorMessage, QStringLiteral("生成第 %1 行出库流水失败：%2")
                                              .arg(index + 1).arg(insertLedger.lastError().text()));
            } else {
                return fail(errorMessage, QStringLiteral("第 %1 行库存效果必须为入库或出库。")
                                          .arg(index + 1));
            }
        }

        const QStringList serials = normalizedSerials(line.serialNumbers);
        for (const QString &serial : serials) {
            findSerial.bindValue(0, serial);
            if (!findSerial.exec())
                return fail(errorMessage, QStringLiteral("检查SN %1失败：%2")
                                          .arg(serial, findSerial.lastError().text()));
            qlonglong serialId = 0;
            if (findSerial.next()) {
                if (findSerial.value(1).toLongLong() != line.materialId)
                    return fail(errorMessage, QStringLiteral("SN %1 已属于其他物料。").arg(serial));
                serialId = findSerial.value(0).toLongLong();
            } else {
                createSerial.bindValue(0, line.materialId);
                createSerial.bindValue(1, serial);
                createSerial.bindValue(2, normalized(line.batchNo));
                if (!createSerial.exec())
                    return fail(errorMessage, QStringLiteral("创建SN %1失败：%2")
                                              .arg(serial, createSerial.lastError().text()));
                serialId = createSerial.lastInsertId().toLongLong();
            }
            if (!affectedSerialIds.contains(serialId)) affectedSerialIds.append(serialId);
            for (qlonglong ledgerId : std::as_const(ledgerIdsForLine)) {
                linkSerial.bindValue(0, ledgerId);
                linkSerial.bindValue(1, serialId);
                if (!linkSerial.exec())
                    return fail(errorMessage, QStringLiteral("关联SN %1失败：%2")
                                              .arg(serial, linkSerial.lastError().text()));
            }
        }
        if (status != QStringLiteral("DRAFT")
            && (lineDirection == QStringLiteral("IN")
                || lineDirection == QStringLiteral("TRANSFER"))
            && !line.batchNo.trimmed().isEmpty()) {
            QSqlQuery batch(m_database);
            batch.prepare(QStringLiteral(
                "INSERT INTO batches(material_id,batch_no,supplier,first_in_at) VALUES(?,?,?,?) "
                "ON CONFLICT(material_id,batch_no) DO UPDATE SET supplier="
                "CASE WHEN excluded.supplier<>'' THEN excluded.supplier ELSE batches.supplier END"));
            batch.addBindValue(line.materialId);
            batch.addBindValue(normalized(line.batchNo));
            batch.addBindValue(normalized(document.supplier));
            batch.addBindValue(document.documentDate.toString(Qt::ISODate));
            if (!batch.exec())
                return fail(errorMessage, QStringLiteral("更新批次资料失败：%1")
                                          .arg(batch.lastError().text()));
        }
    }

    QSqlQuery restoreDependent(m_database);
    restoreDependent.prepare(QStringLiteral(
        "UPDATE business_document_items SET source_item_id=? WHERE id=?"));
    for (const DependentItemLink &link : std::as_const(dependentLinks)) {
        restoreDependent.bindValue(0, link.sourceItemId);
        restoreDependent.bindValue(1, link.dependentItemId);
        if (!restoreDependent.exec())
            return fail(errorMessage, QStringLiteral("恢复撤销/退料关联失败：%1")
                                          .arg(restoreDependent.lastError().text()));
    }
    QSqlQuery restoreLedgerLink(m_database);
    restoreLedgerLink.prepare(QStringLiteral(
        "UPDATE inventory_ledger SET reversal_of_ledger_id=? WHERE id=? "
        "AND EXISTS(SELECT 1 FROM inventory_ledger WHERE id=?)"));
    for (auto it = reversalLinks.cbegin(); it != reversalLinks.cend(); ++it) {
        restoreLedgerLink.bindValue(0, it.value());
        restoreLedgerLink.bindValue(1, it.key());
        restoreLedgerLink.bindValue(2, it.value());
        if (!restoreLedgerLink.exec())
            return fail(errorMessage, QStringLiteral("恢复撤销流水关联失败：%1").arg(restoreLedgerLink.lastError().text()));
    }
    // 调拨撤销有两条反向流水。原调拨流水被整单重建后，需要按业务方向重新关联，
    // 否则后续查询会把已经撤销过的调拨当成未撤销。
    QSqlQuery restoreTransferReversal(m_database);
    restoreTransferReversal.prepare(QStringLiteral(
        "UPDATE inventory_ledger SET reversal_of_ledger_id=("
        "SELECT source_ledger.id FROM business_document_items child_item "
        "JOIN inventory_ledger source_ledger ON source_ledger.document_item_id=child_item.source_item_id "
        "WHERE child_item.id=inventory_ledger.document_item_id "
        "AND source_ledger.business_type=CASE inventory_ledger.business_type "
        "WHEN 'CX-DB-OUT' THEN 'DB-IN' ELSE 'DB-OUT' END LIMIT 1) "
        "WHERE business_type IN ('CX-DB-OUT','CX-DB-IN') "
        "AND EXISTS(SELECT 1 FROM business_document_items child_item "
        "WHERE child_item.id=inventory_ledger.document_item_id "
        "AND child_item.source_item_id IS NOT NULL)"));
    if (!restoreTransferReversal.exec())
        return fail(errorMessage, QStringLiteral("恢复调拨撤销流水关联失败：%1")
                                      .arg(restoreTransferReversal.lastError().text()));
    // 先算撤销，再按净退料量回算。分两步更新，保证退料单自身的撤销已经生效，
    // 同时涵盖“修改退料的撤销单”时需要更新的上两级领料来源。
    QSqlQuery refreshRelatedQuantities(m_database);
    if (!refreshRelatedQuantities.exec(QStringLiteral(
            "UPDATE business_document_items AS source SET "
            "reversed_quantity=COALESCE((SELECT SUM(child.quantity) "
            "FROM business_document_items child JOIN business_documents d ON d.id=child.document_id "
            "WHERE child.source_item_id=source.id AND d.document_type='CX' AND d.status<>'DRAFT'),0),"
            "reversed_gift_quantity=COALESCE((SELECT SUM(child.gift_quantity) "
            "FROM business_document_items child JOIN business_documents d ON d.id=child.document_id "
            "WHERE child.source_item_id=source.id AND d.document_type='CX' AND d.status<>'DRAFT'),0)"))
        || !refreshRelatedQuantities.exec(QStringLiteral(
            "UPDATE business_document_items AS source SET "
            "returned_quantity=COALESCE((SELECT SUM(child.quantity-child.reversed_quantity) "
            "FROM business_document_items child JOIN business_documents d ON d.id=child.document_id "
            "WHERE child.source_item_id=source.id AND d.document_type='SCTL' AND d.status<>'DRAFT'),0)"))) {
        return fail(errorMessage, QStringLiteral("重算撤销/退料数量失败：%1")
                                      .arg(refreshRelatedQuantities.lastError().text()));
    }

    if (!rebuildBalances(m_database, errorMessage)
        || !rebuildSerialStates(m_database, affectedSerialIds, errorMessage)) {
        rollback();
        return false;
    }
    QSqlQuery refreshBatchDates(m_database);
    if (!refreshBatchDates.exec(QStringLiteral(
            "UPDATE batches SET first_in_at=(SELECT l.occurred_at "
            "FROM inventory_ledger l WHERE l.material_id=batches.material_id "
            "AND l.batch_no=batches.batch_no AND l.quantity_in>0 ORDER BY %1,l.id LIMIT 1)")
                                              .arg(ledgerTimeOrder(QStringLiteral("l.occurred_at"))))) {
        return fail(errorMessage, QStringLiteral("重算批次首次入库日期失败：%1")
                                      .arg(refreshBatchDates.lastError().text()));
    }
    if (before.productionRunId > 0
        && !refreshProductionRunStatus(before.productionRunId, errorMessage)) {
        rollback();
        return false;
    }
    if (document.productionRunId > 0 && document.productionRunId != before.productionRunId
        && !refreshProductionRunStatus(document.productionRunId, errorMessage)) {
        rollback();
        return false;
    }

    QJsonObject detail;
    detail.insert(QStringLiteral("before"), auditSnapshot(before));
    detail.insert(QStringLiteral("after"), auditSnapshot(document));
    QSqlQuery audit(m_database);
    audit.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
        "VALUES(?,'BUSINESS_DOCUMENT_FULL_REVISE','business_document',?,?)"));
    audit.addBindValue(m_operatorId);
    audit.addBindValue(document.documentId);
    audit.addBindValue(QString::fromUtf8(
        QJsonDocument(detail).toJson(QJsonDocument::Compact)));
    if (!audit.exec())
        return fail(errorMessage, QStringLiteral("记录整单修改日志失败：%1")
                                      .arg(audit.lastError().text()));
    if (!commit(errorMessage)) {
        rollback();
        return false;
    }
    return true;
}
