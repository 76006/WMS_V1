#pragma once

#include <QSqlDatabase>
#include <QString>

class UserService
{
public:
    UserService(QSqlDatabase database, qlonglong operatorId);

    bool createUser(const QString &username, const QString &displayName,
                    const QString &roleCode, const QString &password,
                    qlonglong *userId = nullptr, QString *errorMessage = nullptr);
    bool updateUser(qlonglong userId, const QString &displayName,
                    const QString &roleCode, bool active,
                    QString *errorMessage = nullptr);
    bool resetPassword(qlonglong userId, const QString &newPassword,
                       QString *errorMessage = nullptr);
    bool changeOwnPassword(const QString &currentPassword, const QString &newPassword,
                           QString *errorMessage = nullptr);

private:
    bool requireAdministrator(QString *errorMessage) const;
    bool validPassword(const QString &password, QString *errorMessage) const;
    bool writeAudit(const QString &action, qlonglong entityId,
                    const QString &detail, QString *errorMessage);

    QSqlDatabase m_database;
    qlonglong m_operatorId = 0;
};
