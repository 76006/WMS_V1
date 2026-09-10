#include "services/InspectionService.h"

#include <QCryptographicHash>
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
}

QString noticeSelectColumns()
{
    return QStringLiteral(
        "n.id,n.inspection_no,n.status,n.notification_date,n.arrival_date,n.entrusted_by,"
        "n.notification_department,n.urgency,n.purchase_order_no,n.supplier,n.inspector_name,"
        "n.inspection_date,n.inspection_result,n.conclusion,n.inspection_attachment_id,"
        "n.template_file_attachment_id,n.template_payload,n.archive_path,n.linked_document_id,"
        "n.created_by,COALESCE(u.display_name,''),n.created_at,n.updated_at,n.completed_at,"
        "(SELECT COUNT(*) FROM inspection_notice_items i WHERE i.notice_id=n.id) ");
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
        "purchase_order_no,batch_no,supplier) VALUES(?,?,?,?,?,?,?)"));
    for (int index = 0; index < lines.size(); ++index) {
        const InspectionNoticeLine &line = lines.at(index);
        insert.bindValue(0, noticeId);
        insert.bindValue(1, index + 1);
        insert.bindValue(2, line.materialId);
        insert.bindValue(3, line.quantity);
        insert.bindValue(4, textValue(line.purchaseOrderNumber));
        insert.bindValue(5, textValue(line.batchNumber));
        insert.bindValue(6, textValue(line.supplier));
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
        "i.purchase_order_no,i.batch_no,i.supplier FROM inspection_notice_items i "
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
    if (!listNotices({QStringLiteral("QUALIFIED")}, QString(), notices, errorMessage)) return false;
    for (int index = notices->size() - 1; index >= 0; --index) {
        if (notices->at(index).linkedDocumentId > 0) notices->removeAt(index);
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

bool InspectionService::recordResult(qlonglong noticeId,
                                     const InspectionNoticeResult &result,
                                     QString *errorMessage)
{
    const QString normalizedResult = result.result.trimmed().toUpper();
    if (noticeId <= 0 || m_operatorId <= 0 || !result.inspectionDate.isValid()
        || result.inspectorName.trimmed().isEmpty()
        || (normalizedResult != QStringLiteral("QUALIFIED")
            && normalizedResult != QStringLiteral("UNQUALIFIED"))) {
        setError(errorMessage, QStringLiteral("检验日期、检验员或检验结果无效。"));
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
    if (!beginImmediate(errorMessage)) return false;
    QSqlQuery current(m_database);
    current.prepare(QStringLiteral(
        "SELECT status,linked_document_id,inspection_attachment_id FROM inspection_notices WHERE id=?"));
    current.addBindValue(noticeId);
    if (!current.exec() || !current.next()) {
        setError(errorMessage, QStringLiteral("送检通知不存在或读取失败：%1").arg(current.lastError().text()));
        rollback();
        return false;
    }
    const QString status = current.value(0).toString();
    qlonglong attachmentId = current.value(2).toLongLong();
    if (normalizedResult == QStringLiteral("QUALIFIED")
        && result.attachmentData.isEmpty() && attachmentId <= 0) {
        setError(errorMessage, QStringLiteral("检验合格必须上传检验附件。"));
        rollback();
        return false;
    }
    if (!result.attachmentData.isEmpty()) {
        const qlonglong replacementId = storeAttachment(
            noticeId, result.attachmentFileName, result.attachmentMimeType,
            result.attachmentData, errorMessage);
        if (replacementId <= 0) {
            rollback();
            return false;
        }
        if (!markAttachmentDeleted(attachmentId, errorMessage)) {
            rollback();
            return false;
        }
        attachmentId = replacementId;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE inspection_notices SET status=CASE WHEN linked_document_id IS NOT NULL THEN 'USED' ELSE ? END,"
        "inspector_name=?,inspection_date=?,"
        "inspection_result=?,conclusion=?,inspection_attachment_id=?,updated_by=?,updated_at=?,"
        "completed_at=? WHERE id=?"));
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    update.addBindValue(normalizedResult);
    update.addBindValue(textValue(result.inspectorName));
    update.addBindValue(result.inspectionDate.toString(Qt::ISODate));
    update.addBindValue(normalizedResult);
    update.addBindValue(textValue(result.conclusion));
    update.addBindValue(attachmentId > 0 ? QVariant(attachmentId) : QVariant());
    update.addBindValue(m_operatorId);
    update.addBindValue(now);
    update.addBindValue(now);
    update.addBindValue(noticeId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        if (errorMessage && errorMessage->isEmpty()) {
            setError(errorMessage, QStringLiteral("送检通知已被其他操作改变，请刷新后重试。"));
        }
        rollback();
        return false;
    }
    QSqlQuery syncInbound(m_database);
    syncInbound.prepare(QStringLiteral(
        "UPDATE inbound_inspection_details SET inspection_date=?,inspector_name=?,"
        "inspection_result=?,conclusion=?,inspection_attachment_id=? WHERE inspection_notice_id=?"));
    syncInbound.addBindValue(result.inspectionDate.toString(Qt::ISODate));
    syncInbound.addBindValue(textValue(result.inspectorName));
    syncInbound.addBindValue(normalizedResult);
    syncInbound.addBindValue(textValue(result.conclusion));
    syncInbound.addBindValue(attachmentId > 0 ? QVariant(attachmentId) : QVariant());
    syncInbound.addBindValue(noticeId);
    if (!syncInbound.exec()
        || !writeAudit(QStringLiteral("INSPECTION_RESULT_RECORD"), noticeId,
                       QStringLiteral("%1 / %2").arg(status, normalizedResult), errorMessage)
        || !commit(errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) {
            setError(errorMessage, QStringLiteral("送检通知已被其他操作改变，请刷新后重试。"));
        }
        rollback();
        return false;
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

bool InspectionService::cancelNotice(qlonglong noticeId, QString *errorMessage)
{
    if (noticeId <= 0 || m_operatorId <= 0 || !beginImmediate(errorMessage)) return false;
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE inspection_notices SET status='CANCELLED',updated_by=?,updated_at=? "
        "WHERE id=? AND status IN ('PENDING','QUALIFIED','UNQUALIFIED') "
        "AND linked_document_id IS NULL"));
    update.addBindValue(m_operatorId);
    update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    update.addBindValue(noticeId);
    if (!update.exec() || update.numRowsAffected() != 1
        || !writeAudit(QStringLiteral("INSPECTION_NOTICE_CANCEL"), noticeId, QString(), errorMessage)
        || !commit(errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) {
            setError(errorMessage, QStringLiteral("送检通知不存在、已取消或已经用于入库。"));
        }
        rollback();
        return false;
    }
    return true;
}
