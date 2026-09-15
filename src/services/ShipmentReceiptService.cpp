#include "services/ShipmentReceiptService.h"

#include "services/AttachmentService.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <utility>

namespace {
void setReceiptError(QString *target, const QString &message)
{
    if (target) *target = message;
}
}

ShipmentReceiptService::ShipmentReceiptService(QSqlDatabase database, qlonglong operatorId)
    : m_database(std::move(database)), m_operatorId(operatorId)
{
}

bool ShipmentReceiptService::confirmSigned(qlonglong documentId,
                                           const QDate &receiptDate,
                                           const QString &fileName,
                                           const QString &mimeType,
                                           const QByteArray &fileData,
                                           qlonglong *attachmentId,
                                           QString *errorMessage)
{
    const QString cleanName = QFileInfo(fileName.trimmed()).fileName();
    if (!m_database.isOpen() || m_operatorId <= 0 || documentId <= 0) {
        setReceiptError(errorMessage, QStringLiteral("发货单或当前用户无效。"));
        return false;
    }
    if (!receiptDate.isValid()) {
        setReceiptError(errorMessage, QStringLiteral("请选择有效的签收日期。"));
        return false;
    }
    if (cleanName.isEmpty() || fileData.isEmpty()) {
        setReceiptError(errorMessage, QStringLiteral("改为已签收前必须上传收货单。"));
        return false;
    }
    if (fileData.size() > AttachmentService::MaximumAttachmentBytes) {
        setReceiptError(errorMessage, QStringLiteral("收货单附件不能超过 50 MB。"));
        return false;
    }

    QSqlQuery current(m_database);
    current.prepare(QStringLiteral(
        "SELECT d.document_no,COALESCE(s.receipt_attachment_id,0),"
        "COALESCE(s.receipt_status,'PENDING') "
        "FROM business_documents d "
        "JOIN sales_outbound_details s ON s.document_id=d.id "
        "WHERE d.id=? AND d.document_type='XSCK'"));
    current.addBindValue(documentId);
    if (!current.exec() || !current.next()) {
        setReceiptError(errorMessage, QStringLiteral("销售出库单不存在或读取失败：%1")
                                          .arg(current.lastError().text()));
        return false;
    }
    const QString documentNumber = current.value(0).toString();
    const qlonglong oldAttachmentId = current.value(1).toLongLong();
    const bool replacing = current.value(2).toString() == QStringLiteral("SIGNED");

    if (!m_database.transaction()) {
        setReceiptError(errorMessage, QStringLiteral("无法开始签收事务：%1")
                                          .arg(m_database.lastError().text()));
        return false;
    }

    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO attachments(business_type,business_id,original_file_name,mime_type,"
        "file_size,sha256,file_data,uploaded_by) "
        "VALUES('business_document',?,?,?,?,?,?,?)"));
    insert.addBindValue(documentId);
    insert.addBindValue(cleanName);
    insert.addBindValue(mimeType.trimmed());
    insert.addBindValue(fileData.size());
    insert.addBindValue(QString::fromLatin1(
        QCryptographicHash::hash(fileData, QCryptographicHash::Sha256).toHex()));
    insert.addBindValue(fileData);
    insert.addBindValue(m_operatorId);
    if (!insert.exec()) {
        m_database.rollback();
        setReceiptError(errorMessage, QStringLiteral("保存收货单失败：%1")
                                          .arg(insert.lastError().text()));
        return false;
    }
    const qlonglong newAttachmentId = insert.lastInsertId().toLongLong();
    const QString confirmedAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);

    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE sales_outbound_details SET receipt_status='SIGNED',receipt_date=?,"
        "receipt_attachment_id=?,receipt_confirmed_by=?,receipt_confirmed_at=? "
        "WHERE document_id=?"));
    update.addBindValue(receiptDate.toString(Qt::ISODate));
    update.addBindValue(newAttachmentId);
    update.addBindValue(m_operatorId);
    update.addBindValue(confirmedAt);
    update.addBindValue(documentId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        m_database.rollback();
        setReceiptError(errorMessage, QStringLiteral("更新签收状态失败：%1")
                                          .arg(update.lastError().text()));
        return false;
    }

    if (oldAttachmentId > 0 && oldAttachmentId != newAttachmentId) {
        QSqlQuery removeOld(m_database);
        removeOld.prepare(QStringLiteral(
            "UPDATE attachments SET is_deleted=1,deleted_by=?,deleted_at=? "
            "WHERE id=? AND is_deleted=0"));
        removeOld.addBindValue(m_operatorId);
        removeOld.addBindValue(confirmedAt);
        removeOld.addBindValue(oldAttachmentId);
        if (!removeOld.exec()) {
            m_database.rollback();
            setReceiptError(errorMessage, QStringLiteral("替换旧收货单失败：%1")
                                              .arg(removeOld.lastError().text()));
            return false;
        }
    }

    QSqlQuery audit(m_database);
    audit.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
        "VALUES(?,?, 'business_document',?,?)"));
    audit.addBindValue(m_operatorId);
    audit.addBindValue(replacing ? QStringLiteral("SHIPMENT_RECEIPT_REPLACE")
                                 : QStringLiteral("SHIPMENT_RECEIPT_CONFIRM"));
    audit.addBindValue(documentId);
    audit.addBindValue(QStringLiteral("%1 / 已签收 / %2 / %3")
                           .arg(documentNumber, receiptDate.toString(Qt::ISODate), cleanName));
    if (!audit.exec() || !m_database.commit()) {
        const QString detail = audit.lastError().text().isEmpty()
                                   ? m_database.lastError().text() : audit.lastError().text();
        m_database.rollback();
        setReceiptError(errorMessage, QStringLiteral("提交签收记录失败：%1").arg(detail));
        return false;
    }
    if (attachmentId) *attachmentId = newAttachmentId;
    return true;
}
