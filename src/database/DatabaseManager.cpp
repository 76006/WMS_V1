#include "database/DatabaseManager.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>

DatabaseManager::DatabaseManager() = default;

DatabaseManager::~DatabaseManager()
{
    close();
}

bool DatabaseManager::open(const DatabaseConfig &config, QString *errorMessage)
{
    close();

    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("未找到 Qt SQLite 驱动（QSQLITE）。请检查程序部署目录中的 sqldrivers/qsqlite.dll。");
        }
        return false;
    }

    const QFileInfo fileInfo(config.filePath);
    if (!QDir().mkpath(fileInfo.absolutePath())) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法创建数据库目录：%1").arg(fileInfo.absolutePath());
        }
        return false;
    }

    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QString::fromLatin1(ConnectionName));
    m_database.setDatabaseName(fileInfo.absoluteFilePath());
    m_database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_database.open()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法打开本地数据库：%1\n%2")
                                .arg(fileInfo.absoluteFilePath(), m_database.lastError().text());
        }
        close();
        return false;
    }

    QSqlQuery pragma(m_database);
    const QStringList statements = {
        QStringLiteral("PRAGMA foreign_keys = ON"),
        QStringLiteral("PRAGMA journal_mode = WAL"),
        QStringLiteral("PRAGMA synchronous = NORMAL"),
        QStringLiteral("PRAGMA busy_timeout = 5000")
    };
    for (const QString &statement : statements) {
        if (!pragma.exec(statement)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("数据库初始化设置失败：%1").arg(pragma.lastError().text());
            }
            close();
            return false;
        }
    }

    m_databaseFilePath = fileInfo.absoluteFilePath();
    return true;
}

void DatabaseManager::close()
{
    if (m_database.isValid()) {
        m_database.close();
        m_database = QSqlDatabase();
    }
    if (QSqlDatabase::contains(QString::fromLatin1(ConnectionName))) {
        QSqlDatabase::removeDatabase(QString::fromLatin1(ConnectionName));
    }
    m_databaseFilePath.clear();
}

bool DatabaseManager::isOpen() const
{
    return m_database.isValid() && m_database.isOpen();
}

QSqlDatabase DatabaseManager::database() const
{
    return m_database;
}

QString DatabaseManager::databaseFilePath() const
{
    return m_databaseFilePath;
}

