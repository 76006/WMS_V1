#include "services/InspectionService.h"

#include <QCryptographicHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <cmath>
#include <numeric>
#include <utility>

namespace {
constexpr double QuantityTolerance = 0.0000001;

void setError(QString *target, const QString &message)
{
    if (target) *target = message;
}

QString textValue(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isNull() ? QString::fromLatin1("", 0) : trimmed;
}

bool allowedUrgency(const QString &urgency)
{
    static const QSet<QString> values = {
        QStringLiteral("EXPEDITED"), QStringLiteral("URGENT"), QStringLiteral("NORMAL")};
    return values.contains(urgency.trimmed().toUpper());
}

QDateTime dateTimeValue(const QVariant &value)
{
    const QString text = value.toString();
    QDateTime result = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!result.isValid()) result = QDateTime::fromString(text, Qt::ISODate);
    if (!result.isValid()) {
        result = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    }
    return result;
}

void readNoticeColumns(const QSqlQuery &query, InspectionNotice *notice)
{
    if (!notice) return;
    notice->id = query.value(0).toLongLong();
    notice->inspectionNumber = query.value(1).toString();
    notice->status = query.value(2).toString();
    notice->notificationDate = QDate::fromString(query.value(3).toString(), Qt::ISODate);
    notice->arrivalDate = QDate::fromString(query.value(4).toString(), Qt::ISODate);
    notice->entrustedBy = query.value(5).toString();
    notice->notificationDepartment = query.value(6).toString();
    notice->urgency = query.value(7).toString();
    notice->purchaseOrderNumber = query.value(8).toString();
    notice->supplier = query.value(9).toString();
    notice->inspectorName = query.value(10).toString();
    notice->inspectionDate = QDate::fromString(query.value(11).toString(), Qt::ISODate);
    notice->inspectionResult = query.value(12).toString();
    notice->conclusion = query.value(13).toString();
    notice->inspectionAttachmentId = query.value(14).toLongLong();
    notice->templateFileAttachmentId = query.value(15).toLongLong();
    notice->templatePayload = query.value(16).toString();
    notice->archivePath = query.value(17).toString();
    notice->linkedDocumentId = query.value(18).toLongLong();
    notice->createdBy = query.value(19).toLongLong();
    notice->creatorName = query.value(20).toString();
    notice->createdAt = dateTimeValue(query.value(21));
    notice->updatedAt = dateTimeValue(query.value(22));
    notice->completedAt = dateTimeValue(query.value(23));
    notice->lineCount = query.value(24).toInt();
    notice->totalQuantity = query.value(25).toDouble();
    notice->qualifiedQuantity = query.value(26).toDouble();
    notice->unqualifiedQuantity = query.value(27).toDouble();
    notice->inboundQuantity = query.value(28).toDouble();
}

QString noticeSelectColumns()
{
    return QStringLiteral(
        "n.id,n.inspection_no,n.status,n.notification_date,n.arrival_date,n.entrusted_by,"
        "n.notification_department,n.urgency,n.purchase_order_no,n.supplier,n.inspector_name,"
        "n.inspection_date,n.inspection_result,n.conclusion,n.inspection_attachment_id,"
        "n.template_file_attachment_id,n.template_payload,n.archive_path,n.linked_document_id,"
        "n.created_by,COALESCE(u.display_name,''),n.created_at,n.updated_at,n.completed_at,"
        "(SELECT COUNT(*) FROM inspection_notice_items i WHERE i.notice_id=n.id),"
        "COALESCE((SELECT SUM(i.quantity) FROM inspection_notice_items i WHERE i.notice_id=n.id),0),"
        "COALESCE((SELECT SUM(i.qualified_quantity) FROM inspection_notice_items i WHERE i.notice_id=n.id),0),"
        "COALESCE((SELECT SUM(i.unqualified_quantity) FROM inspection_notice_items i WHERE i.notice_id=n.id),0),"
        "COALESCE((SELECT SUM(i.inbound_quantity) FROM inspection_notice_items i WHERE i.notice_id=n.id),0) ");
}
}

InspectionService::InspectionService(QSqlDatabase database, qlonglong operatorId)
    : m_database(std::move(database)), m_operatorId(operatorId)
{
}

QString InspectionService::previewNextInspectionNumber(const QDate &date,
                                                        QString *errorMessage) const
{
    if (!date.isValid()) {
        setError(errorMessage, QStringLiteral("送检通知日期无效。"));
        return {};
    }
    return nextInspectionNumber(date, errorMessage);
}

bool InspectionService::beginImmediate(QString *errorMessage)
{
    QSqlQuery query(m_database);
    if (query.exec(QStringLiteral("BEGIN IMMEDIATE TRANSACTION"))) return true;
    setError(errorMessage, QStringLiteral("无法开始送检事务：%1").arg(query.lastError().text()));
    return false;
}

bool InspectionService::commit(QString *errorMessage)
{
    QSqlQuery query(m_database);
    if (query.exec(QStringLiteral("COMMIT"))) return true;
    setError(errorMessage, QStringLiteral("无法提交送检事务：%1").arg(query.lastError().text()));
    return false;
}

void InspectionService::rollback()
{
    QSqlQuery(m_database).exec(QStringLiteral("ROLLBACK"));
}

bool InspectionService::validateDraft(const InspectionNoticeDraft &draft,
                                      QString *errorMessage) const
{
    if (!m_database.isOpen() || m_operatorId <= 0) {
        setError(errorMessage, QStringLiteral("数据库或当前操作用户无效。"));
        return false;
    }
    if (!draft.notificationDate.isValid() || !draft.arrivalDate.isValid()
        || draft.entrustedBy.trimmed().isEmpty()
        || draft.notificationDepartment.trimmed().isEmpty()
        || !allowedUrgency(draft.urgency) || draft.lines.isEmpty()) {
        setError(errorMessage, QStringLiteral("送检通知日期、到货日期、委托人员、通知单位、紧急程度或物料明细不完整。"));
        return false;
    }

    QSqlQuery material(m_database);
    material.prepare(QStringLiteral("SELECT 1 FROM materials WHERE id=?"));
    for (int index = 0; index < draft.lines.size(); ++index) {
        const InspectionNoticeLine &line = draft.lines.at(index);
        if (line.materialId <= 0 || !std::isfinite(line.quantity)
            || line.quantity <= QuantityTolerance) {
            setError(errorMessage, QStringLiteral("第 %1 行物料或送检数量无效。").arg(index + 1));
            return false;
        }
        material.bindValue(0, line.materialId);
        if (!material.exec() || !material.next()) {
            setError(errorMessage, QStringLiteral("第 %1 行物料不存在。").arg(index + 1));
            return false;
        }
        if (line.batchNumber.trimmed().isEmpty()) {
            setError(errorMessage, QStringLiteral("第 %1 行批号不能为空。").arg(index + 1));
            return false;
        }
    }
    return true;
}

QString InspectionService::nextInspectionNumber(const QDate &date, QString *errorMessage) const
{
    const QString prefix = QStringLiteral("BMJ-JY-%1-")
                               .arg(date.toString(QStringLiteral("yyyyMMdd")));
    const QRegularExpression pattern(
        QStringLiteral("^%1(\\d{3})$").arg(QRegularExpression::escape(prefix)),
        QRegularExpression::CaseInsensitiveOption);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT inspection_no FROM inspection_notices WHERE inspection_no LIKE ? "
        "UNION ALL SELECT inspection_no FROM inbound_inspection_details WHERE inspection_no LIKE ?"));
    query.addBindValue(prefix + QLatin1Char('%'));
    query.addBindValue(prefix + QLatin1Char('%'));
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("读取送检单流水号失败：%1").arg(query.lastError().text()));
        return {};
    }
    int maximum = 0;
    while (query.next()) {
        const QRegularExpressionMatch match = pattern.match(query.value(0).toString().trimmed());
        if (match.hasMatch()) maximum = qMax(maximum, match.captured(1).toInt());
    }
    if (maximum >= 999) {
        setError(errorMessage, QStringLiteral("%1的送检单三位流水号已用完。")
                                   .arg(date.toString(QStringLiteral("yyyy-MM-dd"))));
        return {};
    }
    return prefix + QStringLiteral("%1").arg(maximum + 1, 3, 10, QLatin1Char('0'));
}

