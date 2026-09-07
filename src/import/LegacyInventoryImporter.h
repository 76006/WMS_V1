#pragma once

#include <QList>
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
