#include "database/SchemaMigrator.h"

#include "core/PasswordHasher.h"
#include "core/SystemProfile.h"

#include <QFile>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {
bool readApplicationId(QSqlDatabase database, int *applicationId, QString *errorMessage)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA application_id")) || !query.next()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("读取数据库文件身份失败：%1")
                                .arg(query.lastError().text());
        }
        return false;
    }
    if (applicationId) *applicationId = query.value(0).toInt();
    return true;
}

bool writeExpectedApplicationId(QSqlDatabase database, QString *errorMessage)
{
    int currentId = 0;
    if (!readApplicationId(database, &currentId, errorMessage)) return false;
    const int expectedId = SystemProfile::databaseApplicationId();
    if (currentId == expectedId) return true;
    if (currentId != 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "数据库文件头身份与“%1”不一致，已拒绝写入。")
                .arg(SystemProfile::displayName());
        }
        return false;
    }
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA application_id = %1").arg(expectedId))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("写入数据库文件身份失败：%1")
                                .arg(query.lastError().text());
        }
        return false;
    }
    int verifiedId = 0;
    if (!readApplicationId(database, &verifiedId, errorMessage)) return false;
    if (verifiedId != expectedId) {
        if (errorMessage) *errorMessage = QStringLiteral("数据库文件身份写入后校验失败。");
        return false;
    }
    return true;
}
}

bool SchemaMigrator::migrate(QSqlDatabase database, QString *errorMessage)
{
    if (!database.isOpen()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("数据库尚未打开。");
        }
        return false;
    }

    // 必须先只读核对归属，再执行任何建表或版本迁移，防止误选另一套系统的数据库。
    if (!validateDatabaseOwnershipBeforeMigration(database, errorMessage)) {
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
    if (!applyInboundInspectionMigration(database, errorMessage)) {
        return false;
    }
    if (!applyMaterialBomMigration(database, errorMessage)) {
        return false;
    }
    if (!applyDocumentFormsMigration(database, errorMessage)) {
        return false;
    }
    if (!applySalesDeliveryDateMigration(database, errorMessage)) {
        return false;
    }
    if (!applyInspectionNoticesMigration(database, errorMessage)) {
        return false;
    }
    if (!applyProductionReturnNumberMigration(database, errorMessage)) {
        return false;
    }
    if (!applyShipmentReceiptsMigration(database, errorMessage)) {
        return false;
    }
    if (!applyIssueOutboundTypesMigration(database, errorMessage)) {
        return false;
    }
    if (!applyIssueLineUsageMigration(database, errorMessage)) {
        return false;
    }
    if (!applyInspectionQuantitiesMigration(database, errorMessage)) {
        return false;
    }
    if (!applyInspectionResultRecordsMigration(database, errorMessage)) {
        return false;
    }
    if (!applySystemIdentityMigration(database, errorMessage)) {
        return false;
    }
    if (!applyTwoDigitNumberRulesMigration(database, errorMessage)) {
        return false;
    }
    if (!ensureSystemIdentity(database, errorMessage)) {
        return false;
    }
    return ensureDefaultAdministrator(database, errorMessage);
}

bool SchemaMigrator::applyTwoDigitNumberRulesMigration(QSqlDatabase database,
                                                        QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=21"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1")
                                .arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/021_two_digit_number_rules.sql"),
                              QStringLiteral("升级数据库到版本21"),
                              errorMessage);
}

