#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDriver>
#include <functional>

namespace Wire {
QJsonValue value(const QVariant &value);
QVariant variant(const QJsonValue &value);
}

// Keeps existing Qt query models usable with a server-owned data connection.
// Client SQL writes are restricted by the server; inventory operations use business RPCs.
class QueryBridge final : public QSqlDriver
{
    Q_OBJECT
public:
    using Executor = std::function<QJsonObject(const QJsonObject &)>;
    explicit QueryBridge(Executor executor, bool remote=false);
    QJsonObject call(const QJsonObject &request) const;
    bool hasFeature(DriverFeature feature) const override;
    bool open(const QString &, const QString &, const QString &, const QString &, int, const QString &) override;
    void close() override;
    QSqlResult *createResult() const override;
    bool beginTransaction() override;
    bool commitTransaction() override;
    bool rollbackTransaction() override;
    QSqlRecord record(const QString &table) const override;
    bool remote() const { return m_remote; }
signals:
    void networkFailure(const QString &message) const;
private:
    bool control(const QString &sql);
    Executor m_executor;
    bool m_remote;
};
