#include "server/LanServer.h"
#include "core/PasswordHasher.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHttpServerRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSqlQuery>
#include <QUrlQuery>
#include <QVariant>

namespace {
using Status = QHttpServerResponder::StatusCode;
QHttpServerResponse error(Status code, const QString &message)
{
    return QHttpServerResponse(QJsonObject{{"error", message}}, code);
}
QByteArray digest(const QByteArray &token)
{
    return QCryptographicHash::hash(token, QCryptographicHash::Sha256);
}

// Both supported process BOM formats use top-level groups and nested "items".
// Use the product and group's identity so reordering groups preserves selections.
const QString moduleRoots = QStringLiteral(R"SQL(
    WITH RECURSIVE roots AS (
        SELECT md5(jsonb_build_array(b.product_material_id,
                   nullif(trim(g.node->>'code'), ''), trim(g.node->>'name'))::text) AS module_id,
               coalesce(nullif(trim(g.node->>'name'), ''), trim(g.node->>'code')) AS label,
               p.code::text AS product, g.ordinal, g.node
        FROM wms_business.process_boms b
        JOIN wms_business.materials p ON p.id=b.product_material_id
        CROSS JOIN LATERAL jsonb_array_elements(coalesce(b.payload::jsonb->'nodes',
            b.payload::jsonb->'groups')) WITH ORDINALITY AS g(node, ordinal)
    )
)SQL");
}

LanServer::LanServer(LanDatabase &database) : m_database(database)
{
    m_http.afterRequest([](QHttpServerResponse &&response) {
        response.setHeader("Cache-Control", "no-store");
        response.setHeader("X-Content-Type-Options", "nosniff");
        return std::move(response);
    });
    m_http.route("/health", QHttpServerRequest::Method::Get, [this] {
        if (!m_database.ready()) return error(Status::ServiceUnavailable, QStringLiteral("数据库暂时不可用"));
        return QHttpServerResponse(QJsonObject{{"status", "ok"}, {"apiVersion", 1},
            {"product", m_database.product()}, {"mode", "query-pilot"}});
    });
    m_http.route("/api/v1/login", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) { return login(request); });
    m_http.route("/", QHttpServerRequest::Method::Get, [this] {
        QFile page(QStringLiteral(":/resources/web/inventory.html"));
        if (!page.open(QIODevice::ReadOnly))
            return error(Status::InternalServerError, QStringLiteral("库存页面暂时不可用"));
        const QString brand = m_database.product() == "ICE_BEAUTY"
            ? QStringLiteral("冰美肌") : QStringLiteral("美鲸灵");
        return QHttpServerResponse("text/html; charset=utf-8", page.readAll().replace("{{brand}}", brand.toUtf8()));
    });
    m_http.route("/stock", QHttpServerRequest::Method::Get,
        [this](const QHttpServerRequest &request) { return publicStock(request); });
    m_http.route("/stock/filters", QHttpServerRequest::Method::Get, [this] { return stockFilters(); });
    m_http.route("/api/v1/inventory", QHttpServerRequest::Method::Get,
        [this](const QHttpServerRequest &request) { return inventory(request); });
    m_http.route("/api/v1/logout", QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) {
            m_sessions.remove(tokenKey(request));
            return QHttpServerResponse(QJsonObject{{"ok", true}});
        });
}

