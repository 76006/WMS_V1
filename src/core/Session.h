#pragma once

#include <QString>

struct Session
{
    qlonglong userId = 0;
    QString username;
    QString displayName;
    QString roleCode;

    bool isAdministrator() const { return roleCode == QStringLiteral("ADMIN"); }
    bool canManageWarehouse() const
    {
        return roleCode == QStringLiteral("ADMIN") || roleCode == QStringLiteral("WAREHOUSE");
    }
    bool canViewInventory() const { return userId > 0; }
};

