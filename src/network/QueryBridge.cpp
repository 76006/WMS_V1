#include "network/QueryBridge.h"
#include <QSqlResult>
#include <QSqlRecord>
#include <QSqlField>
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QCoreApplication>
#include <QDebug>
#include <QFile>

QJsonValue Wire::value(const QVariant &v)
{
    if (!v.isValid() || v.isNull()) return QJsonValue(QJsonValue::Null);
    if (v.metaType().id()==QMetaType::QByteArray)
        return QJsonObject{{"binary",QString::fromLatin1(v.toByteArray().toBase64())}};
    if (v.metaType().id()==QMetaType::LongLong || v.metaType().id()==QMetaType::ULongLong)
        return QJsonObject{{"integer",v.toString()}};
    return QJsonValue::fromVariant(v);
}
QVariant Wire::variant(const QJsonValue &v)
{
    if (v.isNull() || v.isUndefined()) return QVariant();
    if (v.isObject()) {
        const auto o=v.toObject();
        if(o.contains("binary")) return QByteArray::fromBase64(o.value("binary").toString().toLatin1());
        if(o.contains("integer")) return o.value("integer").toString().toLongLong();
    }
    return v.toVariant();
}

class BridgeResult final : public QSqlResult
{
public:
    explicit BridgeResult(const QueryBridge *driver):QSqlResult(driver),m_bridge(driver){}
    bool prepare(const QString &sql) override { m_sql=sql; setQuery(sql); return true; }
    bool exec() override {
        QJsonArray args,names;
        for(int i=0;i<boundValueCount();++i) { args.append(Wire::value(boundValue(i))); names.append(boundValueName(i)); }
        return execute(m_sql,args,names);
    }
    bool reset(const QString &sql) override { m_sql=sql; return execute(sql,{},{}); }
    QVariant data(int column) override {
        return at()>=0 && at()<m_rows.size() && column>=0 && column<m_rows[at()].toArray().size()
            ? Wire::variant(m_rows[at()].toArray().at(column)) : QVariant();
    }
    bool isNull(int column) override { return data(column).isNull(); }
    bool fetch(int row) override {
        if(row<0 || row>=m_rows.size()) { setAt(QSql::AfterLastRow); return false; }
        setAt(row); return true;
    }
    bool fetchFirst() override {return fetch(0);}
    bool fetchLast() override {return fetch(m_rows.size()-1);}
    int size() override {return m_rows.size();}
    int numRowsAffected() override {return m_affected;}
    QVariant lastInsertId() const override {return m_insertId;}
    QSqlRecord record() const override {return m_record;}
private:
    bool execute(const QString &sql,const QJsonArray &args,const QJsonArray &names) {
        setActive(false); setAt(QSql::BeforeFirstRow); m_rows={}; m_record.clear(); m_insertId={};
        const auto reply=m_bridge->call({{"kind","query"},{"sql",sql},{"args",args},{"names",names}});
        if(reply.contains("error")) {
            setLastError(QSqlError("WMS",reply.value("error").toString(),QSqlError::StatementError)); return false;
        }
        setLastError(QSqlError());
        m_rows=reply.value("rows").toArray();
        for(const auto &field:reply.value("fields").toArray()) {
            const auto object=field.toObject();
            m_record.append(QSqlField(object.value("name").toString(),QMetaType(object.value("type").toInt(QMetaType::QString))));
        }
        m_affected=reply.value("affected").toInt(); m_insertId=Wire::variant(reply.value("insertId"));
        setSelect(reply.value("select").toBool()); setActive(true); return true;
    }
    const QueryBridge *m_bridge;
    QString m_sql;
    QJsonArray m_rows;
    QSqlRecord m_record;
    QVariant m_insertId;
    int m_affected=0;
};

QueryBridge::QueryBridge(Executor executor,bool remote):m_executor(std::move(executor)),m_remote(remote){}
QJsonObject QueryBridge::call(const QJsonObject &request) const {
    const auto response=m_executor(request);
    if(m_remote && response.contains("error")) {
        const auto message=response.value("error").toString();
        const_cast<QueryBridge *>(this)->setProperty("networkError",message);
        emit networkFailure(message);
        if(qEnvironmentVariableIsSet("WMS_NETWORK_VERIFY")) {
            auto errors=QCoreApplication::instance()->property("networkErrors").toStringList();
            errors.append(message);QCoreApplication::instance()->setProperty("networkErrors",errors);
            qWarning().noquote()<<request.value("sql").toString().left(180)<<message;
            QFile log(qEnvironmentVariable("WMS_NETWORK_LOG"));
            if(log.open(QIODevice::WriteOnly|QIODevice::Append))log.write((request.value("sql").toString()+"\n"+message+"\n").toUtf8());
        }
    }
    return response;
}
bool QueryBridge::hasFeature(DriverFeature f) const
{
    return f==Transactions || f==QuerySize || f==BLOB || f==Unicode || f==PreparedQueries
        || f==NamedPlaceholders || f==PositionalPlaceholders || f==LastInsertId;
}
bool QueryBridge::open(const QString &,const QString &,const QString &,const QString &,int,const QString &) {setOpen(true);return true;}
void QueryBridge::close(){setOpen(false);}
QSqlResult *QueryBridge::createResult() const {return new BridgeResult(this);}
bool QueryBridge::control(const QString &sql) {
    const auto reply=call({{"kind","query"},{"sql",sql}});
    if(reply.contains("error")) {setLastError(QSqlError("WMS",reply.value("error").toString(),QSqlError::TransactionError));return false;}
    return true;
}
bool QueryBridge::beginTransaction(){return control("BEGIN");}
bool QueryBridge::commitTransaction(){return control("COMMIT");}
bool QueryBridge::rollbackTransaction(){return control("ROLLBACK");}
QSqlRecord QueryBridge::record(const QString &table) const {
    BridgeResult result(this);
    result.reset("SELECT * FROM " + escapeIdentifier(table,TableName) + " LIMIT 0");
    return result.record();
}
