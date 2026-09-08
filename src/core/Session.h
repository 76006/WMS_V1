#pragma once

#include <QString>
#include <QSet>

struct Session
{
    qlonglong userId = 0;
    QString username;
    QString displayName;
    QString roleCode;
    QSet<QString> permissions;

    bool isAdministrator() const { return roleCode == QStringLiteral("ADMIN"); }
    bool hasPermission(const QString &code) const
    {
        return isAdministrator() || permissions.contains(code);
    }
    bool canManageWarehouse() const
    {
        return hasPermission(QStringLiteral("POST_INVENTORY"));
    }
    bool canPostProduction() const { return hasPermission(QStringLiteral("POST_PRODUCTION")); }
    bool canManageMaterials() const { return hasPermission(QStringLiteral("MANAGE_MATERIALS")); }
    bool canManageWarehouseData() const { return hasPermission(QStringLiteral("MANAGE_WAREHOUSES")); }
    bool canManageAttachments() const { return hasPermission(QStringLiteral("MANAGE_ATTACHMENTS")); }
    bool canManageUsers() const { return hasPermission(QStringLiteral("MANAGE_USERS")); }
    bool canManageSystem() const { return hasPermission(QStringLiteral("MANAGE_SYSTEM")); }
    bool canViewAudit() const { return hasPermission(QStringLiteral("VIEW_AUDIT")); }
    bool canViewInventory() const { return userId > 0 && hasPermission(QStringLiteral("VIEW_INVENTORY")); }
};