bool InspectionService::replaceLines(qlonglong noticeId,
                                     const QList<InspectionNoticeLine> &lines,
                                     QString *errorMessage)
{
    struct ExistingResult {
        qlonglong materialId = 0;
        double quantity = 0.0;
        QString batchNumber;
        double qualifiedQuantity = 0.0;
        double unqualifiedQuantity = 0.0;
        double inboundQuantity = 0.0;
    };
    QMap<qlonglong, ExistingResult> existingResults;
    QSqlQuery existing(m_database);
    existing.prepare(QStringLiteral(
        "SELECT id,material_id,quantity,batch_no,qualified_quantity,unqualified_quantity,"
        "inbound_quantity FROM inspection_notice_items WHERE notice_id=?"));
    existing.addBindValue(noticeId);
    if (!existing.exec()) {
        setError(errorMessage, QStringLiteral("读取原送检明细失败：%1")
                                   .arg(existing.lastError().text()));
        return false;
    }
    while (existing.next()) {
        existingResults.insert(existing.value(0).toLongLong(),
            {existing.value(1).toLongLong(), existing.value(2).toDouble(),
             existing.value(3).toString(), existing.value(4).toDouble(),
             existing.value(5).toDouble(), existing.value(6).toDouble()});
    }

    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM inspection_notice_items WHERE notice_id=?"));
    remove.addBindValue(noticeId);
    if (!remove.exec()) {
        setError(errorMessage, QStringLiteral("清理原送检明细失败：%1").arg(remove.lastError().text()));
        return false;
    }
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO inspection_notice_items(notice_id,line_number,material_id,quantity,"
        "purchase_order_no,batch_no,supplier,qualified_quantity,unqualified_quantity,"
        "inbound_quantity) VALUES(?,?,?,?,?,?,?,?,?,?)"));
    for (int index = 0; index < lines.size(); ++index) {
        const InspectionNoticeLine &line = lines.at(index);
        double qualifiedQuantity = 0.0;
        double unqualifiedQuantity = 0.0;
        double inboundQuantity = 0.0;
        const auto previous = existingResults.constFind(line.id);
        if (previous != existingResults.cend()) {
            const bool sameIdentity = previous->materialId == line.materialId
                && previous->batchNumber.trimmed().compare(
                    line.batchNumber.trimmed(), Qt::CaseInsensitive) == 0
                && std::abs(previous->quantity - line.quantity) <= QuantityTolerance;
            if (!sameIdentity && previous->inboundQuantity > QuantityTolerance) {
                setError(errorMessage,
                         QStringLiteral("第 %1 行已有合格品入库，不能改变该行物料、批号或送检数量。")
                             .arg(index + 1));
                return false;
            }
            if (sameIdentity) {
                qualifiedQuantity = previous->qualifiedQuantity;
                unqualifiedQuantity = previous->unqualifiedQuantity;
                inboundQuantity = previous->inboundQuantity;
            }
        }
        insert.bindValue(0, noticeId);
        insert.bindValue(1, index + 1);
        insert.bindValue(2, line.materialId);
        insert.bindValue(3, line.quantity);
        insert.bindValue(4, textValue(line.purchaseOrderNumber));
        insert.bindValue(5, textValue(line.batchNumber));
        insert.bindValue(6, textValue(line.supplier));
        insert.bindValue(7, qualifiedQuantity);
        insert.bindValue(8, unqualifiedQuantity);
        insert.bindValue(9, inboundQuantity);
        if (!insert.exec()) {
            setError(errorMessage, QStringLiteral("保存第 %1 行送检明细失败：%2")
                                       .arg(index + 1).arg(insert.lastError().text()));
            return false;
        }
    }
    return true;
}

bool InspectionService::writeAudit(const QString &action,
                                   qlonglong noticeId,
                                   const QString &detail,
                                   QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
        "VALUES(?,?,'inspection_notice',?,?)"));
    query.addBindValue(m_operatorId);
    query.addBindValue(action);
    query.addBindValue(noticeId);
    query.addBindValue(textValue(detail));
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("记录送检操作日志失败：%1").arg(query.lastError().text()));
        return false;
    }
    return true;
}

bool InspectionService::createNotice(const InspectionNoticeDraft &draft,
                                     qlonglong *noticeId,
                                     QString *inspectionNumber,
                                     QString *errorMessage)
{
    if (noticeId) *noticeId = 0;
    if (inspectionNumber) inspectionNumber->clear();
    if (!validateDraft(draft, errorMessage) || !beginImmediate(errorMessage)) return false;

    const QString number = nextInspectionNumber(draft.notificationDate, errorMessage);
    if (number.isEmpty()) {
        rollback();
        return false;
    }
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO inspection_notices(inspection_no,status,notification_date,arrival_date,"
        "entrusted_by,notification_department,urgency,purchase_order_no,supplier,created_by,updated_by) "
        "VALUES(?,'PENDING',?,?,?,?,?,?,?,?,?)"));
    insert.addBindValue(number);
    insert.addBindValue(draft.notificationDate.toString(Qt::ISODate));
    insert.addBindValue(draft.arrivalDate.toString(Qt::ISODate));
    insert.addBindValue(textValue(draft.entrustedBy));
    insert.addBindValue(textValue(draft.notificationDepartment));
    insert.addBindValue(draft.urgency.trimmed().toUpper());
    insert.addBindValue(textValue(draft.purchaseOrderNumber));
    insert.addBindValue(textValue(draft.supplier));
    insert.addBindValue(m_operatorId);
    insert.addBindValue(m_operatorId);
    if (!insert.exec()) {
        setError(errorMessage, QStringLiteral("创建送检通知失败：%1").arg(insert.lastError().text()));
        rollback();
        return false;
    }
    const qlonglong id = insert.lastInsertId().toLongLong();
    if (!replaceLines(id, draft.lines, errorMessage)
        || !writeAudit(QStringLiteral("INSPECTION_NOTICE_CREATE"), id, number, errorMessage)
        || !commit(errorMessage)) {
        rollback();
        return false;
    }
    if (noticeId) *noticeId = id;
    if (inspectionNumber) *inspectionNumber = number;
    return true;
}

bool InspectionService::markAttachmentDeleted(qlonglong attachmentId, QString *errorMessage)
{
    if (attachmentId <= 0) return true;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE attachments SET is_deleted=1,deleted_by=?,deleted_at=? "
        "WHERE id=? AND is_deleted=0"));
    query.addBindValue(m_operatorId);
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    query.addBindValue(attachmentId);
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("清理旧附件失败：%1").arg(query.lastError().text()));
        return false;
    }
    return true;
}

