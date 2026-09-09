#pragma once

#include <QByteArray>
#include <QDate>
#include <QList>
#include <QSqlDatabase>
#include <QStringList>
#include <QVariant>

struct StockMovementRequest
{
    QString documentType;
    QDate documentDate;
    QString handlerName;
    QString purpose;
    QString supplier;
    QString notes;
    qlonglong materialId = 0;
    double orderedQuantity = 0.0;
    double quantity = 0.0;
    double giftQuantity = 0.0;
    QString batchNo;
    qlonglong warehouseId = 0;
    qlonglong locationId = 0;
    QStringList serialNumbers;
};

struct InboundInspectionRequest
{
    bool required = false;
    QString inspectionNumber;
    QDate inspectionDate;
    QString inspectorName;
    QString result = QStringLiteral("NOT_REQUIRED");
    QString conclusion;
    QString attachmentFileName;
    QString attachmentMimeType;
    QByteArray attachmentData;
};

struct StockDocumentRequest
{
    QString documentType;
    QDate documentDate;
    QString handlerName;
    QString purpose;
    QString supplier;
    QString customerCompany;
    QString destination;
    QString customerContact;
    QString customerPhone;
    QString salesOrderNumber;
    QString logisticsCompany;
    QString trackingNumber;
    QString notes;
    QString submissionToken;
    qlonglong productionRunId = 0;
    InboundInspectionRequest inspection;
    QList<StockMovementRequest> lines;
};

struct ProductionRunRequest
{
    QString batchNo;
    qlonglong productMaterialId = 0;
    double plannedQuantity = 0.0;
};

struct ProductionReturnLine
{
    qlonglong sourceItemId = 0;
    double quantity = 0.0;
    qlonglong warehouseId = 0;
    qlonglong locationId = 0;
    QStringList serialNumbers;
    QString notes;
};

struct ProductionReturnRequest
{
    qlonglong sourceDocumentId = 0;
    QDate documentDate;
    QString handlerName;
    QString notes;
    QString submissionToken;
    QList<ProductionReturnLine> lines;
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
    double giftQuantity = 0.0;
    QString handlerName;
    QString notes;
    QStringList serialNumbers;
};

struct PostedDocument
{
    qlonglong documentId = 0;
    QString documentNumber;
};

struct InitialInventoryLine
{
    QString materialCode;
    QString materialName;
    QString specification;
    QString categoryCode;
    QString unit = QStringLiteral("个");
    QString batchNo;
    double quantity = 0.0;
    QStringList serialNumbers;
    QString notes;
};

struct InitialInventoryRequest
{
    QDate documentDate;
    QString handlerName;
    QString sourceFile;
    QString submissionToken;
    qlonglong warehouseId = 0;
    qlonglong locationId = 0;
    QList<InitialInventoryLine> lines;
};

struct InventoryCountLine
{
    qlonglong materialId = 0;
    qlonglong warehouseId = 0;
    qlonglong locationId = 0;
    QString batchNo;
    double systemQuantity = 0.0;
    double actualQuantity = 0.0;
    QString differenceReason;
};

struct InventoryCountRequest
{
    QDate documentDate;
    QString handlerName;
    QString notes;
    QString submissionToken;
    QList<InventoryCountLine> lines;
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
    bool postStockDocument(const StockDocumentRequest &request,
                           bool inbound,
                           PostedDocument *postedDocument,
                           QString *errorMessage = nullptr);
    bool importInitialInventory(const InitialInventoryRequest &request,
                                PostedDocument *postedDocument,
                                QString *errorMessage = nullptr);
    bool postInventoryCount(const InventoryCountRequest &request,
                            PostedDocument *postedDocument,
                            QString *errorMessage = nullptr);
    bool postTransfer(const TransferRequest &request,
                      PostedDocument *postedDocument,
                      QString *errorMessage = nullptr);
    bool reverseTransfer(const ReversalRequest &request,
                         PostedDocument *postedDocument,
                         QString *errorMessage = nullptr);
    bool reverseItem(const ReversalRequest &request,
                     PostedDocument *postedDocument,
                     QString *errorMessage = nullptr);

    bool postProductionIssue(const ProductionRunRequest &run,
                             const StockDocumentRequest &document,
                             PostedDocument *postedDocument,
                             qlonglong *productionRunId = nullptr,
                             QString *errorMessage = nullptr);
    bool postProductionReturn(const ProductionReturnRequest &request,
                              PostedDocument *postedDocument,
                              QString *errorMessage = nullptr);
    bool postFinishedGoodsInbound(const StockDocumentRequest &document,
                                  PostedDocument *postedDocument,
                                  QString *errorMessage = nullptr);
    double productionRunReceivedQuantity(qlonglong productionRunId,
                                         QString *errorMessage = nullptr) const;

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
    qlonglong createDocumentItem(qlonglong documentId,
                                 int lineNumber,
                                 const StockMovementRequest &request,
                                 const QVariant &sourceItemId,
                                 QString *errorMessage);
    bool setDocumentProductionContext(qlonglong documentId,
                                      qlonglong productionRunId,
                                      const QString &submissionToken,
                                      QString *errorMessage);
    bool setDocumentSubmissionToken(qlonglong documentId,
                                    const QString &submissionToken,
                                    QString *errorMessage);
    qlonglong ensureProductionRun(const ProductionRunRequest &run, QString *errorMessage);
    bool refreshProductionRunStatus(qlonglong productionRunId, QString *errorMessage);
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
