#pragma once

#include <QString>

namespace TestLoginPasswords {
inline bool enabled()
{
#ifdef WMS_SERVER_BUILD
    return qEnvironmentVariable("WMS_LAN_TEST_PASSWORDS") == QStringLiteral("1");
#else
    return false;
#endif
}

inline QString defaultPassword()
{
    return QStringLiteral("123456");
}

inline bool acceptsDefault(const QString &password)
{
    return enabled() && password == defaultPassword();
}

inline QString adminAssignedPassword(const QString &password)
{
    return password.isEmpty() && enabled() ? defaultPassword() : password;
}
}