bool InspectionService::updateNotice(qlonglong noticeId,
                                     const InspectionNoticeDraft &draft,
                                     QString *inspectionNumber,
                                     QString *errorMessage)
{
    if (inspectionNumber) inspectionNumber->clear();
    if (noticeId <= 0 || !validateDraft(draft, errorMessage)
        || !beginImmediate(errorMessage)) return false;

    QSqlQuery current(m_database);
    current.prepare(QStringLiteral(
        "SELECT status,notification_date,inspection_attachment_id,template_file_attachment_id,"
        "linked_document_id,inspection_no FROM inspection_notices WHERE id=?"));
    current.addBindValue(noticeId);
    if (!current.exec() || !current.next()) {
        setError(errorMessage, QStringLiteral("送检通知不存在或读取失败：%1").arg(current.lastError().text()));
        rollback();
        return false;
    }
    const QString status = current.value(0).toString();
    QString number = current.value(5).toString();
    if (QDate::fromString(current.value(1).toString(), Qt::ISODate) != draft.notificationDate) {
        number = nextInspectionNumber(draft.notificationDate, errorMessage);
        if (number.isEmpty()) {
            rollback();
            return false;
        }
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE inspection_notices SET inspection_no=?,notification_date=?,"
        "arrival_date=?,entrusted_by=?,notification_department=?,urgency=?,purchase_order_no=?,"
        "supplier=?,updated_by=?,updated_at=? WHERE id=?"));
    update.addBindValue(number);
    update.addBindValue(draft.notificationDate.toString(Qt::ISODate));
    update.addBindValue(draft.arrivalDate.toString(Qt::ISODate));
    update.addBindValue(textValue(draft.entrustedBy));
    update.addBindValue(textValue(draft.notificationDepartment));
    update.addBindValue(draft.urgency.trimmed().toUpper());
    update.addBindValue(textValue(draft.purchaseOrderNumber));
    update.addBindValue(textValue(draft.supplier));
    update.addBindValue(m_operatorId);
    update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    update.addBindValue(noticeId);
    if (!update.exec() || update.numRowsAffected() != 1
        || !replaceLines(noticeId, draft.lines, errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) {
            setError(errorMessage, QStringLiteral("送检通知已被其他操作改变，请刷新后重试。"));
        }
        rollback();
        return false;
    }
    QSqlQuery refreshResult(m_database);
    refreshResult.prepare(QStringLiteral(
        "UPDATE inspection_notices SET status=CASE "
        "WHEN EXISTS(SELECT 1 FROM inspection_notice_items i WHERE i.notice_id=inspection_notices.id "
        "AND ABS(i.quantity-i.qualified_quantity-i.unqualified_quantity)>0.0000001) THEN 'PENDING' "
        "WHEN EXISTS(SELECT 1 FROM inspection_notice_items i WHERE i.notice_id=inspection_notices.id "
        "AND i.unqualified_quantity>0.0000001) THEN 'PENDING' "
        "WHEN EXISTS(SELECT 1 FROM inspection_notice_items i WHERE i.notice_id=inspection_notices.id "
        "AND i.qualified_quantity-i.inbound_quantity>0.0000001) THEN 'QUALIFIED' ELSE 'USED' END,"
        "inspection_result=CASE "
        "WHEN EXISTS(SELECT 1 FROM inspection_notice_items i WHERE i.notice_id=inspection_notices.id "
        "AND ABS(i.quantity-i.qualified_quantity-i.unqualified_quantity)>0.0000001) THEN 'PENDING' "
        "WHEN EXISTS(SELECT 1 FROM inspection_notice_items i WHERE i.notice_id=inspection_notices.id "
        "AND i.unqualified_quantity>0.0000001) THEN 'UNQUALIFIED' ELSE 'QUALIFIED' END,"
        "completed_at=CASE WHEN EXISTS(SELECT 1 FROM inspection_notice_items i "
        "WHERE i.notice_id=inspection_notices.id "
        "AND ABS(i.quantity-i.qualified_quantity-i.unqualified_quantity)>0.0000001) "
        "THEN NULL ELSE COALESCE(completed_at,?) END WHERE id=?"));
    refreshResult.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    refreshResult.addBindValue(noticeId);
    if (!refreshResult.exec()) {
        setError(errorMessage, QStringLiteral("更新送检结果状态失败：%1")
                                   .arg(refreshResult.lastError().text()));
        rollback();
        return false;
    }
    QSqlQuery syncInbound(m_database);
    syncInbound.prepare(QStringLiteral(
        "UPDATE inbound_inspection_details SET inspection_no=? WHERE inspection_notice_id=?"));
    syncInbound.addBindValue(number);
    syncInbound.addBindValue(noticeId);
    if (!syncInbound.exec()
        || !writeAudit(QStringLiteral("INSPECTION_NOTICE_UPDATE"), noticeId,
                       QStringLiteral("%1 / %2").arg(number, status), errorMessage)
        || !commit(errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) {
            setError(errorMessage, QStringLiteral("送检通知已被其他操作改变，请刷新后重试。"));
        }
        rollback();
        return false;
    }
    if (inspectionNumber) *inspectionNumber = number;
    return true;
}

bool InspectionService::readNotice(qlonglong noticeId,
                                   InspectionNotice *notice,
                                   QString *errorMessage) const
{
    if (!notice || noticeId <= 0) {
        setError(errorMessage, QStringLiteral("送检通知参数无效。"));
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT ") + noticeSelectColumns()
                  + QStringLiteral("FROM inspection_notices n LEFT JOIN users u ON u.id=n.created_by "
                                   "WHERE n.id=?"));
    query.addBindValue(noticeId);
    if (!query.exec() || !query.next()) {
        setError(errorMessage, QStringLiteral("读取送检通知失败：%1").arg(query.lastError().text()));
        return false;
    }
    InspectionNotice result;
    readNoticeColumns(query, &result);

    QSqlQuery lines(m_database);
    lines.prepare(QStringLiteral(
        "SELECT i.id,i.line_number,i.material_id,m.code,m.name,m.specification,i.quantity,"
        "i.purchase_order_no,i.batch_no,i.supplier,i.qualified_quantity,"
        "i.unqualified_quantity,i.inbound_quantity FROM inspection_notice_items i "
        "JOIN materials m ON m.id=i.material_id WHERE i.notice_id=? ORDER BY i.line_number"));
    lines.addBindValue(noticeId);
    if (!lines.exec()) {
        setError(errorMessage, QStringLiteral("读取送检物料明细失败：%1").arg(lines.lastError().text()));
        return false;
    }
    while (lines.next()) {
        InspectionNoticeLine line;
        line.id = lines.value(0).toLongLong();
        line.lineNumber = lines.value(1).toInt();
        line.materialId = lines.value(2).toLongLong();
        line.materialCode = lines.value(3).toString();
        line.materialName = lines.value(4).toString();
        line.specification = lines.value(5).toString();
        line.quantity = lines.value(6).toDouble();
        line.purchaseOrderNumber = lines.value(7).toString();
        line.batchNumber = lines.value(8).toString();
        line.supplier = lines.value(9).toString();
        line.qualifiedQuantity = lines.value(10).toDouble();
        line.unqualifiedQuantity = lines.value(11).toDouble();
        line.inboundQuantity = lines.value(12).toDouble();
        result.lines.append(line);
    }
    *notice = result;
    return true;
}

bool InspectionService::listNotices(const QStringList &statuses,
                                    const QString &keyword,
                                    QList<InspectionNotice> *notices,
                                    QString *errorMessage) const
{
    if (!notices) {
        setError(errorMessage, QStringLiteral("送检通知列表参数无效。"));
        return false;
    }
    notices->clear();
    QStringList normalizedStatuses;
    static const QSet<QString> allowed = {
        QStringLiteral("PENDING"), QStringLiteral("QUALIFIED"),
        QStringLiteral("UNQUALIFIED"), QStringLiteral("USED"), QStringLiteral("CANCELLED")};
    for (const QString &status : statuses) {
        const QString normalized = status.trimmed().toUpper();
        if (allowed.contains(normalized) && !normalizedStatuses.contains(normalized)) {
            normalizedStatuses.append(normalized);
        }
    }
    QString sql = QStringLiteral("SELECT ") + noticeSelectColumns()
        + QStringLiteral("FROM inspection_notices n LEFT JOIN users u ON u.id=n.created_by WHERE 1=1 ");
    if (!normalizedStatuses.isEmpty()) {
        sql += QStringLiteral("AND n.status IN (%1) ")
                   .arg(QStringList(normalizedStatuses.size(), QStringLiteral("?")).join(QLatin1Char(',')));
    }
    const QString trimmedKeyword = keyword.trimmed();
    if (!trimmedKeyword.isEmpty()) {
        sql += QStringLiteral(
            "AND (n.inspection_no LIKE ? OR n.purchase_order_no LIKE ? OR n.supplier LIKE ? "
            "OR n.entrusted_by LIKE ?) ");
    }
    sql += QStringLiteral("ORDER BY n.notification_date DESC,n.id DESC");
    QSqlQuery query(m_database);
    query.prepare(sql);
    for (const QString &status : std::as_const(normalizedStatuses)) query.addBindValue(status);
    if (!trimmedKeyword.isEmpty()) {
        const QString like = QLatin1Char('%') + trimmedKeyword + QLatin1Char('%');
        for (int index = 0; index < 4; ++index) query.addBindValue(like);
    }
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("查询送检通知失败：%1").arg(query.lastError().text()));
        return false;
    }
    while (query.next()) {
        InspectionNotice notice;
        readNoticeColumns(query, &notice);
        notices->append(notice);
    }
    return true;
}

