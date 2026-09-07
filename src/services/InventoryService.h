#pragma once

#include <QDate>
#include <QSqlDatabase>
#include <QStringList>
#include <QVariant>

struct StockMovementRequest
{
    QString documentType;
    QDate documentDate;
    QString handlerName;
    QString purpose;
    QString notes;
    qlonglong materialId = 0;
    double quantity = 0.0;
    QString batchNo;
    qlonglong warehouseId = 0;
    qlonglong locationId = 0;
    QStringList serialNumbers;
};

struct TransferRequest : public StockMovementRequest
{
    qlonglong targetWarehouseId = 0;
    qlonglong targetLocationId = 0;
};

struct ReversalRequest
{
    qlonglong sourceItemId = 0;
    QDate documentDate;
    double quantity = 0.0;
    QString handlerName;
    QString notes;
    QStringList serialNumbers;
};

struct PostedDocument
{
    qlonglong documentId = 0;
    QString documentNumber;
};

class InventoryService
{
public:
    InventoryService(QSqlDatabase database, qlonglong operatorId);

    bool postInbound(const StockMovementRequest &request,
                     PostedDocument *postedDocument,
                     QString *errorMessage = nullptr);
    bool postOutbound(const StockMovementRequest &request,
                      PostedDocument *postedDocument,
                      QString *errorMessage = nullptr);
    bool postTransfer(const TransferRequest &request,
                      PostedDocument *postedDocument,
                      QString *errorMessage = nullptr);
    bool reverseItem(const ReversalRequest &request,
                     PostedDocument *postedDocument,
                     QString *errorMessage = nullptr);

    QStringList previewSerialNumbers(qlonglong materialId,
                                     const QString &prefix,
                                     int count,
                                     QString *errorMessage = nullptr) const;

private:
    struct MaterialRules {
        bool valid = false;
        bool requireBatch = false;
        bool requireSerial = false;
        QString code;
    };

    bool postMovement(const StockMovementRequest &request,
                      bool inbound,
                      PostedDocument *postedDocument,
                      QString *errorMessage);
    bool beginImmediate(QString *errorMessage);
    void rollback();
    bool commit(QString *errorMessage);
    MaterialRules materialRules(qlonglong materialId, QString *errorMessage) const;
    bool validateLocation(qlonglong warehouseId, qlonglong locationId, QString *errorMessage) const;
    bool validateMovement(const StockMovementRequest &request,
                          const MaterialRules &rules,
                          QString *errorMessage) const;
    QString nextDocumentNumber(const QString &documentType,
                               const QDate &documentDate,
                               QString *errorMessage);
    qlonglong createDocument(const QString &number,
                             const QString &type,
                             const QString &direction,
                             const QDate &date,
                             const QString &handler,
                             const QString &purpose,
                             const QString &notes,
                             const QVariant &sourceDocumentId,
                             QString *errorMessage);
    qlonglong createItem(qlonglong documentId,
                         const StockMovementRequest &request,
                         qlonglong targetWarehouseId,
                         qlonglong targetLocationId,
                         QString *errorMessage);
    bool changeBalance(qlonglong materialId,
                       qlonglong warehouseId,
                       qlonglong locationId,
                       const QString &batchNo,
                       double delta,
                       double *quantityBefore,
                       double *quantityAfter,
                       QString *errorMessage);
    qlonglong createLedger(qlonglong documentId,
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
                           QString *errorMessage);
    bool attachSerialsToInbound(const StockMovementRequest &request,
                                qlonglong documentId,
                                qlonglong ledgerId,
                                QString *errorMessage);
    bool attachSerialsToOutbound(const StockMovementRequest &request,
                                 qlonglong documentId,
                                 qlonglong ledgerId,
                                 QString *errorMessage);
    bool linkLedgerSerial(qlonglong ledgerId, qlonglong serialId, QString *errorMessage);
    bool finalizeDocument(qlonglong documentId, QString *errorMessage);
    bool writeAudit(const QString &action,
                    const QString &entityType,
                    qlonglong entityId,
                    const QString &detail,
                    QString *errorMessage);

    QSqlDatabase m_database;
    qlonglong m_operatorId = 0;
};
