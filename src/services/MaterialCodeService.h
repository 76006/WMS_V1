#pragma once

#include <QSqlDatabase>
#include <QString>

class MaterialCodeService
{
public:
    static QString normalizeProjectCode(const QString &projectCode);
    static bool isValidProjectCode(const QString &projectCode);
    static bool isValidMaterialType(const QString &materialType);
    static bool isValidDisciplineCode(const QString &disciplineCode);
    static QString nextCode(QSqlDatabase database,
                            const QString &materialType,
                            const QString &projectCode,
                            const QString &disciplineCode,
                            QString *errorMessage = nullptr);
};