bool InspectionService::listQualifiedUnused(QList<InspectionNotice> *notices,
                                            QString *errorMessage) const
{
    if (!listNotices({QStringLiteral("PENDING"), QStringLiteral("QUALIFIED")},
                     QString(), notices, errorMessage)) return false;
    for (int index = notices->size() - 1; index >= 0; --index) {
        const InspectionNotice &notice = notices->at(index);
        if (notice.inspectionAttachmentId <= 0
            || notice.qualifiedQuantity - notice.inboundQuantity <= QuantityTolerance) {
            notices->removeAt(index);
        }
    }
    return true;
}

qlonglong InspectionService::storeAttachment(qlonglong noticeId,
                                             const QString &fileName,
                                             const QString &mimeType,
                                             const QByteArray &data,
                                             QString *errorMessage)
{
    if (data.isEmpty() || fileName.trimmed().isEmpty() || data.size() > MaximumAttachmentBytes) {
        setError(errorMessage, data.size() > MaximumAttachmentBytes
                                   ? QStringLiteral("送检附件不能超过50 MB。")
                                   : QStringLiteral("送检附件文件名或内容为空。"));
        return 0;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO attachments(business_type,business_id,original_file_name,mime_type,file_size,"
        "sha256,file_data,uploaded_by) VALUES('inspection_notice',?,?,?,?,?,?,?)"));
    query.addBindValue(noticeId);
    query.addBindValue(textValue(fileName));
    query.addBindValue(textValue(mimeType));
    query.addBindValue(data.size());
    query.addBindValue(QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
    query.addBindValue(data);
    query.addBindValue(m_operatorId);
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("保存送检附件失败：%1").arg(query.lastError().text()));
        return 0;
    }
    return query.lastInsertId().toLongLong();
}

qlonglong InspectionService::storeResultAttachment(qlonglong resultId,
                                                    const QString &fileName,
                                                    const QString &mimeType,
                                                    const QByteArray &data,
                                                    QString *errorMessage)
{
    if (data.isEmpty() || fileName.trimmed().isEmpty() || data.size() > MaximumAttachmentBytes) {
        setError(errorMessage, data.size() > MaximumAttachmentBytes
                                   ? QStringLiteral("检验附件不能超过50 MB。")
                                   : QStringLiteral("检验附件文件名或内容为空。"));
        return 0;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO attachments(business_type,business_id,original_file_name,mime_type,file_size,"
        "sha256,file_data,uploaded_by) VALUES('inspection_result',?,?,?,?,?,?,?)"));
    query.addBindValue(resultId);
    query.addBindValue(textValue(fileName));
    query.addBindValue(textValue(mimeType));
    query.addBindValue(data.size());
    query.addBindValue(QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
    query.addBindValue(data);
    query.addBindValue(m_operatorId);
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("保存本次检验附件失败：%1")
                                   .arg(query.lastError().text()));
        return 0;
    }
    return query.lastInsertId().toLongLong();
}

