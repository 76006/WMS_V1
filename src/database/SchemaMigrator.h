#pragma once

#include <QSqlDatabase>

class SchemaMigrator
{
public:
    static bool migrate(QSqlDatabase database, QString *errorMessage = nullptr);

private:
    static bool executeSchema(QSqlDatabase database, QString *errorMessage);
    static bool ensureDefaultAdministrator(QSqlDatabase database, QString *errorMessage);
};
