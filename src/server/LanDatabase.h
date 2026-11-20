#pragma once

#include <QSqlDatabase>
#include <QString>

// Phase one owns a separate PostgreSQL schema; it never opens a desktop SQLite file.
class LanDatabase
{
public:
    explicit LanDatabase(QString product);
    bool open(QString *error);
    bool initialize(const QString &password, bool includeDemoStock, QString *error);
    bool verify(QString *error);
    bool ready();
    QSqlDatabase connection() const { return m_db; }
    QString product() const { return m_product; }
private:
    QSqlDatabase m_db;
    QString m_product;
};
