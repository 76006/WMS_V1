#include "core/PasswordHasher.h"

#include <QtTest>

class CoreTests final : public QObject
{
    Q_OBJECT

private slots:
    void passwordRoundTrip()
    {
        const QString encoded = PasswordHasher::hashPassword(QStringLiteral("Admin@123"), 10000);
        QVERIFY(PasswordHasher::verifyPassword(QStringLiteral("Admin@123"), encoded));
        QVERIFY(!PasswordHasher::verifyPassword(QStringLiteral("wrong-password"), encoded));
    }

    void rejectsMalformedHash()
    {
        QVERIFY(!PasswordHasher::verifyPassword(QStringLiteral("anything"), QStringLiteral("broken")));
    }
};

QTEST_APPLESS_MAIN(CoreTests)
#include "CoreTests.moc"

