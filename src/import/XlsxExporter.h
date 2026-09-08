#pragma once

#include <QList>
#include <QString>
#include <QVariant>

class XlsxExporter
{
public:
    static bool writeSingleSheet(const QString &filePath,
                                 const QString &sheetName,
                                 const QStringList &headers,
                                 const QList<QList<QVariant>> &rows,
                                 QString *errorMessage = nullptr);
};