bool SchemaMigrator::validateDatabaseOwnershipBeforeMigration(QSqlDatabase database,
                                                               QString *errorMessage)
{
    int applicationId = 0;
    if (!readApplicationId(database, &applicationId, errorMessage)) return false;
    if (applicationId != 0 && applicationId != SystemProfile::databaseApplicationId()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "该数据库文件属于另一套库存系统，不能使用“%1”程序打开。为防止串库，程序未执行任何迁移或业务写入。")
                .arg(SystemProfile::displayName());
        }
        return false;
    }

    QSqlQuery table(database);
    table.prepare(QStringLiteral(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='system_identity'"));
    if (!table.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库身份表失败：%1").arg(table.lastError().text());
        }
        return false;
    }
    if (table.next()) {
        QSqlQuery identity(database);
        if (!identity.exec(QStringLiteral(
                "SELECT profile_id,display_name FROM system_identity WHERE singleton_id=1"))) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("读取数据库身份失败：%1")
                                    .arg(identity.lastError().text());
            }
            return false;
        }
        if (!identity.next()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "数据库身份表存在但身份记录缺失。为防止串库，程序已拒绝打开，请勿继续使用该文件。");
            }
            return false;
        }
        if (identity.value(0).toString().trimmed().toUpper()
            != SystemProfile::profileId()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "该数据库属于“%1”，不能使用“%2”程序打开。为防止串库，程序未执行任何迁移或业务写入。")
                    .arg(identity.value(1).toString(), SystemProfile::displayName());
            }
            return false;
        }
        return true;
    }

    QSqlQuery footprint(database);
    footprint.prepare(QStringLiteral(
        "SELECT COUNT(*),"
        "COALESCE(SUM(CASE WHEN name IN ("
        "'schema_migrations','users','warehouses','locations','materials','batches',"
        "'stock_balances','business_documents','inventory_ledger','attachments',"
        "'inspection_notices','document_forms') THEN 1 ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN name='schema_migrations' THEN 1 ELSE 0 END),0) "
        "FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"));
    if (!footprint.exec() || !footprint.next()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查未标记数据库归属失败：%1")
                                .arg(footprint.lastError().text());
        }
        return false;
    }
    const int tableCount = footprint.value(0).toInt();
    const int wmsTableCount = footprint.value(1).toInt();
    const bool hasMigrationTable = footprint.value(2).toInt() == 1;
    if (tableCount == 0) return true;

    // 有表但不具备旧版 WMS 的完整基本特征时，视为未知 SQLite 文件。
    // 两套程序都禁止向其中建表，避免误选其他软件的数据库造成破坏。
    if (!hasMigrationTable || wmsTableCount < 3) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "该 SQLite 文件没有本库存系统的身份标记，也不符合旧版仓库数据库结构。为保护原文件，程序未执行任何迁移或写入。");
        }
        return false;
    }

    // 双系统拆分前只有冰美肌版本，因此无身份标记的旧 WMS 库只能由冰美肌升级。
    if (SystemProfile::isMeiJingLing()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "该文件是未标记身份的旧版冰美肌数据库。为防止串库，美鲸灵程序未执行任何迁移或业务写入；请改用冰美肌程序打开升级。");
        }
        return false;
    }
    return true;
}

bool SchemaMigrator::applySystemIdentityMigration(QSqlDatabase database,
                                                   QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=20"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/020_system_identity.sql"),
                              QStringLiteral("升级数据库到版本20"),
                              errorMessage);
}

bool SchemaMigrator::ensureSystemIdentity(QSqlDatabase database,
                                          QString *errorMessage)
{
    bool identityExists = false;
    QSqlQuery identity(database);
    if (!identity.exec(QStringLiteral(
            "SELECT profile_id,display_name FROM system_identity WHERE singleton_id=1"))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("读取系统身份失败：%1").arg(identity.lastError().text());
        }
        return false;
    }
    if (identity.next()) {
        const QString storedProfile = identity.value(0).toString().trimmed().toUpper();
        if (storedProfile != SystemProfile::profileId()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "该数据库属于“%1”，不能使用“%2”程序打开。请检查数据库文件或恢复来源。")
                    .arg(identity.value(1).toString(), SystemProfile::displayName());
            }
            return false;
        }
        identityExists = true;
        if (!writeExpectedApplicationId(database, errorMessage)) return false;
        // 冰美肌沿用原数据库配置；美鲸灵还需在每次升级时同步正式编码规则。
        if (!SystemProfile::isMeiJingLing()) return true;
    }

    // 旧版本数据库没有身份标记。已有业务资料时只能由冰美肌程序首次认领，
    // 防止误把真实冰美肌数据库当作新建的美鲸灵数据库。
    if (!identityExists && SystemProfile::isMeiJingLing()) {
        QSqlQuery existing(database);
        if (!existing.exec(QStringLiteral(
                "SELECT (SELECT COUNT(*) FROM business_documents) + "
                "(SELECT COUNT(*) FROM materials) + "
                "(SELECT COUNT(*) FROM inventory_ledger)"))
            || !existing.next()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("检查旧数据库内容失败：%1")
                                    .arg(existing.lastError().text());
            }
            return false;
        }
        if (existing.value(0).toLongLong() > 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "此数据库包含旧版业务数据但没有系统身份标记。为防止串库，请先使用冰美肌程序打开并升级，不能直接作为美鲸灵数据库使用。");
            }
            return false;
        }
    }

    if (!database.transaction()) {
        if (errorMessage) *errorMessage = database.lastError().text();
        return false;
    }
    if (!identityExists) {
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral(
            "INSERT INTO system_identity(singleton_id,profile_id,display_name) VALUES(1,?,?)"));
        insert.addBindValue(SystemProfile::profileId());
        insert.addBindValue(SystemProfile::displayName());
        if (!insert.exec()) {
            database.rollback();
            if (errorMessage) {
                *errorMessage = QStringLiteral("写入系统身份失败：%1")
                                    .arg(insert.lastError().text());
            }
            return false;
        }
    }

    if (SystemProfile::isMeiJingLing()) {
        QSqlQuery projects(database);
        // 只在首次创建空白美鲸灵库时移除 schema 自带的冰美肌项目；已经使用过的
        // 美鲸灵库保留历史项目，避免影响已有物料编码。
        if (!identityExists) {
            projects.prepare(QStringLiteral(
                "DELETE FROM material_projects WHERE code='SM01'"));
            if (!projects.exec()) {
                database.rollback();
                if (errorMessage) *errorMessage = projects.lastError().text();
                return false;
            }
        }
        projects.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO material_projects(code,name) VALUES(?,?)"));
        projects.addBindValue(SystemProfile::defaultProjectCode());
        projects.addBindValue(SystemProfile::defaultProjectName());
        if (!projects.exec()) {
            database.rollback();
            if (errorMessage) *errorMessage = projects.lastError().text();
            return false;
        }
        if (!identityExists) {
            QSqlQuery rules(database);
            rules.prepare(QStringLiteral(
                "UPDATE number_rules SET prefix=? WHERE document_type=?"));
            const auto prefixes = SystemProfile::defaultNumberPrefixes();
            for (auto it = prefixes.cbegin(); it != prefixes.cend(); ++it) {
                rules.bindValue(0, it.value());
                rules.bindValue(1, it.key());
                if (!rules.exec()) {
                    database.rollback();
                    if (errorMessage) *errorMessage = rules.lastError().text();
                    return false;
                }
            }
        }
        // 美鲸灵正式规则：LL + yyyyMMdd + '-' + 两位流水号。
        QSqlQuery issueRule(database);
        issueRule.prepare(QStringLiteral(
            "UPDATE number_rules SET prefix='LL',sequence_width=2 "
            "WHERE document_type='SCLL'"));
        if (!issueRule.exec()) {
            database.rollback();
            if (errorMessage) *errorMessage = issueRule.lastError().text();
            return false;
        }
    }
    if (!database.commit()) {
        if (errorMessage) *errorMessage = database.lastError().text();
        return false;
    }
    return writeExpectedApplicationId(database, errorMessage);
}

