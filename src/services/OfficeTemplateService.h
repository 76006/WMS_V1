#pragma once

#include "services/InventoryService.h"

#include <QDate>
#include <QList>
#include <QMap>
#include <QSqlDatabase>
#include <QString>

class QWidget;

enum class OfficeFormKind
{
    Inspection,
    RawMaterialInbound,
    ProductionIssue,
    FinishedGoodsInbound,
    StockOutbound,
    DeliveryConfirmation
};

struct OfficeTemplateLine
{
    QString materialCode;
    QString materialName;
    QString specification;
    QString unit;
    QString batchNo;
    QString serialNumbers;
    QString notes;
    QString orderNumber;
    QString supplier;
    double quantity = 0.0;
    double unitUsage = 0.0;
    double externalQuantity = 0.0;
    double reworkQuantity = 0.0;
    double lossQuantity = 0.0;
    double returnQuantity = 0.0;
};

struct OfficeTemplateDocument
{
    OfficeFormKind kind = OfficeFormKind::StockOutbound;
    QString documentNumber;
    QDate documentDate;
    QMap<QString, QString> fields;
    QList<OfficeTemplateLine> lines;
};

class OfficeTemplateService
{
public:
    static QString formTitle(OfficeFormKind kind);
    static QString templateFileName(OfficeFormKind kind);
    static QString outputFileName(const OfficeTemplateDocument &document);
    static QString archiveRootPath();

    static QList<OfficeTemplateLine> materialLines(
        QSqlDatabase database,
        const QList<StockMovementRequest> &lines,
        QString *errorMessage = nullptr);

    static bool renderToFile(const OfficeTemplateDocument &document,
                             const QString &outputPath,
                             QString *errorMessage = nullptr);
    static bool openPreview(const OfficeTemplateDocument &document,
                            QWidget *parent = nullptr);
    static bool attachToDocument(const OfficeTemplateDocument &document,
                                 QSqlDatabase database,
                                 qlonglong operatorId,
                                 qlonglong businessDocumentId,
                                 QString *errorMessage = nullptr);

private:
    static QString findTemplateFile(OfficeFormKind kind);
};
