#pragma once

#include <QByteArray>
#include <QDate>
#include <QSqlDatabase>
#include <QString>

class ShipmentReceiptService
{
public:
    ShipmentReceiptService(QSqlDatabase database, qlonglong operatorId);

    bool confirmSigned(qlonglong documentId,
                       const QDate &receiptDate,
                       const QString &fileName,
                       const QString &mimeType,
                       const QByteArray &fileData,
                       qlonglong *attachmentId = nullptr,
                       QString *errorMessage = nullptr);

private:
    QSqlDatabase m_database;
    qlonglong m_operatorId = 0;
};
