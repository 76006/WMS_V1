#pragma once
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSet>

class BusinessDatabase
{
public:
    explicit BusinessDatabase(const QSqlDatabase &connection);
    ~BusinessDatabase();
    bool open(QString *error);
    QSqlDatabase database() const {return m_bridge;}
    QJsonObject execute(const QJsonObject &request);
    bool begin(QString *error);
    bool commit(QString *error);
    void rollback();
    int depth() const {return m_depth;}
    QSqlDatabase raw() const {return m_raw;}
    static bool initialize(QSqlDatabase raw,const QString &product,const QString &password,QString *error);
    static bool upgradeProcessBoms(QSqlDatabase raw, const QString &product, QString *error);
private:
    QString translate(QString sql) const;
    QSqlDatabase m_raw,m_bridge;
    QSet<QString> m_idTables;
    int m_depth=0;
};
