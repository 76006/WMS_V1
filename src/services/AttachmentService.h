#pragma once

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>

struct AttachmentPayload
{
    qlonglong id = 0;
    QString fileName;
    QString mimeType;
    QByteArray data;
    bool deleted = false;
};

class AttachmentService
{
public:
    static constexpr qint64 MaximumAttachmentBytes = 50LL * 1024LL * 1024LL;

    AttachmentService(QSqlDatabase database, qlonglong operatorId);

    bool uploadDocumentAttachment(qlonglong documentId,
                                  const QString &fileName,
                                  const QString &mimeType,
                                  const QByteArray &data,
                                  qlonglong *attachmentId = nullptr,
                                  QString *errorMessage = nullptr);
    bool loadAttachment(qlonglong attachmentId,
                        AttachmentPayload *payload,
                        QString *errorMessage = nullptr) const;
    bool setDeleted(qlonglong attachmentId,
                    bool deleted,
                    QString *errorMessage = nullptr);

private:
    bool writeAudit(const QString &action, qlonglong attachmentId,
                    const QString &detail, QString *errorMessage);

    QSqlDatabase m_database;
    qlonglong m_operatorId = 0;
};