bool SchemaMigrator::applyInspectionResultRecordsMigration(QSqlDatabase database,
                                                             QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=19"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/019_inspection_result_records.sql"),
                              QStringLiteral("升级数据库到版本19"),
                              errorMessage);
}

bool SchemaMigrator::applyInspectionQuantitiesMigration(QSqlDatabase database,
                                                         QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=18"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/018_inspection_quantities.sql"),
                              QStringLiteral("升级数据库到版本18"),
                              errorMessage);
}

bool SchemaMigrator::applyIssueLineUsageMigration(QSqlDatabase database,
                                                   QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=17"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/017_issue_line_usage.sql"),
                              QStringLiteral("升级数据库到版本17"),
                              errorMessage);
}

bool SchemaMigrator::applyIssueOutboundTypesMigration(QSqlDatabase database,
                                                       QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=16"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/016_issue_outbound_types.sql"),
                              QStringLiteral("升级数据库到版本16"),
                              errorMessage);
}

bool SchemaMigrator::applyShipmentReceiptsMigration(QSqlDatabase database,
                                                     QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=15"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/015_shipment_receipts.sql"),
                              QStringLiteral("升级数据库到版本15"),
                              errorMessage);
}

bool SchemaMigrator::applyProductionReturnNumberMigration(QSqlDatabase database,
                                                            QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=14"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/014_production_return_number.sql"),
                              QStringLiteral("升级数据库到版本14"),
                              errorMessage);
}

bool SchemaMigrator::applyInspectionNoticesMigration(QSqlDatabase database,
                                                      QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=13"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/013_inspection_notices.sql"),
                              QStringLiteral("升级数据库到版本13"),
                              errorMessage);
}

bool SchemaMigrator::applySalesDeliveryDateMigration(QSqlDatabase database,
                                                     QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=12"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/012_sales_delivery_date.sql"),
                              QStringLiteral("升级数据库到版本12"),
                              errorMessage);
}

bool SchemaMigrator::applyDocumentFormsMigration(QSqlDatabase database,
                                                  QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=11"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/011_document_forms.sql"),
                              QStringLiteral("升级数据库到版本11"),
                              errorMessage);
}

bool SchemaMigrator::applyMaterialBomMigration(QSqlDatabase database,
                                                QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=10"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/010_material_bom.sql"),
                              QStringLiteral("升级数据库到版本10"),
                              errorMessage);
}

bool SchemaMigrator::applyInboundInspectionMigration(QSqlDatabase database,
                                                       QString *errorMessage)
{
    QSqlQuery applied(database);
    applied.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=9"));
    if (!applied.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("检查数据库版本失败：%1").arg(applied.lastError().text());
        }
        return false;
    }
    if (applied.next()) return true;
    return executeSqlResource(database,
                              QStringLiteral(":/database/migrations/009_inbound_inspections.sql"),
                              QStringLiteral("升级数据库到版本9"),
                              errorMessage);
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