QHttpServerResponse LanServer::stockFilters()
{
    if (!m_database.ready()) return error(Status::ServiceUnavailable, QStringLiteral("库存数据库暂时不可用"));
    const QList<QPair<QString, QString>> lists = {
        {"categories", "SELECT id,name FROM wms_business.material_categories ORDER BY sort_order,name"},
        {"modules", moduleRoots + "SELECT module_id,label,product FROM roots ORDER BY product,ordinal"},
        {"warehouses", "SELECT id,name FROM wms_business.warehouses ORDER BY code"},
        {"locations", "SELECT l.id,concat_ws(' · ',l.code::text,nullif(nullif(l.name,''),l.code::text)),"
                      "l.warehouse_id FROM wms_business.locations l "
                      "JOIN wms_business.warehouses w ON w.id=l.warehouse_id ORDER BY w.code,l.code"}
    };
    QJsonObject result;
    for (const auto &list : lists) {
        QSqlQuery query(m_database.connection());
        if (!query.exec(list.second)) return error(Status::ServiceUnavailable, QStringLiteral("库存筛选项读取失败"));
        QJsonArray options;
        while (query.next()) {
            QJsonObject option{{"id",query.value(0).toString()}, {"label",query.value(1).toString()}};
            if (list.first == "modules") option.insert("product", query.value(2).toString());
            if (list.first == "locations") option.insert("warehouse", query.value(2).toString());
            options.append(option);
        }
        result.insert(list.first, options);
    }
    return QHttpServerResponse(result);
}

