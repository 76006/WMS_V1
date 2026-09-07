#include "core/DatabaseConfig.h"
#include "core/PasswordHasher.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"
#include "services/UserService.h"

#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QVariant scalar(QSqlDatabase database, const QString &sql)
{
    QSqlQuery query(database);
    return query.exec(sql) && query.next() ? query.value(0) : QVariant();
}
}

class UserServiceTests final : public QObject
{
    Q_OBJECT
private slots:
    void administratorAndPasswordRules();
};

void UserServiceTests::administratorAndPasswordRules()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DatabaseConfig config;
    config.filePath = directory.filePath(QStringLiteral("users.db"));
    DatabaseManager manager;
    QString error;
    QVERIFY2(manager.open(config, &error), qPrintable(error));
    QVERIFY2(SchemaMigrator::migrate(manager.database(), &error), qPrintable(error));
    const qlonglong adminId = scalar(manager.database(),
        QStringLiteral("SELECT id FROM users WHERE username='admin'")).toLongLong();
    UserService admin(manager.database(), adminId);
    qlonglong userId = 0;
    QVERIFY2(admin.createUser(QStringLiteral("warehouse01"), QStringLiteral("仓库测试员"),
                              QStringLiteral("WAREHOUSE"), QStringLiteral("Warehouse@123"),
                              &userId, &error), qPrintable(error));
    QVERIFY(userId > 0);
    QVERIFY(!admin.createUser(QStringLiteral("warehouse01"), QStringLiteral("重复"),
                              QStringLiteral("QUERY"), QStringLiteral("Another@123"),
                              nullptr, &error));
    UserService ordinary(manager.database(), userId);
    QVERIFY(!ordinary.createUser(QStringLiteral("blocked01"), QStringLiteral("禁止"),
                                 QStringLiteral("QUERY"), QStringLiteral("Blocked@123"),
                                 nullptr, &error));
    QVERIFY(!admin.updateUser(adminId, QStringLiteral("系统管理员"),
                              QStringLiteral("QUERY"), true, &error));
    QVERIFY(error.contains(QStringLiteral("至少保留")));
    QVERIFY2(admin.resetPassword(userId, QStringLiteral("ResetPwd@123"), &error), qPrintable(error));
    const QString resetHash = scalar(manager.database(), QStringLiteral(
        "SELECT password_hash FROM users WHERE id=%1").arg(userId)).toString();
    QVERIFY(PasswordHasher::verifyPassword(QStringLiteral("ResetPwd@123"), resetHash));
    QVERIFY2(ordinary.changeOwnPassword(QStringLiteral("ResetPwd@123"),
                                        QStringLiteral("ChangedPwd@123"), &error), qPrintable(error));
    const QString changedHash = scalar(manager.database(), QStringLiteral(
        "SELECT password_hash FROM users WHERE id=%1").arg(userId)).toString();
    QVERIFY(PasswordHasher::verifyPassword(QStringLiteral("ChangedPwd@123"), changedHash));
    QVERIFY(!ordinary.changeOwnPassword(QStringLiteral("wrong"),
                                        QStringLiteral("ChangedAgain@123"), &error));
}

QTEST_GUILESS_MAIN(UserServiceTests)
#include "UserServiceTests.moc"
