#include "database/SchemaMigrator.h"

#include "core/PasswordHasher.h"

#include <QFile>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

bool SchemaMigrator::migrate(QSqlDatabase database, QString *errorMessage)
{
    if (!database.isOpen()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("数据库尚未打开。");
        }
        return false;
    }

    if (!executeSchema(database, errorMessage)) {
        return false;
    }
    return ensureDefaultAdministrator(database, errorMessage);
}

bool SchemaMigrator::executeSchema(QSqlDatabase database, QString *errorMessage)
{
    QFile file(QStringLiteral(":/database/schema.sql"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法读取内置数据库结构文件。");
        }
        return false;
    }

    QString schema = QString::fromUtf8(file.readAll());
    schema.remove(QRegularExpression(QStringLiteral("--[^\\n]*")));
    const QStringList statements = schema.split(QLatin1Char(';'), Qt::SkipEmptyParts);

    if (!database.transaction()) {
        if (errorMessage) {
            *errorMessage = database.lastError().text();
        }
        return false;
    }

    QSqlQuery query(database);
    for (const QString &rawStatement : statements) {
        const QString statement = rawStatement.trimmed();
        if (statement.isEmpty()) {
            continue;
        }
        if (!query.exec(statement)) {
            database.rollback();
            if (errorMessage) {
                *errorMessage = QStringLiteral("建表失败：%1\n%2")
                                    .arg(query.lastError().text(), statement.left(160));
            }
            return false;
        }
    }

    if (!database.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("提交数据库结构失败：%1").arg(database.lastError().text());
        }
        return false;
    }
    return true;
}

bool SchemaMigrator::ensureDefaultAdministrator(QSqlDatabase database, QString *errorMessage)
{
    QSqlQuery countQuery(database);
    if (!countQuery.exec(QStringLiteral("SELECT COUNT(*) FROM users")) || !countQuery.next()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查初始用户失败：%1").arg(countQuery.lastError().text());
        }
        return false;
    }
    if (countQuery.value(0).toLongLong() > 0) {
        return true;
    }

    QSqlQuery roleQuery(database);
    roleQuery.prepare(QStringLiteral("SELECT id FROM roles WHERE code = 'ADMIN'"));
    if (!roleQuery.exec() || !roleQuery.next()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("未找到管理员角色：%1").arg(roleQuery.lastError().text());
        }
        return false;
    }

    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT INTO users(role_id, username, display_name, password_hash) VALUES(?, ?, ?, ?)"));
    insert.addBindValue(roleQuery.value(0));
    insert.addBindValue(QStringLiteral("admin"));
    insert.addBindValue(QStringLiteral("系统管理员"));
    insert.addBindValue(PasswordHasher::hashPassword(QStringLiteral("Admin@123")));
    if (!insert.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("创建初始管理员失败：%1").arg(insert.lastError().text());
        }
        return false;
    }
    return true;
}
