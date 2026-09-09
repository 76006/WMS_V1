#pragma once

#include <QSqlDatabase>

class SchemaMigrator
{
public:
    static bool migrate(QSqlDatabase database, QString *errorMessage = nullptr);

private:
    static bool executeSchema(QSqlDatabase database, QString *errorMessage);
    static bool applyProductionWorkflowMigration(QSqlDatabase database, QString *errorMessage);
    static bool applyFinalFeaturesMigration(QSqlDatabase database, QString *errorMessage);
    static bool applyMaterialNumberingMigration(QSqlDatabase database, QString *errorMessage);
    static bool applyInventoryReportingMigration(QSqlDatabase database, QString *errorMessage);
    static bool applyMaterialOperationsMigration(QSqlDatabase database, QString *errorMessage);
    static bool applyProductionIssueNumberMigration(QSqlDatabase database, QString *errorMessage);
    static bool applySalesOutboundDetailsMigration(QSqlDatabase database, QString *errorMessage);
    static bool applyInboundInspectionMigration(QSqlDatabase database, QString *errorMessage);
    static bool applyMaterialBomMigration(QSqlDatabase database, QString *errorMessage);
    static bool executeSqlResource(QSqlDatabase database,
                                   const QString &resourcePath,
                                   const QString &operationName,
                                   QString *errorMessage);
    static bool ensureDefaultAdministrator(QSqlDatabase database, QString *errorMessage);
};
