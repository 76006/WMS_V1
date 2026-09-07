#include "services/UserService.h"

#include "core/PasswordHasher.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>

#include <utility>

namespace {
void setUserError(QString *target, const QString &message) { if (target) *target = message; }
QString dbText(const QString &value) {
    return value.isNull() ? QString::fromLatin1("", 0) : value;
}
}

UserService::UserService(QSqlDatabase database, qlonglong operatorId)
    : m_database(std::move(database)), m_operatorId(operatorId) {}

bool UserService::requireAdministrator(QString *errorMessage) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM users u JOIN roles r ON r.id=u.role_id "
        "WHERE u.id=? AND u.is_active=1 AND r.code='ADMIN'"));
    query.addBindValue(m_operatorId);
    if (!query.exec() || !query.next()) {
        setUserError(errorMessage, QStringLiteral("只有管理员可以执行此操作。"));
        return false;
    }
    return true;
}

bool UserService::validPassword(const QString &password, QString *errorMessage) const
{
    if (password.size() < 8) {
        setUserError(errorMessage, QStringLiteral("密码至少需要8个字符。"));
        return false;
    }
    return true;
}

bool UserService::createUser(const QString &username, const QString &displayName,
                             const QString &roleCode, const QString &password,
                             qlonglong *userId, QString *errorMessage)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.-]{3,32}$"));
    const QString login = username.trimmed();
    if (!requireAdministrator(errorMessage) || !validPassword(password, errorMessage)) return false;
    if (!pattern.match(login).hasMatch() || displayName.trimmed().isEmpty()) {
        setUserError(errorMessage, QStringLiteral("用户名需为3到32位字母、数字、点、横线或下划线，显示名不能为空。"));
        return false;
    }
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO users(role_id,username,display_name,password_hash) "
        "SELECT id,?,?,? FROM roles WHERE code=?"));
    insert.addBindValue(login);
    insert.addBindValue(displayName.trimmed());
    insert.addBindValue(PasswordHasher::hashPassword(password));
    insert.addBindValue(roleCode.trimmed().toUpper());
    if (!insert.exec() || insert.numRowsAffected() != 1) {
        setUserError(errorMessage, insert.lastError().text().contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive)
                                       ? QStringLiteral("用户名已存在。")
                                       : QStringLiteral("创建用户失败：%1").arg(insert.lastError().text()));
        return false;
    }
    const qlonglong id = insert.lastInsertId().toLongLong();
    if (!writeAudit(QStringLiteral("USER_CREATE"), id, login, errorMessage)) return false;
    if (userId) *userId = id;
    return true;
}

bool UserService::updateUser(qlonglong userId, const QString &displayName,
                             const QString &roleCode, bool active, QString *errorMessage)
{
    if (!requireAdministrator(errorMessage)) return false;
    if (userId <= 0 || displayName.trimmed().isEmpty()) {
        setUserError(errorMessage, QStringLiteral("用户或显示名无效。"));
        return false;
    }
    if (userId == m_operatorId && !active) {
        setUserError(errorMessage, QStringLiteral("不能停用当前登录账号。"));
        return false;
    }
    QSqlQuery current(m_database);
    current.prepare(QStringLiteral(
        "SELECT r.code FROM users u JOIN roles r ON r.id=u.role_id WHERE u.id=?"));
    current.addBindValue(userId);
    if (!current.exec() || !current.next()) {
        setUserError(errorMessage, QStringLiteral("用户不存在。"));
        return false;
    }
    if (current.value(0).toString() == QStringLiteral("ADMIN")
        && (roleCode != QStringLiteral("ADMIN") || !active)) {
        QSqlQuery count(m_database);
        count.exec(QStringLiteral(
            "SELECT COUNT(*) FROM users u JOIN roles r ON r.id=u.role_id "
            "WHERE r.code='ADMIN' AND u.is_active=1"));
        if (count.next() && count.value(0).toInt() <= 1) {
            setUserError(errorMessage, QStringLiteral("系统必须至少保留一个启用的管理员。"));
            return false;
        }
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE users SET display_name=?,role_id=(SELECT id FROM roles WHERE code=?),"
        "is_active=?,updated_at=? WHERE id=?"));
    update.addBindValue(displayName.trimmed());
    update.addBindValue(roleCode.trimmed().toUpper());
    update.addBindValue(active);
    update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    update.addBindValue(userId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        setUserError(errorMessage, QStringLiteral("更新用户失败：%1").arg(update.lastError().text()));
        return false;
    }
    return writeAudit(QStringLiteral("USER_UPDATE"), userId, roleCode, errorMessage);
}

bool UserService::resetPassword(qlonglong userId, const QString &newPassword, QString *errorMessage)
{
    if (!requireAdministrator(errorMessage) || !validPassword(newPassword, errorMessage)) return false;
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE users SET password_hash=?,updated_at=? WHERE id=?"));
    update.addBindValue(PasswordHasher::hashPassword(newPassword));
    update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    update.addBindValue(userId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        setUserError(errorMessage, QStringLiteral("重置密码失败。"));
        return false;
    }
    return writeAudit(QStringLiteral("PASSWORD_RESET"), userId, QString(), errorMessage);
}

bool UserService::changeOwnPassword(const QString &currentPassword, const QString &newPassword,
                                    QString *errorMessage)
{
    if (!validPassword(newPassword, errorMessage)) return false;
    QSqlQuery user(m_database);
    user.prepare(QStringLiteral(
        "SELECT u.username,u.password_hash,r.code FROM users u JOIN roles r ON r.id=u.role_id "
        "WHERE u.id=? AND u.is_active=1"));
    user.addBindValue(m_operatorId);
    if (!user.exec() || !user.next()) {
        setUserError(errorMessage, QStringLiteral("当前用户无效。"));
        return false;
    }
    bool allowEmptyDebugAdmin = false;
#ifndef NDEBUG
    allowEmptyDebugAdmin = currentPassword.isEmpty() && user.value(2).toString() == QStringLiteral("ADMIN");
#endif
    if (!allowEmptyDebugAdmin
        && !PasswordHasher::verifyPassword(currentPassword, user.value(1).toString())) {
        setUserError(errorMessage, QStringLiteral("当前密码不正确。"));
        return false;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE users SET password_hash=?,updated_at=? WHERE id=?"));
    update.addBindValue(PasswordHasher::hashPassword(newPassword));
    update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    update.addBindValue(m_operatorId);
    if (!update.exec()) {
        setUserError(errorMessage, update.lastError().text());
        return false;
    }
    return writeAudit(QStringLiteral("PASSWORD_CHANGE"), m_operatorId, QString(), errorMessage);
}

bool UserService::writeAudit(const QString &action, qlonglong entityId,
                             const QString &detail, QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) VALUES(?,?,'user',?,?)"));
    query.addBindValue(m_operatorId);
    query.addBindValue(action);
    query.addBindValue(entityId);
    query.addBindValue(dbText(detail));
    if (!query.exec()) {
        setUserError(errorMessage, QStringLiteral("记录用户操作日志失败：%1").arg(query.lastError().text()));
        return false;
    }
    return true;
}
