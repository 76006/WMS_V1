#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QString>

enum class LegacyImportStatus
{
    Ready,
    Warning,
    Error,
    Skipped
};

struct LegacyImportRow
{
    LegacyImportStatus status = LegacyImportStatus::Skipped;
    QString sourceSheet;
    int sourceRow = 0;
    QString materialCode;
    QString materialName;
    QString specification;
    QString categoryCode;
    QString unit = QStringLiteral("个");
    QString batchNo;
    double quantity = 0.0;
    QString rawQuantity;
    QString message;
};

class LegacyInventoryImporter
{
public:
    static bool parseFile(const QString &filePath,
                          QList<LegacyImportRow> *rows,
                          QString *errorMessage = nullptr);
    static QString statusText(LegacyImportStatus status);
};

enum class MaterialImportStatus
{
    Ready,
    Warning,
    Error,
    Skipped
};

struct MaterialImportRow
{
    MaterialImportStatus status = MaterialImportStatus::Ready;
    int sourceRow = 0;
    QString materialCode;
    QString materialName;
    QString specification;
    QString categoryCode;
    QString categoryName;
    QString processingMethod;
    bool processingMethodProvided = false;
    QString unit;
    double unitUsage = 0.0;
    bool unitUsageProvided = false;
    double currentStock = 0.0;
    bool currentStockProvided = false;
    bool importCurrentStock = false;
    double minimumStock = 0.0;
    QString defaultWarehouseCode;
    QString defaultLocationCode;
    bool requireBatch = false;
    bool requireSerial = false;
    QString inventoryBatch;
    QStringList inventorySerialNumbers;
    bool isActive = true;
    bool isActiveProvided = false;
    QString brand;
    QString notes;
    QString message;
};

class MaterialExcelImporter
{
public:
    static bool parseFile(const QString &filePath,
                          QList<MaterialImportRow> *rows,
                          QString *errorMessage = nullptr);
    static void validateReferences(QSqlDatabase database,
                                   QList<MaterialImportRow> *rows);
    static bool importRows(QSqlDatabase database,
                           qlonglong operatorId,
                           const QList<MaterialImportRow> &rows,
                           int *createdCount,
                           int *updatedCount,
                           QString *errorMessage = nullptr);
    // 在调用方已开启的事务内导入物料：本函数绝不开始、提交或回滚事务，
    // 失败时只返回 false 并报告错误，事务处理完全由调用方负责。
    static bool importRowsInCurrentTransaction(QSqlDatabase database,
                                               qlonglong operatorId,
                                               const QList<MaterialImportRow> &rows,
                                               int *createdCount,
                                               int *updatedCount,
                                               QString *errorMessage = nullptr);
    static QString statusText(MaterialImportStatus status);
};

struct BomImportRow
{
    int sourceRow = 0;
    int level = 0;
    int parentIndex = -1;
    QString materialCode;
    QString materialName;
    QString specification;
    QString categoryCode;
    QString unit;
    double quantity = 1.0;
    QString processingMethod;
};

struct BomImportResult
{
    QString sourceSheet;
    QString productCode;
    QList<BomImportRow> rows;
};

class BomExcelImporter
{
public:
    static bool parseFile(const QString &filePath,
                          BomImportResult *result,
                          QString *errorMessage = nullptr);
    static bool importRows(QSqlDatabase database,
                           qlonglong operatorId,
                           const BomImportResult &result,
                           int *createdMaterialCount,
                           int *updatedMaterialCount,
                           QString *errorMessage = nullptr);
};

struct SpreadsheetPreviewSheet
{
    QString name;
    QList<QStringList> rows;
};

class OfficePreviewExtractor
{
public:
    static bool previewXlsx(const QString &filePath,
                            QList<SpreadsheetPreviewSheet> *sheets,
                            QString *errorMessage = nullptr);
    static bool previewDocx(const QString &filePath,
                            QString *text,
                            QString *errorMessage = nullptr);
};
