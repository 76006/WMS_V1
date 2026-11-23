#pragma once

#include "server/LanDatabase.h"
#include <QDateTime>
#include <QHash>
#include <QHttpServer>

class LanServer
{
public:
    explicit LanServer(LanDatabase &database);
    QHttpServer &http() { return m_http; }
private:
    struct LoginSession { qlonglong userId; QDateTime expires; };
    struct Attempt { int count = 0; QDateTime until; };
    QHttpServerResponse login(const QHttpServerRequest &request);
    QHttpServerResponse inventory(const QHttpServerRequest &request);
    QHttpServerResponse publicStock(const QHttpServerRequest &request);
    QHttpServerResponse stockFilters();
    QByteArray tokenKey(const QHttpServerRequest &request) const;
    void prune();
    LanDatabase &m_database;
    QHttpServer m_http;
    QHash<QByteArray, LoginSession> m_sessions;
    QHash<QString, Attempt> m_attempts;
};
