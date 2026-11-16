#include "server/LanDatabase.h"
#include "core/PasswordHasher.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

LanDatabase::LanDatabase(QString product) : m_product(std::move(product)) {}

bool LanDatabase::open(QString *error)
{
    if (!m_db.isValid()) {
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), QStringLiteral("lan_server"));
        m_db.setHostName(qEnvironmentVariable("WMS_PG_HOST", "127.0.0.1"));
        bool ok = false;
        const int port = qEnvironmentVariable("WMS_PG_PORT", "5432").toInt(&ok);
        if (!ok || port < 1 || port > 65535) { *error = "Invalid WMS_PG_PORT"; return false; }
        m_db.setPort(port);
        m_db.setDatabaseName(qEnvironmentVariable("WMS_PG_DATABASE"));
        m_db.setUserName(qEnvironmentVariable("WMS_PG_USER"));
        m_db.setPassword(qEnvironmentVariable("WMS_PG_PASSWORD"));
        m_db.setConnectOptions(QStringLiteral("connect_timeout=5;application_name=WmsServer"));
    }
    if (m_db.databaseName().isEmpty() || m_db.userName().isEmpty() || m_db.password().isEmpty()) {
        *error = "Set WMS_PG_DATABASE, WMS_PG_USER and WMS_PG_PASSWORD";
        return false;
    }
    if (!m_db.open()) { *error = "PostgreSQL connection failed; check configuration and QPSQL/libpq installation"; return false; }
    QSqlQuery settings(m_db);
    if (!settings.exec("SET statement_timeout = '5s'") || !settings.exec("SET TIME ZONE 'UTC'")) {
        *error = "Cannot configure PostgreSQL connection";
        return false;
    }
    return true;
}

bool LanDatabase::verify(QString *error)
{
    QSqlQuery query(m_db);
    if (!query.exec("SELECT product, version FROM wms_lan.system_identity WHERE id=1") || !query.next()) {
        *error = "LAN schema missing; initialize a dedicated test database with --init-demo";
        return false;
    }
    if (query.value(0).toString() != m_product || query.value(1).toInt() != 1) {
        *error = "Database product or LAN schema version mismatch";
        return false;
    }
    return true;
}

bool LanDatabase::ready()
{
    QString error;
    QSqlQuery check(m_db);
    if (m_db.isOpen() && check.exec("SELECT 1")) return verify(&error);
    m_db.close();
    return open(&error) && verify(&error);
}

bool LanDatabase::initializeDemo(const QString &password, QString *error)
{
    if (password.size() < 12 || password.size() > 1024) {
        *error = "WMS_DEMO_ADMIN_PASSWORD must contain 12 to 1024 characters";
        return false;
    }
    if (!m_db.transaction()) { *error = "Cannot start initialization transaction"; return false; }
    const auto fail = [&] { m_db.rollback(); *error = "Demo initialization failed; database rolled back"; return false; };
    QSqlQuery query(m_db);
    if (!query.exec("SELECT pg_advisory_xact_lock(1977011601)")) return fail();
    if (!query.exec("SELECT 1 FROM information_schema.schemata WHERE schema_name='wms_lan'")) return fail();
    if (query.next()) {
        m_db.rollback();
        *error = "wms_lan already exists; start without --init-demo (existing data is never reseeded)";
        return false;
    }
    const QStringList statements = {
        "CREATE SCHEMA wms_lan",
        "CREATE TABLE wms_lan.system_identity (id integer PRIMARY KEY CHECK(id=1), product text NOT NULL, version integer NOT NULL)",
        "CREATE TABLE wms_lan.users (id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, username text NOT NULL, display_name text NOT NULL, password_hash text NOT NULL, is_active boolean NOT NULL DEFAULT true, can_view_inventory boolean NOT NULL DEFAULT true)",
        "CREATE UNIQUE INDEX users_username_lower ON wms_lan.users(lower(username))",
        "CREATE TABLE wms_lan.materials (id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, code text NOT NULL UNIQUE, name text NOT NULL, specification text NOT NULL DEFAULT '', unit text NOT NULL, minimum_stock numeric(20,6) NOT NULL DEFAULT 0 CHECK(minimum_stock>=0))",
        "CREATE TABLE wms_lan.warehouses (id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, code text NOT NULL UNIQUE, name text NOT NULL)",
        "CREATE TABLE wms_lan.locations (id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, warehouse_id bigint NOT NULL REFERENCES wms_lan.warehouses(id), code text NOT NULL, UNIQUE(warehouse_id,code), UNIQUE(id,warehouse_id))",
        "CREATE TABLE wms_lan.stock_balances (id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, material_id bigint NOT NULL REFERENCES wms_lan.materials(id), warehouse_id bigint NOT NULL REFERENCES wms_lan.warehouses(id), location_id bigint NOT NULL, batch_no text NOT NULL DEFAULT '', quantity numeric(20,6) NOT NULL CHECK(quantity>=0), updated_at timestamptz NOT NULL DEFAULT now(), FOREIGN KEY(location_id,warehouse_id) REFERENCES wms_lan.locations(id,warehouse_id), UNIQUE(material_id,warehouse_id,location_id,batch_no))",
        "CREATE TABLE wms_lan.audit_logs (id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, user_id bigint NOT NULL REFERENCES wms_lan.users(id), action text NOT NULL, occurred_at timestamptz NOT NULL DEFAULT now())"
    };
    for (const auto &sql : statements) if (!query.exec(sql)) return fail();
    query.prepare("INSERT INTO wms_lan.system_identity VALUES(1,?,1)");
    query.addBindValue(m_product);
    if (!query.exec()) return fail();
    query.prepare("INSERT INTO wms_lan.users(username,display_name,password_hash) VALUES('admin',?,?)");
    query.addBindValue(QStringLiteral("测试管理员"));
    query.addBindValue(PasswordHasher::hashPassword(password));
    if (!query.exec()) return fail();
    const QStringList demo = {
        QStringLiteral("INSERT INTO wms_lan.materials(code,name,specification,unit,minimum_stock) VALUES('DEMO-001','测试物料（非正式库存）','联网验证','个',10)"),
        QStringLiteral("INSERT INTO wms_lan.warehouses(code,name) VALUES('DEMO','测试仓库')"),
        "INSERT INTO wms_lan.locations(warehouse_id,code) SELECT id,'A-01' FROM wms_lan.warehouses",
        "INSERT INTO wms_lan.stock_balances(material_id,warehouse_id,location_id,batch_no,quantity) SELECT m.id,w.id,l.id,'DEMO-BATCH',100 FROM wms_lan.materials m CROSS JOIN wms_lan.warehouses w JOIN wms_lan.locations l ON l.warehouse_id=w.id"
    };
    for (const auto &sql : demo) if (!query.exec(sql)) return fail();
    if (!m_db.commit()) return fail();
    return true;
}
