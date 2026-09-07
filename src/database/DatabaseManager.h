#pragma once

#include "core/DatabaseConfig.h"

#include <QSqlDatabase>

class DatabaseManager
{
public:
    DatabaseManager();
    ~DatabaseManager();

    bool open(const DatabaseConfig &config, QString *errorMessage = nullptr);
    void close();
    bool isOpen() const;
    QSqlDatabase database() const;
    QString databaseFilePath() const;

private:
    static constexpr const char *ConnectionName = "wms_main";
    QSqlDatabase m_database;
    QString m_databaseFilePath;
};