QHttpServerResponse LanServer::publicStock(const QHttpServerRequest &request)
{
    const auto params = request.query();
    QString term = params.queryItemValue("q", QUrl::FullyDecoded).trimmed();
    bool valid = true;
    const int offset = params.hasQueryItem("offset") ? params.queryItemValue("offset").toInt(&valid) : 0;
    constexpr int pageSize = 40;
    if (!valid || offset < 0 || term.size() > 200)
        return error(Status::BadRequest, QStringLiteral("查询条件无效"));
    if (!m_database.ready())
        return error(Status::ServiceUnavailable, QStringLiteral("库存数据库暂时不可用"));

    QString sql = moduleRoots + QStringLiteral(R"SQL(
        , tree AS (
            SELECT module_id,label,node FROM roots
            UNION ALL
            SELECT t.module_id,t.label,child.node FROM tree t
            CROSS JOIN LATERAL jsonb_array_elements(t.node->'items') child(node)
        ), membership AS (
            SELECT upper(trim(node->>'code')) AS code,
                   array_agg(DISTINCT module_id) AS ids,
                   string_agg(DISTINCT label, '、' ORDER BY label) AS labels
            FROM tree WHERE nullif(trim(node->>'code'), '') IS NOT NULL
            GROUP BY upper(trim(node->>'code'))
        )
    )SQL") + QStringLiteral(
        "SELECT m.code, m.name, m.specification, w.name, l.code, s.batch_no, s.quantity::text, m.unit, "
        "coalesce(c.name,'未分类'), coalesce(mb.labels,'未分模块'),l.name "
        "FROM wms_business.stock_balances s "
        "JOIN wms_business.materials m ON m.id=s.material_id "
        "LEFT JOIN wms_business.material_categories c ON c.id=m.category_id "
        "LEFT JOIN membership mb ON mb.code=upper(m.code::text) "
        "JOIN wms_business.warehouses w ON w.id=s.warehouse_id "
        "JOIN wms_business.locations l ON l.id=s.location_id "
        "WHERE s.quantity>0 AND m.is_active=1 ");
    QVariantList filters;
    const QList<QPair<QString, QString>> idFilters = {
        {"category", "coalesce(m.category_id,0)"}, {"warehouse", "w.id"}, {"location", "l.id"}
    };
    for (const auto &filter : idFilters) {
        if (!params.hasQueryItem(filter.first)) continue;
        bool ok;
        const auto id = params.queryItemValue(filter.first).toLongLong(&ok);
        if (!ok || id < 0) return error(Status::BadRequest, QStringLiteral("筛选条件无效"));
        sql += "AND " + filter.second + "=? ";
        filters.append(id);
    }
    const QString module = params.queryItemValue("module");
    if (module == "none") {
        sql += "AND mb.ids IS NULL ";
    } else if (!module.isEmpty()) {
        if (module.size() != 32) return error(Status::BadRequest, QStringLiteral("模块参数无效"));
        sql += "AND ?=ANY(mb.ids) ";
        filters.append(module);
    }
    if (!term.isEmpty())
        sql += QStringLiteral("AND (m.code::text ILIKE ? OR m.name ILIKE ? OR m.specification ILIKE ? "
                              "OR w.name ILIKE ? OR l.code::text ILIKE ? OR l.name ILIKE ? OR s.batch_no ILIKE ?) ");
    sql += QStringLiteral("ORDER BY m.code, w.code, l.code, s.batch_no, s.id LIMIT ? OFFSET ?");
    QSqlQuery query(m_database.connection());
    query.prepare(sql);
    for (const auto &value : filters) query.addBindValue(value);
    if (!term.isEmpty()) {
        term.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_");
        const QString pattern = '%' + term + '%';
        for (int i = 0; i < 7; ++i) query.addBindValue(pattern);
    }
    query.addBindValue(pageSize + 1);
    query.addBindValue(offset);
    if (!query.exec()) return error(Status::ServiceUnavailable, QStringLiteral("库存查询失败"));

    const QStringList keys = {"code", "name", "spec", "warehouse", "location", "batch", "quantity", "unit",
                              "category", "modules", "locationName"};
    QJsonArray items;
    while (items.size() < pageSize && query.next()) {
        QJsonObject item;
        for (int i = 0; i < keys.size(); ++i) item.insert(keys[i], query.value(i).toString());
        items.append(item);
    }
    return QHttpServerResponse(QJsonObject{{"items", items}, {"hasMore", query.next()},
        {"pageSize", pageSize}, {"fetchedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
}

void LanServer::prune()
{
    const auto now = QDateTime::currentDateTimeUtc();
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (it->expires <= now) it = m_sessions.erase(it); else ++it;
    }
    for (auto it = m_attempts.begin(); it != m_attempts.end();) {
        if (it->until <= now) it = m_attempts.erase(it); else ++it;
    }
}

QByteArray LanServer::tokenKey(const QHttpServerRequest &request) const
{
    const auto header = request.value("Authorization");
    return header.startsWith("Bearer ") ? digest(header.mid(7)) : QByteArray();
}

QHttpServerResponse LanServer::login(const QHttpServerRequest &request)
{
    prune();
    if (request.body().size() > 16384) return error(Status::BadRequest, QStringLiteral("登录请求过大"));
    const auto body = QJsonDocument::fromJson(request.body());
    if (!body.isObject()) return error(Status::BadRequest, QStringLiteral("无效的登录请求"));
    const auto object = body.object();
    if (object.value("product").toString() != m_database.product())
        return error(Status::Conflict, QStringLiteral("服务器所属系统不匹配，请检查服务器地址"));
    const QString username = object.value("username").toString().trimmed();
    const QString password = object.value("password").toString();
    if (username.isEmpty() || username.size() > 128 || password.isEmpty() || password.size() > 1024)
        return error(Status::BadRequest, QStringLiteral("请输入有效的用户名和密码"));
    const QString peer = request.remoteAddress().toString();
    if (m_attempts.size() >= 4096 || m_sessions.size() >= 1024)
        return error(Status::ServiceUnavailable, QStringLiteral("服务器繁忙，请稍后重试"));
    auto &attempt = m_attempts[peer];
    if (!attempt.until.isValid()) attempt.until = QDateTime::currentDateTimeUtc().addSecs(60);
    if (++attempt.count > 10) return error(Status::TooManyRequests, QStringLiteral("登录尝试过多，请一分钟后重试"));
    if (!m_database.ready()) return error(Status::ServiceUnavailable, QStringLiteral("数据库暂时不可用"));
    QSqlQuery user(m_database.connection());
    user.prepare("SELECT id,display_name,password_hash FROM wms_lan.users WHERE lower(username)=lower(?) AND is_active");
    user.addBindValue(username);
    if (!user.exec()) return error(Status::ServiceUnavailable, QStringLiteral("登录服务暂时不可用"));
    if (!user.next() || !PasswordHasher::verifyPassword(password, user.value(2).toString()))
        return error(Status::Unauthorized, QStringLiteral("用户名或密码错误"));
    const auto id = user.value(0).toLongLong();
    QSqlQuery audit(m_database.connection());
    audit.prepare("INSERT INTO wms_lan.audit_logs(user_id,action) VALUES(?,'LOGIN')");
    audit.addBindValue(id);
    if (!audit.exec()) return error(Status::ServiceUnavailable, QStringLiteral("无法记录登录操作"));
    QByteArray random(32, Qt::Uninitialized);
    for (auto &byte : random) byte = static_cast<char>(QRandomGenerator::system()->generate() & 255);
    const auto token = random.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const auto expiry = QDateTime::currentDateTimeUtc().addSecs(8 * 3600);
    m_sessions.insert(digest(token), {id, expiry});
    return QHttpServerResponse(QJsonObject{{"token", QString::fromLatin1(token)},
        {"expiresAt", expiry.toString(Qt::ISODate)}, {"displayName", user.value(1).toString()},
        {"product", m_database.product()}, {"apiVersion", 1}});
}

QHttpServerResponse LanServer::inventory(const QHttpServerRequest &request)
{
    prune();
    const auto session = m_sessions.constFind(tokenKey(request));
    if (session == m_sessions.cend()) return error(Status::Unauthorized, QStringLiteral("登录已过期，请重新登录"));
    if (!m_database.ready()) return error(Status::ServiceUnavailable, QStringLiteral("数据库暂时不可用"));
    QSqlQuery permission(m_database.connection());
    permission.prepare("SELECT is_active,can_view_inventory FROM wms_lan.users WHERE id=?");
    permission.addBindValue(session->userId);
    if (!permission.exec()) return error(Status::ServiceUnavailable, QStringLiteral("权限查询失败"));
    if (!permission.next() || !permission.value(0).toBool()) {
        m_sessions.remove(tokenKey(request));
        return error(Status::Unauthorized, QStringLiteral("账号已停用，请重新登录"));
    }
    if (!permission.value(1).toBool()) return error(Status::Forbidden, QStringLiteral("没有查看库存的权限"));
    const auto params = request.query();
    bool limitOk = true, offsetOk = true;
    const int limit = params.hasQueryItem("limit") ? params.queryItemValue("limit").toInt(&limitOk) : 100;
    const int offset = params.hasQueryItem("offset") ? params.queryItemValue("offset").toInt(&offsetOk) : 0;
    QString keyword = params.queryItemValue("q");
    if (!limitOk || !offsetOk || limit < 1 || limit > 200 || offset < 0 || keyword.size() > 200)
        return error(Status::BadRequest, QStringLiteral("查询条件或分页参数无效"));
    keyword.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_");
    keyword = '%' + keyword + '%';
    QSqlQuery query(m_database.connection());
    query.prepare("SELECT s.id::text,m.code,m.name,m.specification,m.unit,w.name,l.code,s.batch_no,"
                  "s.quantity::text,m.minimum_stock::text,to_char(s.updated_at,'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                  "FROM wms_lan.stock_balances s JOIN wms_lan.materials m ON m.id=s.material_id "
                  "JOIN wms_lan.warehouses w ON w.id=s.warehouse_id JOIN wms_lan.locations l ON l.id=s.location_id "
                  "WHERE m.code ILIKE ? OR m.name ILIKE ? OR m.specification ILIKE ? OR s.batch_no ILIKE ? "
                  "ORDER BY m.code,w.code,l.code,s.batch_no,s.id LIMIT ? OFFSET ?");
    for (int i=0; i<4; ++i) query.addBindValue(keyword);
    query.addBindValue(limit + 1);
    query.addBindValue(offset);
    if (!query.exec()) return error(Status::ServiceUnavailable, QStringLiteral("库存查询失败"));
    QJsonArray rows;
    bool more = false;
    const QStringList keys = {"id","code","name","specification","unit","warehouse","location","batch","quantity","minimumStock","updatedAt"};
    while (query.next()) {
        if (rows.size() == limit) { more = true; break; }
        QJsonObject row;
        for (int i=0; i<keys.size(); ++i) row.insert(keys[i], query.value(i).toString());
        rows.append(row);
    }
    return QHttpServerResponse(QJsonObject{{"items", rows}, {"offset", offset}, {"hasMore", more},
        {"product", m_database.product()}, {"fetchedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
}
