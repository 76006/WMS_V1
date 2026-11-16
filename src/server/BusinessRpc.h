#pragma once
#include <QJsonObject>
#include <QSqlDatabase>
QJsonObject dispatchBusinessRpc(QSqlDatabase database,qlonglong userId,const QString &method,const QJsonObject &arguments);
QString businessPermission(const QString &method);
