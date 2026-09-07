#include "services/AttachmentService.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <utility>

namespace {
void setAttachmentError(QString *target, const QString &message)
{
    if (target) *target = message;
}
}

AttachmentService::AttachmentService(QSqlDatabase database, qlonglong operatorId)
    : m_database(std::move(database)), m_operatorId(operatorId)
{
}

bool AttachmentService::uploadDocumentAttachment(qlonglong documentId,
                                                 const QString &fileName,
                                                 const QString &mimeType,
                                                 const QByteArray &data,
                                                 qlonglong *attachmentId,
                                                 QString *errorMessage)
{
    const QString cleanName = fileName.trimmed();
    if (!m_database.isOpen() || m_operatorId <= 0 || documentId <= 0
        || cleanName.isEmpty() || data.isEmpty()) {
        setAttachmentError(errorMessage, QStringLiteral("业务单据、文件或当前用户无效。"));
        return false;
    }
    if (data.size() > MaximumAttachmentBytes) {
        setAttachmentError(errorMessage, QStringLiteral("单个附件不能超过50 MB。"));
        return false;
    }
    QSqlQuery document(m_database);
    document.prepare(QStringLiteral("SELECT document_no FROM business_documents WHERE id=?"));
    document.addBindValue(documentId);
    if (!document.exec() || !document.next()) {
        setAttachmentError(errorMessage, QStringLiteral("所选业务单据不存在。"));
        return false;
    }
    const QString documentNumber = document.value(0).toString();
    if (!m_database.transaction()) {
        setAttachmentError(errorMessage, QStringLiteral("无法开始附件事务：%1")
                                             .arg(m_database.lastError().text()));
        return false;
    }
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO attachments(business_type,business_id,original_file_name,mime_type,"
        "file_size,sha256,file_data,uploaded_by) VALUES('business_document',?,?,?,?,?,?,?)"));
    insert.addBindValue(documentId);
    insert.addBindValue(cleanName);
    insert.addBindValue(mimeType.trimmed());
    insert.addBindValue(data.size());
    insert.addBindValue(QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
    insert.addBindValue(data);
    insert.addBindValue(m_operatorId);
    if (!insert.exec()) {
        m_database.rollback();
        setAttachmentError(errorMessage, QStringLiteral("保存附件失败：%1")
                                             .arg(insert.lastError().text()));
        return false;
    }
    const qlonglong id = insert.lastInsertId().toLongLong();
    if (!writeAudit(QStringLiteral("ATTACHMENT_UPLOAD"), id,
                    QStringLiteral("%1 / %2").arg(documentNumber, cleanName), errorMessage)
        || !m_database.commit()) {
        if (errorMessage && errorMessage->isEmpty())
            *errorMessage = QStringLiteral("提交附件事务失败：%1").arg(m_database.lastError().text());
        m_database.rollback();
        return false;
    }
    if (attachmentId) *attachmentId = id;
    return true;
}

bool AttachmentService::loadAttachment(qlonglong attachmentId,
                                       AttachmentPayload *payload,
                                       QString *errorMessage) const
{
    if (!payload || attachmentId <= 0) {
        setAttachmentError(errorMessage, QStringLiteral("附件标识无效。"));
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id,original_file_name,mime_type,file_data,is_deleted FROM attachments WHERE id=?"));
    query.addBindValue(attachmentId);
    if (!query.exec() || !query.next()) {
        setAttachmentError(errorMessage, QStringLiteral("附件不存在。"));
        return false;
    }
    payload->id = query.value(0).toLongLong();
    payload->fileName = query.value(1).toString();
    payload->mimeType = query.value(2).toString();
    payload->data = query.value(3).toByteArray();
    payload->deleted = query.value(4).toBool();
    return true;
}

bool AttachmentService::setDeleted(qlonglong attachmentId,
                                   bool deleted,
                                   QString *errorMessage)
{
    if (!m_database.isOpen() || m_operatorId <= 0 || attachmentId <= 0) {
        setAttachmentError(errorMessage, QStringLiteral("附件或当前用户无效。"));
        return false;
    }
    if (!m_database.transaction()) {
        setAttachmentError(errorMessage, m_database.lastError().text());
        return false;
    }
    QSqlQuery update(m_database);
    if (deleted) {
        update.prepare(QStringLiteral(
            "UPDATE attachments SET is_deleted=1,deleted_by=?,deleted_at=? WHERE id=? AND is_deleted=0"));
        update.addBindValue(m_operatorId);
        update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    } else {
        update.prepare(QStringLiteral(
            "UPDATE attachments SET is_deleted=0,deleted_by=NULL,deleted_at=NULL WHERE id=? AND is_deleted=1"));
    }
    update.addBindValue(attachmentId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        m_database.rollback();
        setAttachmentError(errorMessage, QStringLiteral("附件状态没有变化，可能已被其他操作更新。"));
        return false;
    }
    if (!writeAudit(deleted ? QStringLiteral("ATTACHMENT_DELETE")
                            : QStringLiteral("ATTACHMENT_RESTORE"),
                    attachmentId, QString(), errorMessage)
        || !m_database.commit()) {
        if (errorMessage && errorMessage->isEmpty()) *errorMessage = m_database.lastError().text();
        m_database.rollback();
        return false;
    }
    return true;
}

bool AttachmentService::writeAudit(const QString &action, qlonglong attachmentId,
                                   const QString &detail, QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
        "VALUES(?,?,'attachment',?,?)"));
    query.addBindValue(m_operatorId);
    query.addBindValue(action);
    query.addBindValue(attachmentId);
    query.addBindValue(detail.isNull() ? QString::fromLatin1("", 0) : detail);
    if (!query.exec()) {
        setAttachmentError(errorMessage, QStringLiteral("记录附件操作日志失败：%1")
                                             .arg(query.lastError().text()));
        return false;
    }
    return true;
}
