#pragma once

#include <QList>
#include <QString>
#include <QVariant>

class XlsxExporter
{
public:
    struct SheetData {
        QString name;
        QStringList headers;
        QList<QList<QVariant>> rows;
    };

    static bool writeSingleSheet(const QString &filePath,
                                 const QString &sheetName,
                                 const QStringList &headers,
                                 const QList<QList<QVariant>> &rows,
                                 QString *errorMessage = nullptr);
    static bool writeWorkbook(const QString &filePath,
                              const QList<SheetData> &sheets,
                              QString *errorMessage = nullptr);
};