bool InspectionService::recordResult(qlonglong noticeId,
                                     const InspectionNoticeResult &result,
                                     QString *errorMessage)
{
    if (noticeId <= 0 || m_operatorId <= 0 || !result.inspectionDate.isValid()
        || result.inspectorName.trimmed().isEmpty() || result.lines.isEmpty()) {
        setError(errorMessage, QStringLiteral("检验日期、检验员或检验数量无效。"));
        return false;
    }
    if (result.attachmentData.size() > MaximumAttachmentBytes) {
        setError(errorMessage, QStringLiteral("检验附件不能超过50 MB。"));
        return false;
    }
    if (!result.attachmentData.isEmpty() && result.attachmentFileName.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("检验附件缺少文件名。"));
        return false;
    }
    QMap<qlonglong, InspectionNoticeLineResult> enteredLines;
    for (const InspectionNoticeLineResult &line : result.lines) {
        if (line.itemId <= 0 || !std::isfinite(line.qualifiedQuantity)
            || !std::isfinite(line.unqualifiedQuantity)
            || line.qualifiedQuantity < -QuantityTolerance
            || line.unqualifiedQuantity < -QuantityTolerance
            || enteredLines.contains(line.itemId)) {
            setError(errorMessage, QStringLiteral("检验明细包含无效或重复的合格/不合格数量。"));
            return false;
        }
        enteredLines.insert(line.itemId, line);
    }
    if (!beginImmediate(errorMessage)) return false;
    QSqlQuery current(m_database);
    current.prepare(QStringLiteral("SELECT status FROM inspection_notices WHERE id=?"));
    current.addBindValue(noticeId);
    if (!current.exec() || !current.next()) {
        setError(errorMessage, QStringLiteral("送检通知不存在或读取失败：%1").arg(current.lastError().text()));
        rollback();
        return false;
    }
    const QString status = current.value(0).toString();
    if (status == QStringLiteral("CANCELLED")) {
        setError(errorMessage, QStringLiteral("已取消的送检通知不能录入检验结果。"));
        rollback();
        return false;
    }

    struct PendingLine {
        qlonglong id = 0;
        int lineNumber = 0;
        qlonglong materialId = 0;
        QString materialCode;
        QString materialName;
        QString specification;
        QString batchNumber;
        double availableQuantity = 0.0;
    };
    QList<PendingLine> pendingLines;
    double resultQualified = 0.0;
    QSqlQuery items(m_database);
    items.prepare(QStringLiteral(
        "SELECT i.id,i.line_number,i.material_id,m.code,m.name,m.specification,i.batch_no,"
        "i.quantity,i.qualified_quantity,i.unqualified_quantity "
        "FROM inspection_notice_items i JOIN materials m ON m.id=i.material_id "
        "WHERE i.notice_id=? ORDER BY i.line_number"));
    items.addBindValue(noticeId);
    if (!items.exec()) {
        setError(errorMessage, QStringLiteral("读取送检数量失败：%1").arg(items.lastError().text()));
        rollback();
        return false;
    }
    while (items.next()) {
        const qlonglong itemId = items.value(0).toLongLong();
        const int lineNumber = items.value(1).toInt();
        const double quantity = items.value(7).toDouble();
        const double previousQualified = items.value(8).toDouble();
        const double previousUnqualified = items.value(9).toDouble();
        const bool previouslyInspected = previousQualified > QuantityTolerance
            || previousUnqualified > QuantityTolerance;
        const double available = previouslyInspected ? previousUnqualified : quantity;
        if (available <= QuantityTolerance) continue;
        const auto entered = enteredLines.constFind(itemId);
        if (entered == enteredLines.cend()) {
            setError(errorMessage, QStringLiteral("第 %1 行缺少本次合格或不合格数量。")
                                       .arg(lineNumber));
            rollback();
            return false;
        }
        if (std::abs(entered->qualifiedQuantity + entered->unqualifiedQuantity - available)
                > QuantityTolerance) {
            setError(errorMessage,
                     QStringLiteral("第 %1 行本次合格数量与不合格数量之和必须等于本次待检数量 %2。")
                         .arg(lineNumber).arg(available, 0, 'g', 12));
            rollback();
            return false;
        }
        resultQualified += entered->qualifiedQuantity;
        pendingLines.append({itemId, lineNumber, items.value(2).toLongLong(),
                             items.value(3).toString(), items.value(4).toString(),
                             items.value(5).toString(), items.value(6).toString(), available});
    }
    if (pendingLines.isEmpty()) {
        setError(errorMessage, QStringLiteral("该送检通知当前没有需要再次检验的数量。"));
        rollback();
        return false;
    }
    if (pendingLines.size() != enteredLines.size()) {
        setError(errorMessage, QStringLiteral("本次检验明细与当前待检明细不一致，请关闭窗口后重新打开。"));
        rollback();
        return false;
    }
    if (resultQualified > QuantityTolerance && result.attachmentData.isEmpty()) {
        setError(errorMessage, QStringLiteral("本次检验存在合格数量，必须上传本次检验附件。"));
        rollback();
        return false;
    }

    QSqlQuery sequence(m_database);
    sequence.prepare(QStringLiteral(
        "SELECT COALESCE(MAX(sequence_no),0)+1 FROM inspection_results WHERE notice_id=?"));
    sequence.addBindValue(noticeId);
    if (!sequence.exec() || !sequence.next()) {
        setError(errorMessage, QStringLiteral("生成检验次数失败：%1").arg(sequence.lastError().text()));
        rollback();
        return false;
    }
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    QSqlQuery insertResult(m_database);
    insertResult.prepare(QStringLiteral(
        "INSERT INTO inspection_results(notice_id,sequence_no,inspection_date,inspector_name,"
        "conclusion,created_by,updated_by,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)"));
    insertResult.addBindValue(noticeId);
    insertResult.addBindValue(sequence.value(0).toInt());
    insertResult.addBindValue(result.inspectionDate.toString(Qt::ISODate));
    insertResult.addBindValue(textValue(result.inspectorName));
    insertResult.addBindValue(textValue(result.conclusion));
    insertResult.addBindValue(m_operatorId);
    insertResult.addBindValue(m_operatorId);
    insertResult.addBindValue(now);
    insertResult.addBindValue(now);
    if (!insertResult.exec()) {
        setError(errorMessage, QStringLiteral("新增本次检验记录失败：%1")
                                   .arg(insertResult.lastError().text()));
        rollback();
        return false;
    }
    const qlonglong resultId = insertResult.lastInsertId().toLongLong();
    qlonglong attachmentId = 0;
    if (!result.attachmentData.isEmpty()) {
        attachmentId = storeResultAttachment(resultId, result.attachmentFileName,
                                             result.attachmentMimeType, result.attachmentData,
                                             errorMessage);
        if (attachmentId <= 0) {
            rollback();
            return false;
        }
        QSqlQuery attach(m_database);
        attach.prepare(QStringLiteral("UPDATE inspection_results SET attachment_id=? WHERE id=?"));
        attach.addBindValue(attachmentId);
        attach.addBindValue(resultId);
        if (!attach.exec()) {
            setError(errorMessage, QStringLiteral("关联本次检验附件失败：%1")
                                       .arg(attach.lastError().text()));
            rollback();
            return false;
        }
    }

    QSqlQuery insertItem(m_database);
    insertItem.prepare(QStringLiteral(
        "INSERT INTO inspection_result_items(result_id,notice_item_id,line_number,material_id,"
        "material_code,material_name,specification,batch_no,inspected_quantity,"
        "qualified_quantity,unqualified_quantity) VALUES(?,?,?,?,?,?,?,?,?,?,?)"));
    for (const PendingLine &line : std::as_const(pendingLines)) {
        const InspectionNoticeLineResult entered = enteredLines.value(line.id);
        insertItem.bindValue(0, resultId);
        insertItem.bindValue(1, line.id);
        insertItem.bindValue(2, line.lineNumber);
        insertItem.bindValue(3, line.materialId);
        insertItem.bindValue(4, line.materialCode);
        insertItem.bindValue(5, line.materialName);
        insertItem.bindValue(6, line.specification);
        insertItem.bindValue(7, line.batchNumber);
        insertItem.bindValue(8, line.availableQuantity);
        insertItem.bindValue(9, qMax(0.0, entered.qualifiedQuantity));
        insertItem.bindValue(10, qMax(0.0, entered.unqualifiedQuantity));
        if (!insertItem.exec()) {
            setError(errorMessage, QStringLiteral("保存本次检验明细失败：%1")
                                       .arg(insertItem.lastError().text()));
            rollback();
            return false;
        }
    }

    if (!refreshNoticeResultSummary(noticeId, errorMessage)
        || !writeAudit(QStringLiteral("INSPECTION_RESULT_RECORD"), noticeId,
                       QStringLiteral("第%1次检验 / 本次合格%2 / 本次不合格%3")
                           .arg(sequence.value(0).toInt())
                           .arg(resultQualified, 0, 'g', 12)
                           .arg(std::accumulate(
                               enteredLines.cbegin(), enteredLines.cend(), 0.0,
                               [](double total, const InspectionNoticeLineResult &line) {
                                   return total + line.unqualifiedQuantity;
                               }), 0, 'g', 12),
                       errorMessage)
        || !commit(errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) {
            setError(errorMessage, QStringLiteral("送检通知已被其他操作改变，请刷新后重试。"));
        }
        rollback();
        return false;
    }
    return true;
}

