#pragma once

#include <QSqlDatabase>

class SchemaMigrator
{
public:
    static bool migrate(const QSqlDatabase &database, QString *errorMessage = nullptr);

private:
    static bool executeSchema(const QSqlDatabase &database, QString *errorMessage);
    static bool ensureDefaultAdministrator(const QSqlDatabase &database, QString *errorMessage);
};

