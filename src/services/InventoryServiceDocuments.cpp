#include "services/InventoryService.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QMap>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <cmath>

namespace {
constexpr double DocumentQuantityTolerance = 0.0000001;
constexpr qint64 MaximumInspectionAttachmentBytes = 50LL * 1024LL * 1024LL;

void setDocumentError(QString *target, const QString &message)
{
    if (target) *target = message;
}

QString normalizedText(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isNull() ? QString::fromLatin1("", 0) : trimmed;
}

QString lineError(int lineNumber, const QString &message)
{
    return QStringLiteral("第 %1 行：%2").arg(lineNumber).arg(message);
}

bool isValidInspectionNumber(const QString &number, const QDate &date)
{
    const QString prefix = QStringLiteral("BMJ-JY-%1-")
                               .arg(date.toString(QStringLiteral("yyyyMMdd")));
    if (!number.startsWith(prefix) || number.size() != prefix.size() + 3) return false;
    bool ok = false;
    const int sequence = number.right(3).toInt(&ok);
    return ok && sequence >= 1 && sequence <= 999
        && number.right(3) == QStringLiteral("%1").arg(sequence, 3, 10, QLatin1Char('0'));
}

struct InspectionNoticeSnapshot
{
    QString inspectionNumber;
    QDate inspectionDate;
    QString inspectorName;
    QString conclusion;
    qlonglong attachmentId = 0;
};

QString inspectionLineKey(qlonglong materialId, const QString &batchNumber)
{
    Q_UNUSED(batchNumber);
    return QString::number(materialId);
}

bool loadAndValidateInspectionNotice(QSqlDatabase database,
                                     qlonglong noticeId,
                                     const QList<StockMovementRequest> &requestLines,
                                     InspectionNoticeSnapshot *snapshot,
                                     QString *errorMessage)
{
    QSqlQuery notice(database);
    notice.prepare(QStringLiteral(
        "SELECT inspection_no,status,inspection_date,inspector_name,inspection_result,"
        "conclusion,inspection_attachment_id,linked_document_id "
        "FROM inspection_notices WHERE id=?"));
    notice.addBindValue(noticeId);
    if (!notice.exec() || !notice.next()) {
        setDocumentError(errorMessage, QStringLiteral("选择的送检通知不存在或读取失败：%1")
                                           .arg(notice.lastError().text()));
        return false;
    }
    if (notice.value(1).toString() != QStringLiteral("QUALIFIED")
        || notice.value(4).toString() != QStringLiteral("QUALIFIED")
        || !notice.value(7).isNull()) {
        setDocumentError(errorMessage, QStringLiteral("选择的送检通知尚未合格或已经用于其他入库单。"));
        return false;
    }
    const qlonglong attachmentId = notice.value(6).toLongLong();
    QSqlQuery attachment(database);
    attachment.prepare(QStringLiteral(
        "SELECT 1 FROM attachments WHERE id=? AND business_type='inspection_notice' "
        "AND business_id=? AND is_deleted=0 AND file_size>0"));
    attachment.addBindValue(attachmentId);
    attachment.addBindValue(noticeId);
    if (attachmentId <= 0 || !attachment.exec() || !attachment.next()) {
        setDocumentError(errorMessage, QStringLiteral("选择的合格送检通知缺少有效检验附件。"));
        return false;
    }

    QMap<QString, double> noticeQuantities;
    QSqlQuery items(database);
    items.prepare(QStringLiteral(
        "SELECT material_id,batch_no,quantity FROM inspection_notice_items "
        "WHERE notice_id=? ORDER BY line_number"));
    items.addBindValue(noticeId);
    if (!items.exec()) {
        setDocumentError(errorMessage, QStringLiteral("读取送检通知明细失败：%1")
                                           .arg(items.lastError().text()));
        return false;
    }
    while (items.next()) {
        const QString key = inspectionLineKey(items.value(0).toLongLong(),
                                              items.value(1).toString());
        noticeQuantities[key] += items.value(2).toDouble();
    }
    QMap<QString, double> inboundQuantities;
    for (const StockMovementRequest &line : requestLines) {
        if (line.materialId <= 0 || !std::isfinite(line.quantity)
            || line.quantity <= DocumentQuantityTolerance) {
            setDocumentError(errorMessage, QStringLiteral("入库明细包含无效物料或数量。"));
            return false;
        }
        inboundQuantities[inspectionLineKey(line.materialId, line.batchNo)] += line.quantity;
    }
    if (noticeQuantities.size() != inboundQuantities.size()) {
        setDocumentError(errorMessage, QStringLiteral("入库物料与所选送检通知不一致。"));
        return false;
    }
    for (auto it = noticeQuantities.cbegin(); it != noticeQuantities.cend(); ++it) {
        const auto inbound = inboundQuantities.constFind(it.key());
        if (inbound == inboundQuantities.cend()
            || std::abs(inbound.value() - it.value()) > DocumentQuantityTolerance) {
            setDocumentError(errorMessage, QStringLiteral("入库物料或数量与所选送检通知不一致。"));
            return false;
        }
    }
    if (snapshot) {
        snapshot->inspectionNumber = notice.value(0).toString();
        snapshot->inspectionDate = QDate::fromString(notice.value(2).toString(), Qt::ISODate);
        snapshot->inspectorName = notice.value(3).toString();
        snapshot->conclusion = notice.value(5).toString();
        snapshot->attachmentId = attachmentId;
    }
    return true;
}
}