bool InspectionService::refreshNoticeResultSummary(qlonglong noticeId,
                                                   QString *errorMessage)
{
    struct ItemState {
        qlonglong id = 0;
        int lineNumber = 0;
        qlonglong materialId = 0;
        QString batchNumber;
        double quantity = 0.0;
        double inboundQuantity = 0.0;
        double qualifiedQuantity = 0.0;
        double unqualifiedQuantity = 0.0;
        bool hasResult = false;
    };
    QMap<qlonglong, ItemState> states;
    QMap<QString, qlonglong> identityIndex;
    QSqlQuery items(m_database);
    items.prepare(QStringLiteral(
        "SELECT id,line_number,material_id,batch_no,quantity,inbound_quantity "
        "FROM inspection_notice_items WHERE notice_id=? ORDER BY line_number"));
    items.addBindValue(noticeId);
    if (!items.exec()) {
        setError(errorMessage, QStringLiteral("读取送检汇总明细失败：%1")
                                   .arg(items.lastError().text()));
        return false;
    }
    while (items.next()) {
        ItemState state;
        state.id = items.value(0).toLongLong();
        state.lineNumber = items.value(1).toInt();
        state.materialId = items.value(2).toLongLong();
        state.batchNumber = items.value(3).toString();
        state.quantity = items.value(4).toDouble();
        state.inboundQuantity = items.value(5).toDouble();
        states.insert(state.id, state);
        identityIndex.insert(QStringLiteral("%1|%2|%3")
                                 .arg(state.lineNumber)
                                 .arg(state.materialId)
                                 .arg(state.batchNumber.trimmed().toUpper()),
                             state.id);
    }

    QSqlQuery resultItems(m_database);
    resultItems.prepare(QStringLiteral(
        "SELECT ri.notice_item_id,ri.line_number,ri.material_id,ri.batch_no,"
        "ri.inspected_quantity,ri.qualified_quantity,ri.unqualified_quantity,r.sequence_no "
        "FROM inspection_result_items ri JOIN inspection_results r ON r.id=ri.result_id "
        "WHERE r.notice_id=? ORDER BY r.sequence_no,ri.line_number"));
    resultItems.addBindValue(noticeId);
    if (!resultItems.exec()) {
        setError(errorMessage, QStringLiteral("读取分次检验记录失败：%1")
                                   .arg(resultItems.lastError().text()));
        return false;
    }
    while (resultItems.next()) {
        qlonglong itemId = resultItems.value(0).toLongLong();
        auto state = states.find(itemId);
        if (state == states.end()) {
            const QString identity = QStringLiteral("%1|%2|%3")
                .arg(resultItems.value(1).toInt())
                .arg(resultItems.value(2).toLongLong())
                .arg(resultItems.value(3).toString().trimmed().toUpper());
            state = states.find(identityIndex.value(identity));
        }
        if (state == states.end()) continue;
        const double inspected = resultItems.value(4).toDouble();
        const double qualified = resultItems.value(5).toDouble();
        const double unqualified = resultItems.value(6).toDouble();
        const double available = state->hasResult ? state->unqualifiedQuantity : state->quantity;
        if (std::abs(qualified + unqualified - inspected) > QuantityTolerance
            || std::abs(inspected - available) > QuantityTolerance) {
            setError(errorMessage,
                     QStringLiteral("第%1次检验的第%2行数量与前一次剩余待检数量不一致，"
                                    "请先修正该次检验记录。")
                         .arg(resultItems.value(7).toInt())
                         .arg(resultItems.value(1).toInt()));
            return false;
        }
        state->qualifiedQuantity += qualified;
        state->unqualifiedQuantity = unqualified;
        state->hasResult = true;
    }

    double totalQualified = 0.0;
    double totalUnqualified = 0.0;
    double totalInbound = 0.0;
    bool hasAnyResult = false;
    QSqlQuery updateItem(m_database);
    updateItem.prepare(QStringLiteral(
        "UPDATE inspection_notice_items SET qualified_quantity=?,unqualified_quantity=? WHERE id=?"));
    for (auto state = states.cbegin(); state != states.cend(); ++state) {
        const double qualified = state->hasResult ? state->qualifiedQuantity : 0.0;
        const double unqualified = state->hasResult ? state->unqualifiedQuantity : 0.0;
        if (qualified + QuantityTolerance < state->inboundQuantity) {
            setError(errorMessage,
                     QStringLiteral("第%1行已有%2个合格品入库，修改后的历次合格数量不能低于已入库数量。")
                         .arg(state->lineNumber)
                         .arg(state->inboundQuantity, 0, 'g', 12));
            return false;
        }
        updateItem.bindValue(0, qualified);
        updateItem.bindValue(1, unqualified);
        updateItem.bindValue(2, state->id);
        if (!updateItem.exec()) {
            setError(errorMessage, QStringLiteral("更新送检汇总数量失败：%1")
                                       .arg(updateItem.lastError().text()));
            return false;
        }
        totalQualified += qualified;
        totalUnqualified += unqualified;
        totalInbound += state->inboundQuantity;
        hasAnyResult = hasAnyResult || state->hasResult;
    }

    QSqlQuery latest(m_database);
    latest.prepare(QStringLiteral(
        "SELECT r.inspection_date,r.inspector_name,r.conclusion,"
        "COALESCE(r.attachment_id,(SELECT previous.attachment_id FROM inspection_results previous "
        "WHERE previous.notice_id=r.notice_id AND previous.attachment_id IS NOT NULL "
        "ORDER BY previous.sequence_no DESC LIMIT 1)),r.updated_at "
        "FROM inspection_results r WHERE r.notice_id=? ORDER BY r.sequence_no DESC LIMIT 1"));
    latest.addBindValue(noticeId);
    if (!latest.exec()) {
        setError(errorMessage, QStringLiteral("读取最近一次检验记录失败：%1")
                                   .arg(latest.lastError().text()));
        return false;
    }
    const bool hasLatest = latest.next();
    const QString nextStatus = !hasAnyResult || totalUnqualified > QuantityTolerance
        ? QStringLiteral("PENDING")
        : (totalQualified - totalInbound > QuantityTolerance
               ? QStringLiteral("QUALIFIED") : QStringLiteral("USED"));
    const QString resultStatus = !hasAnyResult
        ? QStringLiteral("PENDING")
        : (totalUnqualified > QuantityTolerance
               ? QStringLiteral("UNQUALIFIED") : QStringLiteral("QUALIFIED"));
    QSqlQuery updateNotice(m_database);
    updateNotice.prepare(QStringLiteral(
        "UPDATE inspection_notices SET status=?,inspection_date=?,inspector_name=?,"
        "inspection_result=?,conclusion=?,inspection_attachment_id=?,updated_by=?,updated_at=?,"
        "completed_at=? WHERE id=?"));
    updateNotice.addBindValue(nextStatus);
    updateNotice.addBindValue(hasLatest ? latest.value(0) : QVariant());
    updateNotice.addBindValue(hasLatest ? latest.value(1) : QVariant(QString()));
    updateNotice.addBindValue(resultStatus);
    updateNotice.addBindValue(hasLatest ? latest.value(2) : QVariant(QString()));
    updateNotice.addBindValue(hasLatest && latest.value(3).toLongLong() > 0
                                  ? latest.value(3) : QVariant());
    updateNotice.addBindValue(m_operatorId);
    updateNotice.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    updateNotice.addBindValue(hasLatest ? latest.value(4) : QVariant());
    updateNotice.addBindValue(noticeId);
    if (!updateNotice.exec() || updateNotice.numRowsAffected() != 1) {
        setError(errorMessage, QStringLiteral("更新送检通知汇总失败：%1")
                                   .arg(updateNotice.lastError().text()));
        return false;
    }

    QSqlQuery syncInbound(m_database);
    syncInbound.prepare(QStringLiteral(
        "UPDATE inbound_inspection_details SET inspection_date=?,inspector_name=?,"
        "conclusion=?,inspection_attachment_id=? WHERE inspection_notice_id=?"));
    syncInbound.addBindValue(hasLatest ? latest.value(0) : QVariant());
    syncInbound.addBindValue(hasLatest ? latest.value(1) : QVariant(QString()));
    syncInbound.addBindValue(hasLatest ? latest.value(2) : QVariant(QString()));
    syncInbound.addBindValue(hasLatest && latest.value(3).toLongLong() > 0
                                 ? latest.value(3) : QVariant());
    syncInbound.addBindValue(noticeId);
    if (!syncInbound.exec()) {
        setError(errorMessage, QStringLiteral("同步入库检验摘要失败：%1")
                                   .arg(syncInbound.lastError().text()));
        return false;
    }
    return true;
}

