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
    if (!applyProductionWorkflowMigration(database, errorMessage)) {
        return false;
    }
    if (!applyFinalFeaturesMigration(database, errorMessage)) {
        return false;
    }
    if (!applyMaterialNumberingMigration(database, errorMessage)) {
        return false;
    }
    if (!applyInventoryReportingMigration(database, errorMessage)) {
        return false;
    }
    if (!applyMaterialOperationsMigration(database, errorMessage)) {
        return false;
    }
    if (!applyProductionIssueNumberMigration(database, errorMessage)) {
        return false;
    }
    if (!applySalesOutboundDetailsMigration(database, errorMessage)) {
        return false;
    }
    return ensureDefaultAdministrator(database, errorMessage);
}

bool SchemaMigrator::applySalesOutboundDetailsMigration(QSqlDatabase database,
                                                          QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=8"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/008_sales_outbound_details.sql"),
                              QStringLiteral("升级数据库到版本8"),
                              errorMessage);
}

bool SchemaMigrator::applyProductionIssueNumberMigration(QSqlDatabase database,
                                                           QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=7"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/007_production_issue_number.sql"),
                              QStringLiteral("升级数据库到版本7"),
                              errorMessage);
}

bool SchemaMigrator::applyMaterialOperationsMigration(QSqlDatabase database,
                                                        QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=6"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/006_material_operations.sql"),
                              QStringLiteral("升级数据库到版本6"),
                              errorMessage);
}

bool SchemaMigrator::applyInventoryReportingMigration(QSqlDatabase database,
                                                       QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=5"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/005_inventory_reporting.sql"),
                              QStringLiteral("升级数据库到版本5"),
                              errorMessage);
}

bool SchemaMigrator::applyMaterialNumberingMigration(QSqlDatabase database,
                                                      QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=4"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/004_material_numbering.sql"),
                              QStringLiteral("升级数据库到版本4"),
                              errorMessage);
}

bool SchemaMigrator::applyFinalFeaturesMigration(QSqlDatabase database,
                                                  QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=3"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/003_final_features.sql"),
                              QStringLiteral("升级数据库到版本3"),
                              errorMessage);
}

bool SchemaMigrator::executeSchema(QSqlDatabase database, QString *errorMessage)
{
    return executeSqlResource(database,
                              QStringLiteral(":/database/schema.sql"),
                              QStringLiteral("建表"),
                              errorMessage);
}

bool SchemaMigrator::applyProductionWorkflowMigration(QSqlDatabase database,
                                                       QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=2"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) {
        return true;
    }

    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/002_production_workflow.sql"),
                              QStringLiteral("升级数据库到版本2"),
                              errorMessage);
}

bool SchemaMigrator::executeSqlResource(QSqlDatabase database,
                                        const QString &resourcePath,
                                        const QString &operationName,
                                        QString *errorMessage)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法读取数据库脚本：%1").arg(resourcePath);
        }
        return false;
    }

    QString schema = QString::fromUtf8(file.readAll());
    schema.remove(QRegularExpression(QStringLiteral("--[^\n]*")));
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
                *errorMessage = QStringLiteral("%1失败：%2；语句：%3")
                                    .arg(operationName, query.lastError().text(), statement.left(160));
            }
            return false;
        }
    }

    if (!database.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("提交%1失败：%2")
                                .arg(operationName, database.lastError().text());
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
