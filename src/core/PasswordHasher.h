#pragma once

#include <QString>

class PasswordHasher
{
public:
    static QString hashPassword(const QString &password, int iterations = 120000);
    static bool verifyPassword(const QString &password, const QString &encodedHash);

private:
    static QByteArray deriveKey(const QByteArray &password,
                                const QByteArray &salt,
                                int iterations,
                                int outputLength);
};