bool InspectionService::updateResult(qlonglong resultId,
                                     const InspectionNoticeResult &result,
                                     QString *errorMessage)
{
    if (resultId <= 0 || m_operatorId <= 0 || !result.inspectionDate.isValid()
        || result.inspectorName.trimmed().isEmpty() || result.lines.isEmpty()) {
        setError(errorMessage, QStringLiteral("检验日期、检验员或本次检验数量无效。"));
        return false;
    }
    if (result.attachmentData.size() > MaximumAttachmentBytes) {
        setError(errorMessage, QStringLiteral("检验附件不能超过50 MB。"));
        return false;
    }
    QMap<qlonglong, InspectionNoticeLineResult> enteredLines;
    for (const InspectionNoticeLineResult &line : result.lines) {
        if (line.itemId <= 0 || !std::isfinite(line.qualifiedQuantity)
            || !std::isfinite(line.unqualifiedQuantity)
            || line.qualifiedQuantity < -QuantityTolerance
            || line.unqualifiedQuantity < -QuantityTolerance
            || enteredLines.contains(line.itemId)) {
            setError(errorMessage, QStringLiteral("本次检验明细包含无效或重复数量。"));
            return false;
        }
        enteredLines.insert(line.itemId, line);
    }
    if (!beginImmediate(errorMessage)) return false;
    QSqlQuery current(m_database);
    current.prepare(QStringLiteral(
        "SELECT r.notice_id,r.attachment_id,n.status FROM inspection_results r "
        "JOIN inspection_notices n ON n.id=r.notice_id WHERE r.id=?"));
    current.addBindValue(resultId);
    if (!current.exec() || !current.next()) {
        setError(errorMessage, QStringLiteral("本次检验记录不存在或读取失败：%1")
                                   .arg(current.lastError().text()));
        rollback();
        return false;
    }
    const qlonglong noticeId = current.value(0).toLongLong();
    qlonglong attachmentId = current.value(1).toLongLong();
    if (current.value(2).toString() == QStringLiteral("CANCELLED")) {
        setError(errorMessage, QStringLiteral("已取消的送检通知不能修改检验结果。"));
        rollback();
        return false;
    }

    double totalQualified = 0.0;
    int storedCount = 0;
    QSqlQuery stored(m_database);
    stored.prepare(QStringLiteral(
        "SELECT notice_item_id,line_number,inspected_quantity FROM inspection_result_items "
        "WHERE result_id=? ORDER BY line_number"));
    stored.addBindValue(resultId);
    if (!stored.exec()) {
        setError(errorMessage, QStringLiteral("读取本次检验明细失败：%1")
                                   .arg(stored.lastError().text()));
        rollback();
        return false;
    }
    while (stored.next()) {
        ++storedCount;
        const qlonglong itemId = stored.value(0).toLongLong();
        const auto entered = enteredLines.constFind(itemId);
        if (entered == enteredLines.cend()
            || std::abs(entered->qualifiedQuantity + entered->unqualifiedQuantity
                        - stored.value(2).toDouble()) > QuantityTolerance) {
            setError(errorMessage,
                     QStringLiteral("第%1行合格与不合格数量之和必须等于该次检验数量%2。")
                         .arg(stored.value(1).toInt())
                         .arg(stored.value(2).toDouble(), 0, 'g', 12));
            rollback();
            return false;
        }
        totalQualified += entered->qualifiedQuantity;
    }
    if (storedCount != enteredLines.size()) {
        setError(errorMessage, QStringLiteral("本次检验明细与保存记录不一致，请重新打开。"));
        rollback();
        return false;
    }
    if (totalQualified > QuantityTolerance
        && result.attachmentData.isEmpty() && attachmentId <= 0) {
        setError(errorMessage, QStringLiteral("本次检验存在合格数量，必须上传本次检验附件。"));
        rollback();
        return false;
    }
    if (!result.attachmentData.isEmpty()) {
        const qlonglong replacementId = storeResultAttachment(
            resultId, result.attachmentFileName, result.attachmentMimeType,
            result.attachmentData, errorMessage);
        if (replacementId <= 0 || !markAttachmentDeleted(attachmentId, errorMessage)) {
            rollback();
            return false;
        }
        attachmentId = replacementId;
    }

    QSqlQuery updateResultQuery(m_database);
    updateResultQuery.prepare(QStringLiteral(
        "UPDATE inspection_results SET inspection_date=?,inspector_name=?,conclusion=?,"
        "attachment_id=?,updated_by=?,updated_at=? WHERE id=?"));
    updateResultQuery.addBindValue(result.inspectionDate.toString(Qt::ISODate));
    updateResultQuery.addBindValue(textValue(result.inspectorName));
    updateResultQuery.addBindValue(textValue(result.conclusion));
    updateResultQuery.addBindValue(attachmentId > 0 ? QVariant(attachmentId) : QVariant());
    updateResultQuery.addBindValue(m_operatorId);
    updateResultQuery.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    updateResultQuery.addBindValue(resultId);
    if (!updateResultQuery.exec()) {
        setError(errorMessage, QStringLiteral("修改本次检验记录失败：%1")
                                   .arg(updateResultQuery.lastError().text()));
        rollback();
        return false;
    }
    QSqlQuery updateItem(m_database);
    updateItem.prepare(QStringLiteral(
        "UPDATE inspection_result_items SET qualified_quantity=?,unqualified_quantity=? "
        "WHERE result_id=? AND notice_item_id=?"));
    for (auto entered = enteredLines.cbegin(); entered != enteredLines.cend(); ++entered) {
        updateItem.bindValue(0, qMax(0.0, entered->qualifiedQuantity));
        updateItem.bindValue(1, qMax(0.0, entered->unqualifiedQuantity));
        updateItem.bindValue(2, resultId);
        updateItem.bindValue(3, entered.key());
        if (!updateItem.exec() || updateItem.numRowsAffected() != 1) {
            setError(errorMessage, QStringLiteral("修改本次检验明细失败：%1")
                                       .arg(updateItem.lastError().text()));
            rollback();
            return false;
        }
    }
    if (!refreshNoticeResultSummary(noticeId, errorMessage)
        || !writeAudit(QStringLiteral("INSPECTION_RESULT_UPDATE"), noticeId,
                       QStringLiteral("修改独立检验记录ID %1").arg(resultId), errorMessage)
        || !commit(errorMessage)) {
        rollback();
        return false;
    }
    return true;
}

bool InspectionService::readResult(qlonglong resultId,
                                   InspectionResultRecord *record,
                                   QString *errorMessage) const
{
    if (resultId <= 0 || !record) {
        setError(errorMessage, QStringLiteral("本次检验记录参数无效。"));
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT r.id,r.notice_id,n.inspection_no,r.sequence_no,n.notification_date,"
        "n.purchase_order_no,n.supplier,r.inspection_date,r.inspector_name,r.conclusion,"
        "r.attachment_id,r.created_at,COUNT(ri.id),COALESCE(SUM(ri.qualified_quantity),0),"
        "COALESCE(SUM(ri.unqualified_quantity),0) FROM inspection_results r "
        "JOIN inspection_notices n ON n.id=r.notice_id "
        "LEFT JOIN inspection_result_items ri ON ri.result_id=r.id WHERE r.id=? GROUP BY r.id"));
    query.addBindValue(resultId);
    if (!query.exec() || !query.next()) {
        setError(errorMessage, QStringLiteral("读取本次检验记录失败：%1")
                                   .arg(query.lastError().text()));
        return false;
    }
    InspectionResultRecord value;
    value.id = query.value(0).toLongLong();
    value.noticeId = query.value(1).toLongLong();
    value.inspectionNumber = query.value(2).toString();
    value.sequenceNumber = query.value(3).toInt();
    value.notificationDate = QDate::fromString(query.value(4).toString(), Qt::ISODate);
    value.purchaseOrderNumber = query.value(5).toString();
    value.supplier = query.value(6).toString();
    value.inspectionDate = QDate::fromString(query.value(7).toString(), Qt::ISODate);
    value.inspectorName = query.value(8).toString();
    value.conclusion = query.value(9).toString();
    value.attachmentId = query.value(10).toLongLong();
    value.createdAt = dateTimeValue(query.value(11));
    value.lineCount = query.value(12).toInt();
    value.qualifiedQuantity = query.value(13).toDouble();
    value.unqualifiedQuantity = query.value(14).toDouble();

    QSqlQuery lines(m_database);
    lines.prepare(QStringLiteral(
        "SELECT notice_item_id,line_number,material_id,material_code,material_name,specification,"
        "inspected_quantity,batch_no,qualified_quantity,unqualified_quantity "
        "FROM inspection_result_items WHERE result_id=? ORDER BY line_number"));
    lines.addBindValue(resultId);
    if (!lines.exec()) {
        setError(errorMessage, QStringLiteral("读取本次检验物料失败：%1")
                                   .arg(lines.lastError().text()));
        return false;
    }
    while (lines.next()) {
        InspectionNoticeLine line;
        line.id = lines.value(0).toLongLong();
        line.lineNumber = lines.value(1).toInt();
        line.materialId = lines.value(2).toLongLong();
        line.materialCode = lines.value(3).toString();
        line.materialName = lines.value(4).toString();
        line.specification = lines.value(5).toString();
        line.quantity = lines.value(6).toDouble();
        line.batchNumber = lines.value(7).toString();
        line.qualifiedQuantity = lines.value(8).toDouble();
        line.unqualifiedQuantity = lines.value(9).toDouble();
        value.lines.append(line);
    }
    *record = value;
    return true;
}