bool InventoryService::setDocumentSubmissionToken(qlonglong documentId,
                                                   const QString &submissionToken,
                                                   QString *errorMessage)
{
    const QString token = submissionToken.trimmed();
    if (documentId <= 0 || token.isEmpty()) {
        setDocumentError(errorMessage, QStringLiteral("单据提交标识无效。"));
        return false;
    }
    QSqlQuery duplicate(m_database);
    duplicate.prepare(QStringLiteral("SELECT 1 FROM business_documents WHERE submission_token=?"));
    duplicate.addBindValue(token);
    if (!duplicate.exec()) {
        setDocumentError(errorMessage, duplicate.lastError().text());
        return false;
    }
    if (duplicate.next()) {
        setDocumentError(errorMessage, QStringLiteral("该单据已经提交，请勿重复操作。"));
        return false;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE business_documents SET submission_token=? WHERE id=?"));
    update.addBindValue(token);
    update.addBindValue(documentId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        const QString detail = update.lastError().text();
        setDocumentError(errorMessage,
                         detail.contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive)
                             ? QStringLiteral("该单据已经提交，请勿重复操作。")
                             : QStringLiteral("保存提交标识失败：%1").arg(detail));
        return false;
    }
    return true;
}

bool InventoryService::postStockDocument(const StockDocumentRequest &request,
                                         bool inbound,
                                         PostedDocument *postedDocument,
                                         QString *errorMessage)
{
    if (!m_database.isOpen() || m_operatorId <= 0 || !request.documentDate.isValid()
        || request.documentType.trimmed().isEmpty() || request.lines.isEmpty()
        || request.submissionToken.trimmed().isEmpty()) {
        setDocumentError(errorMessage, QStringLiteral("出入库单头、明细或当前用户无效。"));
        return false;
    }
    const QString type = request.documentType.trimmed().toUpper();
    if ((!inbound && request.inspectionNoticeId > 0)
        || (request.inspectionNoticeId > 0 && request.inspection.required)) {
        setDocumentError(errorMessage,
                         request.inspectionNoticeId > 0 && request.inspection.required
                             ? QStringLiteral("不能同时使用独立送检通知和旧版内嵌送检资料。")
                             : QStringLiteral("只有入库单可以关联送检通知。"));
        return false;
    }
    if (!inbound && type == QStringLiteral("XSCK")) {
        // 发货资料由服务层独立校验，避免绕过界面写入不完整的销售出库单。
        QStringList missing;
        if (request.customerCompany.trimmed().isEmpty())
            missing.append(QStringLiteral("客户公司名称"));
        if (request.destination.trimmed().isEmpty())
            missing.append(QStringLiteral("销售目的地/收货地址"));
        if (request.customerContact.trimmed().isEmpty())
            missing.append(QStringLiteral("客户联系人"));
        if (request.customerPhone.trimmed().isEmpty())
            missing.append(QStringLiteral("联系电话"));
        if (request.salesOrderNumber.trimmed().isEmpty())
            missing.append(QStringLiteral("客户合同号/订单号"));
        if (request.logisticsCompany.trimmed().isEmpty())
            missing.append(QStringLiteral("物流/快递公司"));
        if (request.trackingNumber.trimmed().isEmpty())
            missing.append(QStringLiteral("运单号"));
        if (!request.deliveryDate.isValid())
            missing.append(QStringLiteral("送货日期"));
        if (!missing.isEmpty()) {
            setDocumentError(errorMessage,
                             QStringLiteral("销售出库必须完整填写以下发货资料：%1。")
                                 .arg(missing.join(QStringLiteral("、"))));
            return false;
        }
    }
    if (inbound && request.inspectionNoticeId <= 0 && request.inspection.required) {
        if (request.inspection.inspectionNumber.trimmed().isEmpty()
            || !request.inspection.inspectionDate.isValid()
            || request.inspection.inspectorName.trimmed().isEmpty()) {
            setDocumentError(errorMessage, QStringLiteral("送检单号、送检日期和检验员不能为空。"));
            return false;
        }
        if (!isValidInspectionNumber(request.inspection.inspectionNumber.trimmed(),
                                     request.inspection.inspectionDate)) {
            setDocumentError(errorMessage,
                             QStringLiteral("送检单号必须符合 BMJ-JY-年月日-三位流水号，并与送检日期一致。"));
            return false;
        }
        if (request.inspection.result.trimmed().toUpper() != QStringLiteral("QUALIFIED")) {
            setDocumentError(errorMessage, QStringLiteral("只有检验结果为合格的送检单才能入库。"));
            return false;
        }
        if (request.inspection.attachmentFileName.trimmed().isEmpty()
            || request.inspection.attachmentData.isEmpty()) {
            setDocumentError(errorMessage, QStringLiteral("检验合格后必须上传检验附件才能入库。"));
            return false;
        }
        if (request.inspection.attachmentData.size() > MaximumInspectionAttachmentBytes) {
            setDocumentError(errorMessage, QStringLiteral("送检附件不能超过50 MB。"));
            return false;
        }
    }
    if (!beginImmediate(errorMessage)) return false;

    InspectionNoticeSnapshot inspectionNotice;
    if (inbound && request.inspectionNoticeId > 0
        && !loadAndValidateInspectionNotice(m_database, request.inspectionNoticeId,
                                            request.lines, &inspectionNotice, errorMessage)) {
        rollback();
        return false;
    }

    const QString number = nextDocumentNumber(type, request.documentDate, errorMessage);
    const qlonglong documentId = number.isEmpty()
        ? 0
        : createDocument(number, type, inbound ? QStringLiteral("IN") : QStringLiteral("OUT"),
                         request.documentDate, request.handlerName, request.purpose,
                         request.notes, QVariant(), errorMessage);
    if (documentId <= 0
        || !setDocumentSubmissionToken(documentId, request.submissionToken, errorMessage)) {
        rollback();
        return false;
    }
    if (inbound && type == QStringLiteral("CGRK")) {
        QSqlQuery supplier(m_database);
        supplier.prepare(QStringLiteral("UPDATE business_documents SET supplier=? WHERE id=?"));
        supplier.addBindValue(normalizedText(request.supplier));
        supplier.addBindValue(documentId);
        if (!supplier.exec()) {
            setDocumentError(errorMessage,
                             QStringLiteral("保存供应商失败：%1").arg(supplier.lastError().text()));
            rollback();
            return false;
        }
    }
    if (inbound) {
        QVariant inspectionAttachmentId;
        if (request.inspectionNoticeId > 0) {
            inspectionAttachmentId = inspectionNotice.attachmentId;
        } else if (request.inspection.required) {
            QSqlQuery attachment(m_database);
            attachment.prepare(QStringLiteral(
                "INSERT INTO attachments(business_type,business_id,original_file_name,mime_type,"
                "file_size,sha256,file_data,uploaded_by) "
                "VALUES('business_document',?,?,?,?,?,?,?)"));
            attachment.addBindValue(documentId);
            attachment.addBindValue(normalizedText(request.inspection.attachmentFileName));
            attachment.addBindValue(normalizedText(request.inspection.attachmentMimeType));
            attachment.addBindValue(request.inspection.attachmentData.size());
            attachment.addBindValue(QString::fromLatin1(QCryptographicHash::hash(
                request.inspection.attachmentData, QCryptographicHash::Sha256).toHex()));
            attachment.addBindValue(request.inspection.attachmentData);
            attachment.addBindValue(m_operatorId);
            if (!attachment.exec()) {
                setDocumentError(errorMessage,
                                 QStringLiteral("保存送检附件失败：%1")
                                     .arg(attachment.lastError().text()));
                rollback();
                return false;
            }
            inspectionAttachmentId = attachment.lastInsertId();
        }

        QSqlQuery inspection(m_database);
        inspection.prepare(QStringLiteral(
            "INSERT INTO inbound_inspection_details(document_id,inspection_notice_id,requires_inspection,inspection_no,"
            "inspection_date,inspector_name,inspection_result,conclusion,inspection_attachment_id) "
            "VALUES(?,?,?,?,?,?,?,?,?)"));
        inspection.addBindValue(documentId);
        inspection.addBindValue(request.inspectionNoticeId > 0
                                    ? QVariant(request.inspectionNoticeId) : QVariant());
        const bool inspected = request.inspectionNoticeId > 0 || request.inspection.required;
        inspection.addBindValue(inspected ? 1 : 0);
        inspection.addBindValue(request.inspectionNoticeId > 0
                                    ? normalizedText(inspectionNotice.inspectionNumber)
                                    : (request.inspection.required
                                           ? normalizedText(request.inspection.inspectionNumber)
                                           : QString()));
        inspection.addBindValue(request.inspectionNoticeId > 0
                                    ? inspectionNotice.inspectionDate.toString(Qt::ISODate)
                                    : (request.inspection.required
                                           ? QVariant(request.inspection.inspectionDate.toString(Qt::ISODate))
                                           : QVariant()));
        inspection.addBindValue(request.inspectionNoticeId > 0
                                    ? normalizedText(inspectionNotice.inspectorName)
                                    : (request.inspection.required
                                           ? normalizedText(request.inspection.inspectorName)
                                           : QString()));
        inspection.addBindValue(inspected ? QStringLiteral("QUALIFIED")
                                          : QStringLiteral("NOT_REQUIRED"));
        inspection.addBindValue(request.inspectionNoticeId > 0
                                    ? normalizedText(inspectionNotice.conclusion)
                                    : (request.inspection.required
                                           ? normalizedText(request.inspection.conclusion)
                                           : QString()));
        inspection.addBindValue(inspectionAttachmentId);
        if (!inspection.exec()) {
            const QString detail = inspection.lastError().text();
            setDocumentError(
                errorMessage,
                detail.contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive)
                    ? QStringLiteral("送检单号已被占用，请重新打开送检单，"
                                     "系统会自动生成新的当日流水号。")
                    : QStringLiteral("保存入库送检资料失败：%1").arg(detail));
            rollback();
            return false;
        }
    }
    if (!inbound && type == QStringLiteral("XSCK")) {
        QSqlQuery sales(m_database);
        sales.prepare(QStringLiteral(
            "INSERT INTO sales_outbound_details(document_id,customer_company,destination,"
            "contact_name,contact_phone,sales_order_no,logistics_company,tracking_no,"
            "delivery_date) VALUES(?,?,?,?,?,?,?,?,?)"));
        sales.addBindValue(documentId);
        sales.addBindValue(normalizedText(request.customerCompany));
        sales.addBindValue(normalizedText(request.destination));
        sales.addBindValue(normalizedText(request.customerContact));
        sales.addBindValue(normalizedText(request.customerPhone));
        sales.addBindValue(normalizedText(request.salesOrderNumber));
        sales.addBindValue(normalizedText(request.logisticsCompany));
        sales.addBindValue(normalizedText(request.trackingNumber));
        sales.addBindValue(request.deliveryDate.toString(Qt::ISODate));
        if (!sales.exec()) {
            setDocumentError(errorMessage,
                             QStringLiteral("保存销售出库信息失败：%1")
                                 .arg(sales.lastError().text()));
            rollback();
            return false;
        }
    }

    QSet<QString> identities;
    for (int index = 0; index < request.lines.size(); ++index) {
        StockMovementRequest movement = request.lines.at(index);
        movement.documentType = type;
        movement.documentDate = request.documentDate;
        const QString identity = QStringLiteral("%1|%2|%3|%4")
                                     .arg(movement.materialId)
                                     .arg(movement.warehouseId)
                                     .arg(movement.locationId)
                                     .arg(movement.batchNo.trimmed().toUpper());
        if (identities.contains(identity)) {
            setDocumentError(errorMessage,
                             lineError(index + 1, QStringLiteral("物料、库位和批次组合重复。")));
            rollback();
            return false;
        }
        identities.insert(identity);

        if (inbound && type == QStringLiteral("CGRK")) {
            if (movement.orderedQuantity < -DocumentQuantityTolerance
                || movement.giftQuantity < -DocumentQuantityTolerance
                || movement.giftQuantity > qMax(0.0, movement.quantity - movement.orderedQuantity)
                                               + DocumentQuantityTolerance) {
                setDocumentError(errorMessage, lineError(index + 1,
                    QStringLiteral("采购数量或赠送数量无效，赠送数量不能超过多到货数量。")));
                rollback();
                return false;
            }
        } else if (std::abs(movement.orderedQuantity) > DocumentQuantityTolerance
                   || std::abs(movement.giftQuantity) > DocumentQuantityTolerance) {
            setDocumentError(errorMessage, lineError(index + 1,
                QStringLiteral("只有采购入库可以填写采购数量和赠送数量。")));
            rollback();
            return false;
        }

        QString detail;
        const MaterialRules rules = materialRules(movement.materialId, &detail);
        if (!rules.valid || !validateMovement(movement, rules, &detail)
            || !validateLocation(movement.warehouseId, movement.locationId, &detail)) {
            setDocumentError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
        const qlonglong itemId = createDocumentItem(documentId, index + 1, movement,
                                                     QVariant(), &detail);
        double before = 0.0;
        double after = 0.0;
        const double delta = inbound ? movement.quantity : -movement.quantity;
        if (itemId <= 0
            || !changeBalance(movement.materialId, movement.warehouseId, movement.locationId,
                              movement.batchNo.trimmed(), delta, &before, &after, &detail)) {
            setDocumentError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
        const qlonglong ledgerId = createLedger(documentId, itemId, type, movement.materialId,
                                                 movement.batchNo.trimmed(),
                                                 inbound ? movement.quantity : 0.0,
                                                 inbound ? 0.0 : movement.quantity,
                                                 before, after, movement.warehouseId,
                                                 movement.locationId, movement.notes, &detail);
        if (ledgerId <= 0) {
            setDocumentError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
        if (inbound && !movement.batchNo.trimmed().isEmpty()) {
            QSqlQuery batch(m_database);
            batch.prepare(QStringLiteral(
                "INSERT INTO batches(material_id,batch_no,supplier,first_in_at) VALUES(?,?,?,?) "
                "ON CONFLICT(material_id,batch_no) DO UPDATE SET supplier="
                "CASE WHEN excluded.supplier<>'' THEN excluded.supplier ELSE batches.supplier END"));
            batch.addBindValue(movement.materialId);
            batch.addBindValue(normalizedText(movement.batchNo));
            batch.addBindValue(normalizedText(movement.supplier.trimmed().isEmpty()
                                                  ? request.supplier : movement.supplier));
            batch.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
            if (!batch.exec()) {
                setDocumentError(errorMessage, lineError(index + 1, batch.lastError().text()));
                rollback();
                return false;
            }
        }
        const bool serialsOk = inbound
            ? attachSerialsToInbound(movement, documentId, ledgerId, &detail)
            : attachSerialsToOutbound(movement, documentId, ledgerId, &detail);
        if (!serialsOk) {
            setDocumentError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
    }

    if (inbound && request.inspectionNoticeId > 0) {
        const QString now = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        QSqlQuery useNotice(m_database);
        useNotice.prepare(QStringLiteral(
            "UPDATE inspection_notices SET status='USED',linked_document_id=?,updated_by=?,"
            "updated_at=? WHERE id=? AND status='QUALIFIED' AND linked_document_id IS NULL"));
        useNotice.addBindValue(documentId);
        useNotice.addBindValue(m_operatorId);
        useNotice.addBindValue(now);
        useNotice.addBindValue(request.inspectionNoticeId);
        if (!useNotice.exec() || useNotice.numRowsAffected() != 1) {
            setDocumentError(errorMessage,
                             QStringLiteral("送检通知已被其他入库单使用，请刷新后重试。"));
            rollback();
            return false;
        }
        QSqlQuery audit(m_database);
        audit.prepare(QStringLiteral(
            "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
            "VALUES(?,'INSPECTION_NOTICE_USE','inspection_notice',?,?)"));
        audit.addBindValue(m_operatorId);
        audit.addBindValue(request.inspectionNoticeId);
        audit.addBindValue(number);
        if (!audit.exec()) {
            setDocumentError(errorMessage, QStringLiteral("记录送检通知入库日志失败：%1")
                                               .arg(audit.lastError().text()));
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

bool InventoryService::importInitialInventory(const InitialInventoryRequest &request,
                                              PostedDocument *postedDocument,
                                              QString *errorMessage)
{
    return importInitialInventoryInternal(request, postedDocument, true, errorMessage);
}

bool InventoryService::importInitialInventoryInCurrentTransaction(
    const InitialInventoryRequest &request,
    PostedDocument *postedDocument,
    QString *errorMessage)
{
    return importInitialInventoryInternal(request, postedDocument, false, errorMessage);
}

bool InventoryService::importInitialInventoryInternal(const InitialInventoryRequest &request,
                                                      PostedDocument *postedDocument,
                                                      bool manageTransaction,
                                                      QString *errorMessage)
{
    if (!m_database.isOpen() || m_operatorId <= 0 || !request.documentDate.isValid()
        || request.warehouseId <= 0 || request.locationId <= 0 || request.lines.isEmpty()
        || request.submissionToken.trimmed().isEmpty()) {
        setDocumentError(errorMessage, QStringLiteral("期初库存导入资料不完整。"));
        return false;
    }
    const auto rollbackIfOwned = [this, manageTransaction]() {
        if (manageTransaction) rollback();
    };
    if (!validateLocation(request.warehouseId, request.locationId, errorMessage)
        || (manageTransaction && !beginImmediate(errorMessage))) return false;

    const QString number = nextDocumentNumber(QStringLiteral("QTRK"), request.documentDate,
                                               errorMessage);
    const QString notes = QStringLiteral("期初库存导入：%1").arg(request.sourceFile);
    const qlonglong documentId = number.isEmpty()
        ? 0
        : createDocument(number, QStringLiteral("QTRK"), QStringLiteral("IN"),
                         request.documentDate, request.handlerName,
                         QStringLiteral("期初库存导入"), notes, QVariant(), errorMessage);
    if (documentId <= 0
        || !setDocumentSubmissionToken(documentId, request.submissionToken, errorMessage)) {
        rollbackIfOwned();
        return false;
    }

    QSet<QString> identities;
    for (int index = 0; index < request.lines.size(); ++index) {
        const InitialInventoryLine &source = request.lines.at(index);
        const QString code = source.materialCode.trimmed().toUpper();
        const QString name = source.materialName.trimmed();
        const QString batchNo = source.batchNo.trimmed();
        // 期初库存允许为0（只建立物料档案），但不允许任何负数，小数点后的极小负值同样拒绝。
        if (code.isEmpty() || name.isEmpty() || source.quantity < 0.0) {
            setDocumentError(errorMessage,
                             lineError(index + 1, QStringLiteral("物料编码、名称无效或数量为负数。")));
            rollbackIfOwned();
            return false;
        }
        const QString identity = code + QLatin1Char('|') + batchNo.toUpper();
        if (identities.contains(identity)) {
            setDocumentError(errorMessage,
                             lineError(index + 1, QStringLiteral("物料编码和批次重复。")));
            rollbackIfOwned();
            return false;
        }
        identities.insert(identity);

        qlonglong materialId = 0;
        QSqlQuery existing(m_database);
        existing.prepare(QStringLiteral("SELECT id,is_active FROM materials WHERE code=?"));
        existing.addBindValue(code);
        if (!existing.exec()) {
            setDocumentError(errorMessage, existing.lastError().text());
            rollbackIfOwned();
            return false;
        }
        if (existing.next()) {
            if (!existing.value(1).toBool()) {
                setDocumentError(errorMessage,
                                 lineError(index + 1, QStringLiteral("已有同编码物料处于停用状态。")));
                rollbackIfOwned();
                return false;
            }
            materialId = existing.value(0).toLongLong();
        } else {
            QSqlQuery category(m_database);
            category.prepare(QStringLiteral(
                "SELECT id FROM material_categories WHERE code=? AND is_active=1"));
            category.addBindValue(source.categoryCode.trimmed().toUpper().isEmpty()
                                      ? QStringLiteral("RAW")
                                      : source.categoryCode.trimmed().toUpper());
            if (!category.exec() || !category.next()) {
                setDocumentError(errorMessage,
                                 lineError(index + 1, QStringLiteral("物料分类不存在或已停用。")));
                rollbackIfOwned();
                return false;
            }
            QSqlQuery insert(m_database);
            insert.prepare(QStringLiteral(
                "INSERT INTO materials(code,name,specification,category_id,unit,"
                "default_warehouse_id,default_location_id,require_batch,require_serial,notes) "
                "VALUES(?,?,?,?,?,?,?,?,0,?)"));
            insert.addBindValue(code);
            insert.addBindValue(name);
            insert.addBindValue(normalizedText(source.specification));
            insert.addBindValue(category.value(0));
            insert.addBindValue(source.unit.trimmed().isEmpty() ? QStringLiteral("个")
                                                                : source.unit.trimmed());
            insert.addBindValue(request.warehouseId);
            insert.addBindValue(request.locationId);
            insert.addBindValue(!batchNo.isEmpty());
            insert.addBindValue(QStringLiteral("由期初库存导入自动创建"));
            if (!insert.exec()) {
                setDocumentError(errorMessage,
                                 lineError(index + 1, QStringLiteral("创建物料失败：%1")
                                                               .arg(insert.lastError().text())));
                rollbackIfOwned();
                return false;
            }
            materialId = insert.lastInsertId().toLongLong();
        }

        // 零库存记录只建立/保留物料档案，不校验出入库规则，也不产生单据明细、库存、流水、SN或批次。
        if (source.quantity <= DocumentQuantityTolerance) continue;

        // 期初库存的权威防重：目标物料+仓库+库位+批次只要已有库存记录（即使数量为0），
        // 就说明该批次已经入过账，必须改用库存盘点或正常出入库业务处理，整单回滚。
        QSqlQuery existingBalance(m_database);
        existingBalance.prepare(QStringLiteral(
            "SELECT s.quantity,w.name,l.name FROM stock_balances s "
            "JOIN warehouses w ON w.id=s.warehouse_id "
            "JOIN locations l ON l.id=s.location_id "
            "WHERE s.material_id=? AND s.warehouse_id=? AND s.location_id=? AND s.batch_no=?"));
        existingBalance.addBindValue(materialId);
        existingBalance.addBindValue(request.warehouseId);
        existingBalance.addBindValue(request.locationId);
        existingBalance.addBindValue(normalizedText(batchNo));
        if (!existingBalance.exec()) {
            setDocumentError(errorMessage, existingBalance.lastError().text());
            rollbackIfOwned();
            return false;
        }
        if (existingBalance.next()) {
            setDocumentError(
                errorMessage,
                lineError(index + 1,
                          QStringLiteral("物料 %1（批次 %2）在仓库 %3 库位 %4 已有库存记录"
                                         "（当前数量 %5），期初库存不能重复导入；"
                                         "请改用库存盘点或正常出入库业务处理。")
                              .arg(code,
                                   batchNo.isEmpty() ? QStringLiteral("无批次") : batchNo,
                                   existingBalance.value(1).toString(),
                                   existingBalance.value(2).toString(),
                                   QString::number(existingBalance.value(0).toDouble(),
                                                   'g', 12))));
            rollbackIfOwned();
            return false;
        }

        StockMovementRequest movement;
        movement.documentType = QStringLiteral("QTRK");
        movement.documentDate = request.documentDate;
        movement.materialId = materialId;
        movement.quantity = source.quantity;
        movement.batchNo = batchNo;
        movement.warehouseId = request.warehouseId;
        movement.locationId = request.locationId;
        movement.serialNumbers = source.serialNumbers;
        movement.notes = source.notes;
        QString detail;
        const MaterialRules rules = materialRules(materialId, &detail);
        if (!rules.valid || !validateMovement(movement, rules, &detail)) {
            setDocumentError(errorMessage, lineError(index + 1, detail));
            rollbackIfOwned();
            return false;
        }
        const qlonglong itemId = createDocumentItem(documentId, index + 1, movement,
                                                     QVariant(), &detail);
        double before = 0.0;
        double after = 0.0;
        if (itemId <= 0
            || !changeBalance(materialId, request.warehouseId, request.locationId, batchNo,
                              source.quantity, &before, &after, &detail)) {
            setDocumentError(errorMessage, lineError(index + 1, detail));
            rollbackIfOwned();
            return false;
        }
        const qlonglong ledgerId = createLedger(
            documentId, itemId, QStringLiteral("QTRK"), materialId, batchNo,
            source.quantity, 0.0, before, after, request.warehouseId,
            request.locationId, source.notes, &detail);
        if (ledgerId <= 0
            || !attachSerialsToInbound(movement, documentId, ledgerId, &detail)) {
            setDocumentError(errorMessage, lineError(index + 1, detail));
            rollbackIfOwned();
            return false;
        }
        if (!batchNo.isEmpty()) {
            QSqlQuery batch(m_database);
            batch.prepare(QStringLiteral(
                "INSERT OR IGNORE INTO batches(material_id,batch_no,first_in_at) VALUES(?,?,?)"));
            batch.addBindValue(materialId);
            batch.addBindValue(batchNo);
            batch.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
            if (!batch.exec()) {
                setDocumentError(errorMessage, lineError(index + 1, batch.lastError().text()));
                rollbackIfOwned();
                return false;
            }
        }
    }

    if (!finalizeDocument(documentId, errorMessage)
        || !writeAudit(QStringLiteral("IMPORT"), QStringLiteral("business_document"),
                       documentId, QStringLiteral("%1；%2 行").arg(number).arg(request.lines.size()),
                       errorMessage)
        || (manageTransaction && !commit(errorMessage))) {
        rollbackIfOwned();
        return false;
    }
    if (postedDocument) {
        postedDocument->documentId = documentId;
        postedDocument->documentNumber = number;
    }
    return true;
}

bool InventoryService::postInventoryCount(const InventoryCountRequest &request,
                                          PostedDocument *postedDocument,
                                          QString *errorMessage)
{
    if (!m_database.isOpen() || m_operatorId <= 0 || !request.documentDate.isValid()
        || request.lines.isEmpty() || request.submissionToken.trimmed().isEmpty()) {
        setDocumentError(errorMessage, QStringLiteral("盘点单头、明细或当前用户无效。"));
        return false;
    }
    if (!beginImmediate(errorMessage)) return false;
    const QString number = nextDocumentNumber(QStringLiteral("PD"), request.documentDate,
                                               errorMessage);
    const qlonglong documentId = number.isEmpty()
        ? 0
        : createDocument(number, QStringLiteral("PD"), QStringLiteral("ADJUST"),
                         request.documentDate, request.handlerName,
                         QStringLiteral("库存盘点"), request.notes, QVariant(), errorMessage);
    if (documentId <= 0
        || !setDocumentSubmissionToken(documentId, request.submissionToken, errorMessage)) {
        rollback();
        return false;
    }
    QSqlQuery count(m_database);
    count.prepare(QStringLiteral(
        "INSERT INTO inventory_counts(document_id,status) VALUES(?,'DRAFT')"));
    count.addBindValue(documentId);
    if (!count.exec()) {
        setDocumentError(errorMessage, count.lastError().text());
        rollback();
        return false;
    }
    const qlonglong countId = count.lastInsertId().toLongLong();
    QSet<QString> identities;
    int documentLine = 0;
    for (int index = 0; index < request.lines.size(); ++index) {
        const InventoryCountLine &line = request.lines.at(index);
        if (line.actualQuantity < -DocumentQuantityTolerance) {
            setDocumentError(errorMessage, lineError(index + 1, QStringLiteral("实盘数量不能小于0。")));
            rollback();
            return false;
        }
        const QString identity = QStringLiteral("%1|%2|%3|%4")
                                     .arg(line.materialId).arg(line.warehouseId)
                                     .arg(line.locationId).arg(line.batchNo.trimmed().toUpper());
        if (identities.contains(identity)) {
            setDocumentError(errorMessage, lineError(index + 1, QStringLiteral("盘点明细重复。")));
            rollback();
            return false;
        }
        identities.insert(identity);
        QString detail;
        const MaterialRules rules = materialRules(line.materialId, &detail);
        if (!rules.valid || !validateLocation(line.warehouseId, line.locationId, &detail)) {
            setDocumentError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
        if (rules.requireBatch && line.batchNo.trimmed().isEmpty()) {
            setDocumentError(errorMessage,
                             lineError(index + 1,
                                       QStringLiteral("该物料启用了批次管理，必须填写批次号。")));
            rollback();
            return false;
        }
        QSqlQuery current(m_database);
        current.prepare(QStringLiteral(
            "SELECT COALESCE(quantity,0) FROM stock_balances WHERE material_id=? "
            "AND warehouse_id=? AND location_id=? AND batch_no=?"));
        current.addBindValue(line.materialId);
        current.addBindValue(line.warehouseId);
        current.addBindValue(line.locationId);
        current.addBindValue(normalizedText(line.batchNo));
        if (!current.exec()) {
            setDocumentError(errorMessage, lineError(index + 1, current.lastError().text()));
            rollback();
            return false;
        }
        const double currentQuantity = current.next() ? current.value(0).toDouble() : 0.0;
        if (std::abs(currentQuantity - line.systemQuantity) > DocumentQuantityTolerance) {
            setDocumentError(errorMessage,
                             lineError(index + 1, QStringLiteral("账面库存已变化，请刷新后重新盘点。")));
            rollback();
            return false;
        }
        const double difference = line.actualQuantity - line.systemQuantity;
        if (rules.requireSerial && std::abs(difference) > DocumentQuantityTolerance) {
            setDocumentError(errorMessage,
                             lineError(index + 1,
                                       QStringLiteral("SN管理物料发生差异时需先通过出入库调整SN。")));
            rollback();
            return false;
        }
        if (std::abs(difference) > DocumentQuantityTolerance
            && line.differenceReason.trimmed().isEmpty()) {
            setDocumentError(errorMessage,
                             lineError(index + 1, QStringLiteral("存在盘点差异，必须填写差异原因。")));
            rollback();
            return false;
        }
        // 批次档案与盘点差异无关：实盘为0也要落库，否则零库存批次的供应商等资料无法维护。
        if (!line.batchNo.trimmed().isEmpty()) {
            QSqlQuery batch(m_database);
            if (line.supplier.isNull()) {
                batch.prepare(QStringLiteral(
                    "INSERT OR IGNORE INTO batches(material_id,batch_no,supplier,first_in_at) "
                    "VALUES(?,?,?,?)"));
            } else {
                batch.prepare(QStringLiteral(
                    "INSERT INTO batches(material_id,batch_no,supplier,first_in_at) VALUES(?,?,?,?) "
                    "ON CONFLICT(material_id,batch_no) DO UPDATE SET supplier=excluded.supplier"));
            }
            batch.addBindValue(line.materialId);
            batch.addBindValue(normalizedText(line.batchNo));
            batch.addBindValue(line.supplier.isNull()
                                   ? QStringLiteral("") : normalizedText(line.supplier));
            // 新建的零库存批次没有实际入库时间，写入 SQL NULL；冲突时保留已有 first_in_at。
            QVariant firstInAt;
            if (line.actualQuantity > DocumentQuantityTolerance) {
                firstInAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
            }
            batch.addBindValue(firstInAt);
            if (!batch.exec()) {
                setDocumentError(errorMessage,
                                 lineError(index + 1,
                                           QStringLiteral("保存批次供应商失败：%1")
                                               .arg(batch.lastError().text())));
                rollback();
                return false;
            }
        }
        QSqlQuery countItem(m_database);
        countItem.prepare(QStringLiteral(
            "INSERT INTO inventory_count_items(inventory_count_id,material_id,warehouse_id,"
            "location_id,batch_no,system_quantity,actual_quantity,difference_quantity,difference_reason) "
            "VALUES(?,?,?,?,?,?,?,?,?)"));
        countItem.addBindValue(countId);
        countItem.addBindValue(line.materialId);
        countItem.addBindValue(line.warehouseId);
        countItem.addBindValue(line.locationId);
        countItem.addBindValue(normalizedText(line.batchNo));
        countItem.addBindValue(line.systemQuantity);
        countItem.addBindValue(line.actualQuantity);
        countItem.addBindValue(difference);
        countItem.addBindValue(normalizedText(line.differenceReason));
        if (!countItem.exec()) {
            setDocumentError(errorMessage, lineError(index + 1, countItem.lastError().text()));
            rollback();
            return false;
        }
        if (std::abs(difference) <= DocumentQuantityTolerance) continue;

        StockMovementRequest movement;
        movement.documentType = QStringLiteral("PD");
        movement.documentDate = request.documentDate;
        movement.materialId = line.materialId;
        movement.quantity = std::abs(difference);
        movement.batchNo = line.batchNo;
        movement.warehouseId = line.warehouseId;
        movement.locationId = line.locationId;
        movement.notes = line.differenceReason;
        const qlonglong itemId = createDocumentItem(documentId, ++documentLine, movement,
                                                     QVariant(), &detail);
        double before = 0.0;
        double after = 0.0;
        if (itemId <= 0
            || !changeBalance(line.materialId, line.warehouseId, line.locationId,
                              line.batchNo, difference, &before, &after, &detail)) {
            setDocumentError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
        const bool gain = difference > 0.0;
        if (createLedger(documentId, itemId,
                         gain ? QStringLiteral("PD-PY") : QStringLiteral("PD-PK"),
                         line.materialId, line.batchNo,
                         gain ? difference : 0.0,
                         gain ? 0.0 : -difference,
                         before, after, line.warehouseId, line.locationId,
                         line.differenceReason, &detail) <= 0) {
            setDocumentError(errorMessage, lineError(index + 1, detail));
            rollback();
            return false;
        }
    }

    QSqlQuery confirm(m_database);
    confirm.prepare(QStringLiteral(
        "UPDATE inventory_counts SET status='CONFIRMED',confirmed_at=? WHERE id=?"));
    confirm.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    confirm.addBindValue(countId);
    if (!confirm.exec() || !finalizeDocument(documentId, errorMessage)
        || !writeAudit(QStringLiteral("COUNT"), QStringLiteral("inventory_count"),
                       countId, number, errorMessage)
        || !commit(errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) *errorMessage = confirm.lastError().text();
        rollback();
        return false;
    }
    if (postedDocument) {
        postedDocument->documentId = documentId;
        postedDocument->documentNumber = number;
    }
    return true;
}
