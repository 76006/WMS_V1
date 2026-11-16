#pragma once
#include <QJsonObject>
#include <QSqlDatabase>
#include <QUrl>

namespace RemoteBackend {
bool isRemote(const QSqlDatabase &database);
QJsonObject rpc(const QSqlDatabase &database,const QString &method,const QJsonObject &arguments,QString *error);
QSqlDatabase connect(const QUrl &url,const QByteArray &token);
}
