#pragma once

#include <QDate>
#include <QList>
#include <QSqlDatabase>
#include <QString>

struct AnnualInventoryReportRow
{
    int month = 0;
    double orderedQuantity = 0.0;
    double purchaseReceivedQuantity = 0.0;
    double giftQuantity = 0.0;
    double billableQuantity = 0.0;
    double inboundQuantity = 0.0;
    double outboundQuantity = 0.0;
};

struct MonthlyMaterialReportRow
{
    qlonglong materialId = 0;
    QString materialCode;
    QString materialName;
    QString specification;
    QString unit;
    double openingQuantity = 0.0;
    double orderedQuantity = 0.0;
    double purchaseReceivedQuantity = 0.0;
    double giftQuantity = 0.0;
    double billableQuantity = 0.0;
    double otherInboundQuantity = 0.0;
    double inboundQuantity = 0.0;
    double outboundQuantity = 0.0;
    double closingQuantity = 0.0;
};

struct InventoryMovementReportRow
{
    QDate documentDate;
    QString documentNumber;
    QString documentType;
    QString direction;
    QString materialCode;
    QString materialName;
    QString specification;
    QString unit;
    QString supplier;
    QString warehouse;
    QString location;
    QString batchNumber;
    double orderedQuantity = 0.0;
    double purchaseReceivedQuantity = 0.0;
    double giftQuantity = 0.0;
    double billableQuantity = 0.0;
    double inboundQuantity = 0.0;
    double outboundQuantity = 0.0;
    QString handlerName;
    QString operatorName;
    QString notes;
};

class InventoryReportService
{
public:
    explicit InventoryReportService(QSqlDatabase database);

    bool loadAnnual(int year,
                    const QString &keyword,
                    QList<AnnualInventoryReportRow> *rows,
                    QString *errorMessage = nullptr) const;
    bool loadMonthly(int year,
                     int month,
                     const QString &keyword,
                     QList<MonthlyMaterialReportRow> *summaryRows,
                     QList<InventoryMovementReportRow> *detailRows,
                     QString *errorMessage = nullptr) const;

    static QString documentTypeName(const QString &documentType);
    static QString directionName(const QString &direction);

private:
    QSqlDatabase m_database;
};