bool InspectionService::listResults(const QString &keyword,
                                    QList<InspectionResultRecord> *records,
                                    QString *errorMessage) const
{
    if (!records) {
        setError(errorMessage, QStringLiteral("检验历史列表参数无效。"));
        return false;
    }
    records->clear();
    QString sql = QStringLiteral(
        "SELECT r.id,r.notice_id,n.inspection_no,r.sequence_no,n.notification_date,"
        "n.purchase_order_no,n.supplier,r.inspection_date,r.inspector_name,r.conclusion,"
        "r.attachment_id,r.created_at,COUNT(ri.id),COALESCE(SUM(ri.qualified_quantity),0),"
        "COALESCE(SUM(ri.unqualified_quantity),0) FROM inspection_results r "
        "JOIN inspection_notices n ON n.id=r.notice_id "
        "LEFT JOIN inspection_result_items ri ON ri.result_id=r.id WHERE n.status<>'CANCELLED' ");
    const QString trimmed = keyword.trimmed();
    if (!trimmed.isEmpty()) {
        sql += QStringLiteral(
            "AND (n.inspection_no LIKE ? OR n.purchase_order_no LIKE ? OR n.supplier LIKE ? "
            "OR r.inspector_name LIKE ?) ");
    }
    sql += QStringLiteral("GROUP BY r.id ORDER BY r.inspection_date DESC,r.id DESC");
    QSqlQuery query(m_database);
    query.prepare(sql);
    if (!trimmed.isEmpty()) {
        const QString like = QLatin1Char('%') + trimmed + QLatin1Char('%');
        for (int index = 0; index < 4; ++index) query.addBindValue(like);
    }
    if (!query.exec()) {
        setError(errorMessage, QStringLiteral("查询分次检验历史失败：%1")
                                   .arg(query.lastError().text()));
        return false;
    }
    while (query.next()) {
        InspectionResultRecord value;
        value.id = query.value(0).toLongLong();
        value.noticeId = query.value(1).toLongLong();
        value.inspectionNumber = query.value(2).toString();
        value.sequenceNumber = query.value(3).toInt();
        value.notificationDate = QDate::fromString(query.value(4).toString(), Qt::ISODate);
        value.purchaseOrderNumber = query.value(5).toString();
        value.supplier = query.value(6).toString();
        value.inspectionDate = QDate::fromString(query.value(7).toString(), Qt::ISODate);
        value.inspectorName = query.value(8).toString();
        value.conclusion = query.value(9).toString();
        value.attachmentId = query.value(10).toLongLong();
        value.createdAt = dateTimeValue(query.value(11));
        value.lineCount = query.value(12).toInt();
        value.qualifiedQuantity = query.value(13).toDouble();
        value.unqualifiedQuantity = query.value(14).toDouble();
        records->append(value);
    }
    return true;
}

bool InspectionService::recordTemplateArtifact(qlonglong noticeId,
                                               const InspectionTemplateArtifact &artifact,
                                               QString *errorMessage)
{
    if (noticeId <= 0 || m_operatorId <= 0
        || (artifact.payload.trimmed().isEmpty() && artifact.archivePath.trimmed().isEmpty()
            && artifact.data.isEmpty())) {
        setError(errorMessage, QStringLiteral("送检通知模板资料无效。"));
        return false;
    }
    if (artifact.data.size() > MaximumAttachmentBytes) {
        setError(errorMessage, QStringLiteral("送检通知表单附件不能超过50 MB。"));
        return false;
    }
    if (!artifact.data.isEmpty() && artifact.fileName.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("送检通知表单附件缺少文件名。"));
        return false;
    }
    if (!beginImmediate(errorMessage)) return false;
    QSqlQuery current(m_database);
    current.prepare(QStringLiteral(
        "SELECT status,template_file_attachment_id FROM inspection_notices WHERE id=?"));
    current.addBindValue(noticeId);
    if (!current.exec() || !current.next()) {
        setError(errorMessage, QStringLiteral("送检通知不存在或读取失败：%1").arg(current.lastError().text()));
        rollback();
        return false;
    }
    if (current.value(0).toString() == QStringLiteral("USED")
        || current.value(0).toString() == QStringLiteral("CANCELLED")) {
        setError(errorMessage, QStringLiteral("已入库或已取消的送检通知不能替换通知单表单。"));
        rollback();
        return false;
    }
    qlonglong attachmentId = current.value(1).toLongLong();
    if (!artifact.data.isEmpty()) {
        if (attachmentId > 0) {
            QSqlQuery replace(m_database);
            replace.prepare(QStringLiteral(
                "UPDATE attachments SET original_file_name=?,mime_type=?,file_size=?,sha256=?,"
                "file_data=?,is_deleted=0,deleted_by=NULL,deleted_at=NULL WHERE id=? "
                "AND business_type='inspection_notice' AND business_id=?"));
            replace.addBindValue(textValue(artifact.fileName));
            replace.addBindValue(textValue(artifact.mimeType));
            replace.addBindValue(artifact.data.size());
            replace.addBindValue(QString::fromLatin1(
                QCryptographicHash::hash(artifact.data, QCryptographicHash::Sha256).toHex()));
            replace.addBindValue(artifact.data);
            replace.addBindValue(attachmentId);
            replace.addBindValue(noticeId);
            if (!replace.exec() || replace.numRowsAffected() != 1) {
                setError(errorMessage, QStringLiteral("更新送检通知表单附件失败：%1")
                                           .arg(replace.lastError().text()));
                rollback();
                return false;
            }
        } else {
            attachmentId = storeAttachment(noticeId, artifact.fileName,
                                           artifact.mimeType, artifact.data,
                                           errorMessage);
            if (attachmentId <= 0) {
                rollback();
                return false;
            }
        }
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE inspection_notices SET template_payload=?,archive_path=?,"
        "template_file_attachment_id=?,updated_by=?,updated_at=? WHERE id=? "
        "AND status NOT IN ('USED','CANCELLED')"));
    update.addBindValue(artifact.payload);
    update.addBindValue(textValue(artifact.archivePath));
    update.addBindValue(attachmentId > 0 ? QVariant(attachmentId) : QVariant());
    update.addBindValue(m_operatorId);
    update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    update.addBindValue(noticeId);
    if (!update.exec() || update.numRowsAffected() != 1
        || !writeAudit(QStringLiteral("INSPECTION_NOTICE_FORM_SAVE"), noticeId,
                       artifact.archivePath, errorMessage)
        || !commit(errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) {
            setError(errorMessage, QStringLiteral("送检通知已被其他操作改变，请刷新后重试。"));
        }
        rollback();
        return false;
    }
    return true;
}

bool InspectionService::cancelNotice(qlonglong noticeId,
                                     const QString &reason,
                                     QString *errorMessage)
{
    if (noticeId <= 0 || m_operatorId <= 0 || reason.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("送检通知编号或撤销原因无效。"));
        return false;
    }
    if (!beginImmediate(errorMessage)) return false;
    const QString cancelledAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE inspection_notices SET status='CANCELLED',updated_by=?,updated_at=? "
        "WHERE id=? AND status IN ('PENDING','QUALIFIED','UNQUALIFIED') "
        "AND linked_document_id IS NULL"));
    update.addBindValue(m_operatorId);
    update.addBindValue(cancelledAt);
    update.addBindValue(noticeId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        if (errorMessage && errorMessage->isEmpty()) {
            setError(errorMessage, QStringLiteral("送检通知不存在、已取消或已经用于入库。"));
        }
        rollback();
        return false;
    }
    QSqlQuery hideAttachments(m_database);
    hideAttachments.prepare(QStringLiteral(
        "UPDATE attachments SET is_deleted=1,deleted_by=?,deleted_at=? "
        "WHERE business_type='inspection_notice' AND business_id=? AND is_deleted=0"));
    hideAttachments.addBindValue(m_operatorId);
    hideAttachments.addBindValue(cancelledAt);
    hideAttachments.addBindValue(noticeId);
    if (!hideAttachments.exec()
        || !writeAudit(QStringLiteral("INSPECTION_NOTICE_CANCEL"), noticeId,
                       reason.trimmed(), errorMessage)
        || !commit(errorMessage)) {
        if (errorMessage && errorMessage->isEmpty())
            setError(errorMessage, QStringLiteral("同步送检通知附件状态失败。"));
        rollback();
        return false;
    }
    return true;
}
