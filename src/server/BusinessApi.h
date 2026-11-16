#pragma once
#include "server/BusinessDatabase.h"
#include <QDateTime>
#include <QHash>
#include <QHttpServer>
#include <memory>

class BusinessApi
{
public:
    BusinessApi(QHttpServer &http,QSqlDatabase database,QString product);
private:
    struct Session {qlonglong user=0;QDateTime expires;std::shared_ptr<BusinessDatabase> database;QHash<QString,QString> begun;};
    QHttpServerResponse login(const QHttpServerRequest &request);
    QHttpServerResponse data(const QHttpServerRequest &request);
    QByteArray token(const QHttpServerRequest &request) const;
    void prune();
    QSqlDatabase m_database;
    QString m_product;
    QHash<QByteArray,Session> m_sessions;
    QHash<QString,QPair<QDateTime,int>> m_attempts;
};
