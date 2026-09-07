#include "core/PasswordHasher.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QStringList>

namespace {
bool constantTimeEquals(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size()) {
        return false;
    }

    unsigned char difference = 0;
    for (qsizetype i = 0; i < left.size(); ++i) {
        difference |= static_cast<unsigned char>(left.at(i))
                      ^ static_cast<unsigned char>(right.at(i));
    }
    return difference == 0;
}
}

QByteArray PasswordHasher::deriveKey(const QByteArray &password,
                                     const QByteArray &salt,
                                     int iterations,
                                     int outputLength)
{
    constexpr int hashLength = 32;
    QByteArray result;
    const int blockCount = (outputLength + hashLength - 1) / hashLength;

    for (int block = 1; block <= blockCount; ++block) {
        QByteArray blockIndex(4, Qt::Uninitialized);
        blockIndex[0] = static_cast<char>((block >> 24) & 0xff);
        blockIndex[1] = static_cast<char>((block >> 16) & 0xff);
        blockIndex[2] = static_cast<char>((block >> 8) & 0xff);
        blockIndex[3] = static_cast<char>(block & 0xff);

        QByteArray u = QMessageAuthenticationCode::hash(
            salt + blockIndex, password, QCryptographicHash::Sha256);
        QByteArray t = u;
        for (int iteration = 1; iteration < iterations; ++iteration) {
            u = QMessageAuthenticationCode::hash(u, password, QCryptographicHash::Sha256);
            for (int i = 0; i < hashLength; ++i) {
                t[i] = static_cast<char>(t.at(i) ^ u.at(i));
            }
        }
        result.append(t);
    }
    return result.left(outputLength);
}

QString PasswordHasher::hashPassword(const QString &password, int iterations)
{
    QByteArray salt(16, Qt::Uninitialized);
    for (auto &byte : salt) {
        byte = static_cast<char>(QRandomGenerator::global()->generate() & 0xff);
    }

    const QByteArray derived = deriveKey(password.toUtf8(), salt, iterations, 32);
    return QStringLiteral("pbkdf2_sha256$%1$%2$%3")
        .arg(iterations)
        .arg(QString::fromLatin1(salt.toBase64(QByteArray::OmitTrailingEquals)))
        .arg(QString::fromLatin1(derived.toBase64(QByteArray::OmitTrailingEquals)));
}

bool PasswordHasher::verifyPassword(const QString &password, const QString &encodedHash)
{
    const QStringList parts = encodedHash.split(QLatin1Char('$'));
    if (parts.size() != 4 || parts.at(0) != QStringLiteral("pbkdf2_sha256")) {
        return false;
    }

    bool ok = false;
    const int iterations = parts.at(1).toInt(&ok);
    if (!ok || iterations < 10000 || iterations > 1000000) {
        return false;
    }

    const QByteArray salt = QByteArray::fromBase64(parts.at(2).toLatin1());
    const QByteArray expected = QByteArray::fromBase64(parts.at(3).toLatin1());
    if (salt.isEmpty() || expected.isEmpty()) {
        return false;
    }

    const QByteArray actual = deriveKey(password.toUtf8(), salt, iterations, expected.size());
    return constantTimeEquals(actual, expected);
}

