#include "server/LegacySource.h"

#include "core/SystemProfile.h"
#include "database/SchemaMigrator.h"
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace {
constexpr int currentVersion = 22;

QString connectionName()
{
    return QStringLiteral("import_sqlite_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

// VACUUM INTO reads a single consistent SQLite snapshot, including committed
// WAL records. It writes to the destination even with a read-only source handle.
bool copySnapshot(const QString &source, const QString &target, QString *error)
{
    const QString name = connectionName();
    const bool ok = [&] {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(source);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
        if (!db.open())
            return fail(error, QStringLiteral("无法只读打开旧库：%1").arg(db.lastError().text()));
        QSqlQuery query(db);
        if (!query.prepare(QStringLiteral("VACUUM INTO ?")))
            return fail(error, query.lastError().text());
        query.addBindValue(target);
        if (!query.exec())
            return fail(error, QStringLiteral("制作旧库快照失败：%1").arg(query.lastError().text()));
        return true;
    }();
    QSqlDatabase::removeDatabase(name);
    return ok;
}

bool inspect(QSqlDatabase db, const QString &product, int *version, QString *error)
{
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
            "('schema_migrations','roles','users','materials','warehouses','locations',"
            "'stock_balances','business_documents','business_document_items','inventory_ledger')"))
        || !q.next() || q.value(0).toInt() != 10) {
        return fail(error, QStringLiteral("所选文件不是完整的旧版仓库数据库。"));
    }
    if (!q.exec(QStringLiteral("SELECT version FROM schema_migrations ORDER BY version")))
        return fail(error, q.lastError().text());
    int latest = 0;
    while (q.next()) {
        if (q.value(0).toLongLong() != ++latest || latest > currentVersion)
            return fail(error, QStringLiteral("旧库迁移记录不连续或版本超过 %1。").arg(currentVersion));
    }
    if (!latest || q.lastError().isValid())
        return fail(error, QStringLiteral("无法确认旧库的版本。"));
    if (!q.exec(QStringLiteral("PRAGMA application_id")) || !q.next())
        return fail(error, QStringLiteral("无法读取旧库的产品标识。"));
    const int appId = q.value(0).toInt();
    if (appId != 0 && appId != SystemProfile::databaseApplicationId())
        return fail(error, QStringLiteral("旧库的文件头属于其他产品。"));

    if (!q.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='system_identity'")))
        return fail(error, q.lastError().text());
    const bool identified = q.next();
    if (identified != (latest >= 20))
        return fail(error, QStringLiteral("产品身份表与旧库版本不一致。"));
    if (identified) {
        if (!q.exec(QStringLiteral("SELECT profile_id FROM system_identity WHERE singleton_id=1"))
            || !q.next() || q.value(0).toString() != product) {
            return fail(error, QStringLiteral("旧库的产品身份与所选系统不符。"));
        }
    } else if (product != QStringLiteral("ICE_BEAUTY")) {
        return fail(error, QStringLiteral("版本 20 以前的无身份标记旧库只能导入冰美肌。"));
    }
    *version = latest;
    return true;
}
}

LegacySource::~LegacySource()
{
    if (m_database.isValid()) {
        const QString name = m_database.connectionName();
        m_database.close();
        m_database = {};
        QSqlDatabase::removeDatabase(name);
    }
}

bool LegacySource::prepare(const QString &sourceFile, const QString &product, QString *error)
{
    if (m_database.isValid() || !m_directory.isValid())
        return fail(error, QStringLiteral("无法创建用于迁移的临时工作目录。"));
    if ((product != QStringLiteral("ICE_BEAUTY") && product != QStringLiteral("MEI_JING_LING"))
        || product != SystemProfile::profileId()) {
        return fail(error, QStringLiteral("导入程序的产品设置无效。"));
    }
    const QFileInfo file(sourceFile);
    if (!file.isFile() || file.size() == 0)
        return fail(error, QStringLiteral("请选择现有的非空 SQLite 仓库文件。"));
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        return fail(error, QStringLiteral("主机包缺少 QSQLITE 驱动。"));

    const QString snapshot = m_directory.filePath(QStringLiteral("snapshot.db"));
    if (!copySnapshot(file.absoluteFilePath(), snapshot, error)) return false;
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName());
    m_database.setDatabaseName(snapshot);
    if (!m_database.open()) return fail(error, m_database.lastError().text());
    int version = 0;
    if (!inspect(m_database, product, &version, error)) return false;
    QSqlQuery check(m_database);
    if (!check.exec(QStringLiteral("PRAGMA foreign_keys=ON")))
        return fail(error, check.lastError().text());
    if (version < currentVersion) {
        if (!SchemaMigrator::migrate(m_database, error)) return false;
        if (!inspect(m_database, product, &version, error)) return false;
        if (version != currentVersion)
            return fail(error, QStringLiteral("临时快照未升级到预期版本。"));
    }
    if (!check.exec(QStringLiteral("PRAGMA query_only=ON")) ||
        !check.exec(QStringLiteral("PRAGMA integrity_check")) || !check.next() ||
        check.value(0).toString() != QStringLiteral("ok")) {
        return fail(error, QStringLiteral("旧库快照未通过 SQLite 完整性检查。"));
    }
    if (!check.exec(QStringLiteral("PRAGMA foreign_key_check")))
        return fail(error, check.lastError().text());
    if (check.next()) return fail(error, QStringLiteral("旧库存在失效外键，涉及表 %1。").arg(check.value(0).toString()));
    if (check.lastError().isValid()) return fail(error, check.lastError().text());
    return true;
}
