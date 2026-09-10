#pragma once

#include <QByteArray>
#include <QDate>
#include <QList>
#include <QMap>
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
    QMap<QString, QString> templateFields;
};

struct StockDocumentRequest
{
    QString documentType;
    QDate documentDate;
    // 销售出库的送货日期：与出库日期分开保存，并用于送货确认单模板。
    QDate deliveryDate;
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
    // 独立送检通知主键。大于0时以数据库中的合格通知为唯一送检依据。
    qlonglong inspectionNoticeId = 0;
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

// 已入账业务单据的完整可编辑快照。界面保存时必须整份提交，服务层会在同一事务中
// 替换单头、明细、库存流水和SN关联，并重算库存，避免只修改显示资料而库存未同步。
struct PostedDocumentEditLine
{
    qlonglong itemId = 0;
    qlonglong materialId = 0;
    double quantity = 0.0;
    double orderedQuantity = 0.0;
    double giftQuantity = 0.0;
    QString batchNo;
    qlonglong warehouseId = 0;
    qlonglong locationId = 0;
    qlonglong targetWarehouseId = 0;
    qlonglong targetLocationId = 0;
    qlonglong sourceItemId = 0;
    // 普通单据由单头库存方向决定；盘点等 ADJUST 单据按行保存 IN 或 OUT。
    QString movementDirection;
    QStringList serialNumbers;
    QString notes;
};

struct PostedDocumentEdit
{
    qlonglong documentId = 0;
    QString documentNumber;
    QString documentType;
    QString stockDirection;
    QString status;
    QDate documentDate;
    QDate deliveryDate;
    qlonglong sourceDocumentId = 0;
    qlonglong productionRunId = 0;
    qlonglong inspectionNoticeId = 0;
    QString handlerName;
    QString purpose;
    QString supplier;
    QString notes;
    QString customerCompany;
    QString destination;
    QString customerContact;
    QString customerPhone;
    QString salesOrderNumber;
    QString logisticsCompany;
    QString trackingNumber;
    QList<PostedDocumentEditLine> lines;
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
    QString supplier;
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
    bool loadPostedDocument(qlonglong documentId,
                            PostedDocumentEdit *document,
                            QString *errorMessage = nullptr) const;
    bool revisePostedDocument(const PostedDocumentEdit &document,
                              QString *errorMessage = nullptr);
    bool importInitialInventory(const InitialInventoryRequest &request,
                                PostedDocument *postedDocument,
                                QString *errorMessage = nullptr);
    // 在调用方已开启的事务内导入期初库存：本函数绝不开始、提交或回滚事务，
    // 任何校验、数据库或收尾错误都只返回 false，由调用方决定是否回滚。
    bool importInitialInventoryInCurrentTransaction(const InitialInventoryRequest &request,
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
    // 返回原业务明细当前可撤销的SN（去重、稳定排序），规则与 reverseItem 完全一致；
    // 明细不存在、非SN管理物料、方向无效或原单为撤销单时返回空并写入 errorMessage。
    QStringList reversibleSerialNumbers(qlonglong sourceItemId,
                                        QString *errorMessage = nullptr) const;
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
    // 期初库存导入的共享实现：manageTransaction 为 true 时自行开始/提交/回滚事务，
    // 为 false 时事务完全归调用方所有，本实现绝不开始、提交或回滚。
    bool importInitialInventoryInternal(const InitialInventoryRequest &request,
                                        PostedDocument *postedDocument,
                                        bool manageTransaction,
                                        QString *errorMessage);

    QSqlDatabase m_database;
    qlonglong m_operatorId = 0;
};
