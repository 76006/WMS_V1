#pragma once

#include <QSqlDatabase>
#include <QTemporaryDir>

class LegacySource
{
public:
    LegacySource() = default;
    ~LegacySource();
    Q_DISABLE_COPY(LegacySource)

    bool prepare(const QString &sourceFile, const QString &product, QString *error);
    QSqlDatabase database() const { return m_database; }

private:
    QTemporaryDir m_directory;
    QSqlDatabase m_database;
};
