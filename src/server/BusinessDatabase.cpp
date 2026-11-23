#include "server/BusinessDatabase.h"
#include "server/TestLoginPasswords.h"
#include "network/QueryBridge.h"
#include "core/PasswordHasher.h"
#include "core/SystemProfile.h"
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QSqlField>
#include <QUuid>

namespace {
QJsonObject failure(const QString &message){return {{"error",message}};}
bool run(QSqlDatabase db,const QString &sql,QString *error){QSqlQuery q(db);if(q.exec(sql))return true;*error=q.lastError().text();return false;}
}
BusinessDatabase::BusinessDatabase(const QSqlDatabase &connection)
{
    m_raw=QSqlDatabase::cloneDatabase(connection,"business_pg_"+QUuid::createUuid().toString(QUuid::WithoutBraces));
}
BusinessDatabase::~BusinessDatabase()
{
    if(m_depth>0)m_raw.rollback();
    auto bridgeName=m_bridge.connectionName(),rawName=m_raw.connectionName();
    m_bridge=QSqlDatabase();m_raw.close();m_raw=QSqlDatabase();
    QSqlDatabase::removeDatabase(bridgeName);QSqlDatabase::removeDatabase(rawName);
}
bool BusinessDatabase::open(QString *error)
{
    if(!m_raw.open()){*error=m_raw.lastError().text();return false;}
    if(!run(m_raw,"SET search_path=wms_business,public,pg_catalog",error)
        || !run(m_raw,"SET statement_timeout='15s'",error)
        || !run(m_raw,"SET lock_timeout='1s'",error)
        || !run(m_raw,"SET idle_in_transaction_session_timeout='60s'",error)
        || !run(m_raw,"SET TIME ZONE 'Asia/Shanghai'",error))return false;
    QSqlQuery q(m_raw);
    if(!q.exec("SELECT table_name FROM information_schema.columns WHERE table_schema='wms_business' AND column_name='id'")){*error=q.lastError().text();return false;}
    while(q.next())m_idTables.insert(q.value(0).toString());
    m_bridge=QSqlDatabase::addDatabase(new QueryBridge([this](const QJsonObject &r){return execute(r);}),"business_bridge_"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_bridge.open();return true;
}
bool BusinessDatabase::begin(QString *error)
{
    if(m_depth>0){if(!run(m_raw,"SAVEPOINT wms_"+QString::number(m_depth),error))return false;++m_depth;return true;}
    if(!m_raw.transaction()){*error=m_raw.lastError().text();return false;}
    QSqlQuery lock(m_raw);
    if(!lock.exec("SELECT pg_try_advisory_xact_lock(1977011602)") || !lock.next() || !lock.value(0).toBool()){
        m_raw.rollback();*error=QStringLiteral("其他用户正在提交，请稍后重试。");return false;
    }
    m_depth=1;return true;
}
bool BusinessDatabase::commit(QString *error)
{
    if(m_depth==0){*error=QStringLiteral("事务已失效，请刷新后重试。");return false;}
    if(m_depth>1){if(!run(m_raw,"RELEASE SAVEPOINT wms_"+QString::number(m_depth-1),error))return false;--m_depth;return true;}
    if(!m_raw.commit()){*error=m_raw.lastError().text();m_raw.rollback();m_depth=0;return false;}
    m_depth=0;return true;
}
void BusinessDatabase::rollback()
{
    QString ignored;
    if(m_depth>1){run(m_raw,"ROLLBACK TO SAVEPOINT wms_"+QString::number(m_depth-1),&ignored);run(m_raw,"RELEASE SAVEPOINT wms_"+QString::number(m_depth-1),&ignored);--m_depth;}
    else {m_raw.rollback();m_depth=0;}
}
QString BusinessDatabase::translate(QString sql) const
{
    sql.remove(QRegularExpression("\\s+COLLATE\\s+NOCASE",QRegularExpression::CaseInsensitiveOption));
    sql.replace(QRegularExpression("\\bLIKE\\b",QRegularExpression::CaseInsensitiveOption),"ILIKE");
    sql.replace(QRegularExpression("\\bMAX\\(0\\s*,",QRegularExpression::CaseInsensitiveOption),"GREATEST(0,");
    sql.replace(QRegularExpression("\\bMIN\\(([^(),]+),",QRegularExpression::CaseInsensitiveOption),"LEAST(\\1,");
    sql.replace(QRegularExpression("(\\?|:[A-Za-z_0-9]\\w*)\\s+IS\\s+NULL",QRegularExpression::CaseInsensitiveOption),"CAST(\\1 AS TEXT) IS NULL");
    if(sql.contains(QRegularExpression("\\bINSERT\\s+OR\\s+IGNORE\\b",QRegularExpression::CaseInsensitiveOption))){
        sql.replace(QRegularExpression("INSERT\\s+OR\\s+IGNORE",QRegularExpression::CaseInsensitiveOption),"INSERT");
        sql+=" ON CONFLICT DO NOTHING";
    }
    return sql;
}
QJsonObject BusinessDatabase::execute(const QJsonObject &request)
{
    QString sql=request.value("sql").toString().trimmed();
    while(sql.endsWith(';'))sql.chop(1);
    QString error;
    if(sql.startsWith("BEGIN",Qt::CaseInsensitive))return begin(&error)?QJsonObject{}:failure(error);
    if(sql.compare("COMMIT",Qt::CaseInsensitive)==0)return commit(&error)?QJsonObject{}:failure(error);
    if(sql.compare("ROLLBACK",Qt::CaseInsensitive)==0){rollback();return {};}
    sql=translate(sql);
    const auto insert=QRegularExpression("^INSERT\\s+INTO\\s+([a-z_]+)",QRegularExpression::CaseInsensitiveOption).match(sql);
    bool returning=insert.hasMatch() && m_idTables.contains(insert.captured(1)) && !sql.contains("RETURNING",Qt::CaseInsensitive);
    if(returning)sql+=" RETURNING id";
    QSqlQuery query(m_raw);
    if(!query.prepare(sql))return failure(query.lastError().text());
    const auto args=request.value("args").toArray();
    const auto names=request.value("names").toArray();
    for(int i=0;i<args.size();++i){
        const auto name=i<names.size()?names[i].toString():QString();
        auto value=Wire::variant(args[i]);
        if(value.metaType().id()==QMetaType::Bool)value=value.toBool()?1:0;
        if(!name.isEmpty() && sql.contains(name))query.bindValue(name,value);else query.addBindValue(value);
    }
    if(!query.exec())return failure(query.lastError().text());
    QJsonArray fields,rows;
    const auto record=query.record();
    for(int i=0;i<record.count();++i)fields.append(QJsonObject{{"name",record.fieldName(i)},{"type",record.field(i).metaType().id()}});
    QVariant id;
    if(returning){if(query.next())id=query.value(0);}
    else while(query.next()){
        QJsonArray row;for(int i=0;i<record.count();++i)row.append(Wire::value(query.value(i)));rows.append(row);
    }
    return {{"select",query.isSelect()&&!returning},{"fields",fields},{"rows",rows},{"affected",query.numRowsAffected()},{"insertId",Wire::value(id)}};
}

bool BusinessDatabase::upgradeProcessBoms(QSqlDatabase raw, const QString &product, QString *error)
{
    QSqlQuery query(raw);
    if (!query.exec("SELECT to_regclass('wms_business.network_identity')") || !query.next()) {
        *error = query.lastError().text();
        return false;
    }
    if (query.value(0).isNull()) return true; // Standalone LAN pilot schema.
    query.prepare("SELECT 1 FROM wms_business.network_identity WHERE product=?");
    query.addBindValue(product);
    if (!query.exec() || !query.next()) {
        *error = QStringLiteral("联网业务库身份不匹配");
        return false;
    }
    QStringList migrations;
    for (const int version : {22, 23, 24}) {
        if (!query.exec(QStringLiteral("SELECT 1 FROM wms_business.schema_migrations WHERE version=%1").arg(version))) {
            *error = query.lastError().text(); return false;
        }
        if (query.next()) continue;
        QFile migration(version == 22 ? ":/database/migrations/022_process_boms.sql"
                        : version == 23 ? ":/database/migrations/023_production_stages.sql"
                                        : ":/database/migrations/024_purchase_suppliers.sql");
        if (!migration.open(QIODevice::ReadOnly)) { *error = migration.errorString(); return false; }
        migrations.append(QString::fromUtf8(migration.readAll()));
    }
    if (migrations.isEmpty()) return true;

    if (!raw.transaction()) { *error = raw.lastError().text(); return false; }
    if (!run(raw, "SET LOCAL search_path=wms_business,public,pg_catalog", error)) {
        raw.rollback();
        return false;
    }
    for (const auto &statement : migrations.join(QLatin1Char('\n')).trimmed().split(';', Qt::SkipEmptyParts)) {
        if (!run(raw, statement, error)) { raw.rollback(); return false; }
    }
    if (raw.commit()) return true;
    *error = raw.lastError().text();
    raw.rollback();
    return false;
}

bool BusinessDatabase::initialize(QSqlDatabase raw,const QString &product,const QString &password,QString *error)
{
    if (password.size() < 12 && !TestLoginPasswords::acceptsDefault(password)) {
        *error = "Set WMS_DEMO_ADMIN_PASSWORD (at least 12 characters)";
        return false;
    }
    QSqlQuery existing(raw);
    if(!existing.exec("SELECT 1 FROM information_schema.schemata WHERE schema_name='wms_business'")){*error=existing.lastError().text();return false;}
    if(existing.next()){*error="Business schema already exists; refusing to overwrite";return false;}
    if(!raw.transaction()){*error=raw.lastError().text();return false;}
    auto fail=[&]{raw.rollback();return false;};
    const QStringList setup={
        "CREATE EXTENSION IF NOT EXISTS citext WITH SCHEMA public",
        "CREATE SCHEMA wms_business",
        "SET LOCAL search_path=wms_business,public,pg_catalog",
        "CREATE FUNCTION strftime(fmt text,val text,modifier text DEFAULT '') RETURNS text LANGUAGE sql STABLE AS $$ SELECT to_char(CASE WHEN val='now' THEN now() ELSE val::timestamptz END AT TIME ZONE 'Asia/Shanghai',replace(replace(replace(replace(replace(replace(fmt,'%Y','YYYY'),'%m','MM'),'%d','DD'),'%H','HH24'),'%M','MI'),'%f','SS.MS')) $$",
        "CREATE FUNCTION date(val text,modifier text DEFAULT '') RETURNS text LANGUAGE sql STABLE AS $$ SELECT CASE WHEN val='now' THEN to_char(now() AT TIME ZONE 'Asia/Shanghai','YYYY-MM-DD') ELSE substring(val,1,10) END $$",
        "CREATE FUNCTION substr(val text,pos integer) RETURNS text LANGUAGE sql IMMUTABLE AS $$ SELECT pg_catalog.substr(val,CASE WHEN pos<0 THEN length(val)+pos+1 ELSE pos END) $$",
        "CREATE FUNCTION substr(val text,pos integer,len integer) RETURNS text LANGUAGE sql IMMUTABLE AS $$ SELECT pg_catalog.substr(val,CASE WHEN pos<0 THEN length(val)+pos+1 ELSE pos END,len) $$",
        "CREATE FUNCTION instr(val text,part text) RETURNS integer LANGUAGE sql IMMUTABLE AS $$ SELECT strpos(val,part) $$",
        "CREATE FUNCTION concat_state(state text,val text) RETURNS text LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN state IS NULL THEN val WHEN val IS NULL THEN state ELSE state||','||val END $$",
        "CREATE FUNCTION concat_state_delim(state text,val text,delimiter text) RETURNS text LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN state IS NULL THEN val WHEN val IS NULL THEN state ELSE state||delimiter||val END $$",
        "CREATE AGGREGATE group_concat(text) (SFUNC=concat_state,STYPE=text)",
        "CREATE AGGREGATE group_concat(text,text) (SFUNC=concat_state_delim,STYPE=text)"
    };
    for(const auto &sql:setup)if(!run(raw,sql,error))return fail();
    QFile file(":/database/postgresql/business-schema.json");
    if(!file.open(QIODevice::ReadOnly)){*error="Missing business schema resource";return fail();}
    const auto schema=QJsonDocument::fromJson(file.readAll()).object();
    for(const auto &sql:schema.value("statements").toArray())if(!run(raw,sql.toString(),error))return fail();
    QSet<QString> identityTables;
    for(const auto &seed:schema.value("seeds").toArray()){
        const auto o=seed.toObject();const auto table=o.value("table").toString();const auto values=o.value("values").toObject();
        QStringList keys,marks;for(auto i=values.begin();i!=values.end();++i){keys<<'"'+i.key()+'"';marks<<"?";}
        QSqlQuery insert(raw);insert.prepare("INSERT INTO "+table+"("+keys.join(',')+") VALUES("+marks.join(',')+")");
        for(auto i=values.begin();i!=values.end();++i){
            auto value=i.value().toVariant();
            if(i.key()=="password_hash")value=PasswordHasher::hashPassword(password);
            if(table=="system_identity" && i.key()=="profile_id")value=product;
            if(table=="system_identity" && i.key()=="display_name")value=SystemProfile::displayName();
            if(table=="material_projects" && i.key()=="code")value=SystemProfile::defaultProjectCode();
            if(table=="material_projects" && i.key()=="name")value=SystemProfile::defaultProjectName();
            if(table=="number_rules" && i.key()=="prefix")value=SystemProfile::defaultNumberPrefixes().value(values.value("document_type").toString(),value.toString());
            insert.addBindValue(value);
        }
        if(!insert.exec()){*error=insert.lastError().text();return fail();}
        if(values.contains("id"))identityTables.insert(table);
    }
    for(const auto &table:identityTables){
        QSqlQuery q(raw);q.prepare("SELECT pg_get_serial_sequence(?, 'id')");q.addBindValue("wms_business."+table);
        if(q.exec() && q.next() && !q.value(0).isNull()){
            if(!run(raw,"SELECT setval(pg_get_serial_sequence('wms_business."+table+"','id'),GREATEST((SELECT MAX(id) FROM "+table+"),1))",error))return fail();
        }
    }
    if(!run(raw,"CREATE TABLE network_requests (request_id text PRIMARY KEY,user_id bigint NOT NULL,payload_hash text NOT NULL,result jsonb NOT NULL,created_at timestamptz NOT NULL DEFAULT now())",error))return fail();
    if(!run(raw,"CREATE TABLE network_identity (product text NOT NULL,version integer NOT NULL)",error))return fail();
    if(!run(raw,"CREATE VIEW users_public AS SELECT id,role_id,username,display_name,is_active,last_login_at,created_at,updated_at FROM users",error))return fail();
    if(!run(raw,R"SQL(CREATE FUNCTION authenticated_actor() RETURNS trigger LANGUAGE plpgsql AS $$
        DECLARE actor bigint := NULLIF(current_setting('wms.user_id',true),'')::bigint;
        BEGIN
          IF actor IS NULL THEN RAISE EXCEPTION 'Authenticated operator is required'; END IF;
          IF TG_TABLE_NAME='audit_logs' THEN NEW.user_id:=actor;
          ELSIF TG_TABLE_NAME='attachments' THEN
            IF TG_OP='INSERT' OR NEW.file_data IS DISTINCT FROM OLD.file_data THEN NEW.uploaded_by:=actor; ELSE NEW.uploaded_by:=OLD.uploaded_by; END IF;
            IF NEW.is_deleted=1 THEN NEW.deleted_by:=actor; END IF;
          ELSE NEW.updated_by:=actor;
          END IF;
          RETURN NEW;
        END $$)SQL",error))return fail();
    for(const auto &table:QStringList{"audit_logs","attachments","material_images","inspection_notices"})
        if(!run(raw,"CREATE TRIGGER authenticated_actor BEFORE INSERT OR UPDATE ON "+table+" FOR EACH ROW EXECUTE FUNCTION authenticated_actor()",error))return fail();
    QSqlQuery identity(raw);identity.prepare("INSERT INTO network_identity VALUES(?,2)");identity.addBindValue(product);
    if(!identity.exec()){*error=identity.lastError().text();return fail();}
    if(!raw.commit()){*error=raw.lastError().text();return fail();}
    return true;
}
