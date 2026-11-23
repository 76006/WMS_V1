#include "server/BusinessApi.h"
#include "server/BusinessRpc.h"
#include "server/TestLoginPasswords.h"
#include "core/PasswordHasher.h"
#include "network/QueryBridge.h"
#include <QCryptographicHash>
#include <QHttpServerRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>

namespace {
using Status=QHttpServerResponder::StatusCode;
QHttpServerResponse error(const QString &message,Status status=Status::BadRequest){return QHttpServerResponse(QJsonObject{{"error",message}},status);}
QByteArray hash(const QByteArray &value){return QCryptographicHash::hash(value,QCryptographicHash::Sha256).toHex();}
QString lexical(QString sql){sql.replace(QRegularExpression("'(?:''|[^'])*'"),"''");sql.remove('"');return sql;}
QString permissionForTable(const QString &table){
    if(QStringList{"materials","material_images","material_projects","material_bom_items","process_boms","material_categories"}.contains(table))return "MANAGE_MATERIALS";
    if(QStringList{"warehouses","locations"}.contains(table))return "MANAGE_WAREHOUSES";
    if(table=="role_permissions")return "MANAGE_USERS";
    if(table=="number_rules")return "MANAGE_SYSTEM";
    if(table=="sales_outbound_details")return "POST_INVENTORY";
    if(table=="attachments" || table=="document_forms")return "FORM_WRITE";
    if(table=="inspection_notices")return "POST_INVENTORY";
    if(table=="audit_logs")return "";
    return "__DENIED__";
}
}
BusinessApi::BusinessApi(QHttpServer &http,QSqlDatabase database,QString product):m_database(database),m_product(std::move(product))
{
    http.route("/api/v2/login",QHttpServerRequest::Method::Post,[this](const QHttpServerRequest &r){return login(r);});
    http.route("/api/v2/data",QHttpServerRequest::Method::Post,[this](const QHttpServerRequest &r){return data(r);});
    http.route("/api/v2/logout",QHttpServerRequest::Method::Post,[this](const QHttpServerRequest &r){m_sessions.remove(token(r));return QHttpServerResponse(QJsonObject{{"ok",true}});});
    auto *timer=new QTimer(&http);
    QObject::connect(timer,&QTimer::timeout,&http,[this]{prune();});timer->start(30000);
}
QByteArray BusinessApi::token(const QHttpServerRequest &request) const
{
    const auto auth=request.value("Authorization");return auth.startsWith("Bearer ")?hash(auth.mid(7)):QByteArray();
}
void BusinessApi::prune()
{
    const auto now=QDateTime::currentDateTimeUtc();
    for(auto i=m_sessions.begin();i!=m_sessions.end();)if(i->expires<now)i=m_sessions.erase(i);else ++i;
    for(auto i=m_attempts.begin();i!=m_attempts.end();)if(i->first<now)i=m_attempts.erase(i);else ++i;
}
QHttpServerResponse BusinessApi::login(const QHttpServerRequest &request)
{
    prune();
    if(request.body().size()>16384)return error(QStringLiteral("登录请求过大"));
    auto object=QJsonDocument::fromJson(request.body()).object();
    if(object.value("product").toString()!=m_product)return error(QStringLiteral("服务器所属系统不匹配"),Status::Conflict);
    const auto username=object.value("username").toString().trimmed(),password=object.value("password").toString();
    if(username.isEmpty() || username.size()>128 || password.isEmpty() || password.size()>1024)return error(QStringLiteral("请输入有效账号密码"));
    auto &attempt=m_attempts[request.remoteAddress().toString()];
    if(!attempt.first.isValid())attempt.first=QDateTime::currentDateTimeUtc().addSecs(60);
    if(++attempt.second>20)return error(QStringLiteral("登录尝试过多，请一分钟后重试"),Status::TooManyRequests);
    if(m_sessions.size()>=128)return error(QStringLiteral("在线会话过多"),Status::ServiceUnavailable);
    auto database=std::make_shared<BusinessDatabase>(m_database);QString message;
    if(!database->open(&message))return error(QStringLiteral("业务数据库暂时不可用"),Status::ServiceUnavailable);
    QSqlQuery identity(database->raw());
    if(!identity.exec("SELECT product FROM network_identity") || !identity.next() || identity.value(0).toString()!=m_product)
        return error(QStringLiteral("完整业务数据库尚未初始化或系统身份不匹配"),Status::ServiceUnavailable);
    QSqlQuery user(database->raw());
    user.prepare("SELECT u.id,u.username,u.display_name,u.password_hash,r.code FROM users u JOIN roles r ON r.id=u.role_id WHERE u.username=? AND u.is_active=1");user.addBindValue(username);
    if(!user.exec() || !user.next() || !PasswordHasher::verifyPassword(password,user.value(3).toString()))return error(QStringLiteral("用户名或密码错误"),Status::Unauthorized);
    const auto id=user.value(0).toLongLong();
    QSqlQuery actor(database->raw());actor.prepare("SELECT set_config('wms.user_id',?,false)");actor.addBindValue(QString::number(id));
    if(!actor.exec())return error(QStringLiteral("无法建立业务会话"),Status::ServiceUnavailable);
    QSqlQuery permissions(database->raw());permissions.prepare("SELECT permission_code FROM role_permissions WHERE role_id=(SELECT role_id FROM users WHERE id=?) AND is_allowed=1");permissions.addBindValue(id);
    if(!permissions.exec())return error(QStringLiteral("权限读取失败"),Status::ServiceUnavailable);
    QJsonArray codes;while(permissions.next())codes.append(permissions.value(0).toString());
    QByteArray bytes(32,Qt::Uninitialized);for(auto &b:bytes)b=static_cast<char>(QRandomGenerator::system()->generate()&255);
    const auto secret=bytes.toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals);
    m_sessions.insert(hash(secret),{id,QDateTime::currentDateTimeUtc().addSecs(8*3600),database,{}});
    QSqlQuery audit(database->raw());audit.prepare("INSERT INTO audit_logs(user_id,action,entity_type,entity_id) VALUES(?,'LOGIN','user',?)");audit.addBindValue(id);audit.addBindValue(id);audit.exec();
    return QHttpServerResponse(QJsonObject{{"apiVersion",2},{"product",m_product},{"token",QString::fromLatin1(secret)},
        {"testPasswordDefaults",TestLoginPasswords::enabled()},
        {"userId",QString::number(id)},{"username",user.value(1).toString()},{"displayName",user.value(2).toString()},{"roleCode",user.value(4).toString()},{"permissions",codes}});
}
QHttpServerResponse BusinessApi::data(const QHttpServerRequest &request)
{
    prune();
    auto session=m_sessions.find(token(request));
    if(session==m_sessions.end())return error(QStringLiteral("登录已过期，请重新登录"),Status::Unauthorized);
    if(request.body().size()>72*1024*1024)return error(QStringLiteral("请求超过大小限制"));
    auto payload=QJsonDocument::fromJson(request.body()).object();
    const auto requestId=payload.take("requestId").toString();
    if(!QRegularExpression("^[a-zA-Z0-9-]{16,64}$").match(requestId).hasMatch())return error(QStringLiteral("请求标识无效"));
    auto &database=*session->database;
    auto raw=database.raw();
    QSqlQuery user(raw);user.prepare("SELECT r.code,u.is_active FROM users u JOIN roles r ON r.id=u.role_id WHERE u.id=?");user.addBindValue(session->user);
    if(!user.exec()) {m_sessions.erase(session);return error(QStringLiteral("数据库连接已失效，请重新登录"),Status::Unauthorized);}
    if(!user.next() || !user.value(1).toBool()){m_sessions.erase(session);return error(QStringLiteral("账号已停用"),Status::Unauthorized);}
    const bool admin=user.value(0).toString()=="ADMIN";
    QSet<QString> permissions;
    QSqlQuery allowed(raw);allowed.prepare("SELECT permission_code FROM role_permissions WHERE role_id=(SELECT role_id FROM users WHERE id=?) AND is_allowed=1");allowed.addBindValue(session->user);
    if(!allowed.exec())return error(QStringLiteral("权限读取失败"),Status::ServiceUnavailable);
    while(allowed.next())permissions.insert(allowed.value(0).toString());
    const auto has=[&](const QString &code){return code!="__DENIED__" && (code.isEmpty() || admin || permissions.contains(code) || (code=="FORM_WRITE" && (permissions.contains("POST_PRODUCTION") || permissions.contains("POST_INVENTORY") || permissions.contains("MANAGE_ATTACHMENTS"))));};
    const auto fingerprint=QString::fromLatin1(hash(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
    if(session->begun.contains(requestId)) {
        if(session->begun.value(requestId)!=fingerprint)return error(QStringLiteral("事务请求标识冲突"),Status::Conflict);
        return QHttpServerResponse(QJsonObject{});
    }
    QSqlQuery cached(raw);cached.prepare("SELECT user_id,payload_hash,result::text FROM network_requests WHERE request_id=?");cached.addBindValue(requestId);
    if(!cached.exec())return error(QStringLiteral("请求记录读取失败"),Status::ServiceUnavailable);
    if(cached.next()){
        if(cached.value(0).toLongLong()!=session->user || cached.value(1).toString()!=fingerprint)return error(QStringLiteral("请求标识已用于其他操作"),Status::Conflict);
        return QHttpServerResponse(QJsonDocument::fromJson(cached.value(2).toByteArray()).object());
    }
    const bool rpc=payload.value("kind").toString()=="rpc";
    const auto method=payload.value("method").toString();
    QString sql=payload.value("sql").toString().trimmed();
    const QString cleaned=lexical(sql).toLower();
    const bool begin=cleaned=="begin" || cleaned=="begin immediate",commit=cleaned=="commit",rollback=cleaned=="rollback";
    bool write=rpc && businessPermission(method)!="VIEW_INVENTORY";
    QString required;
    if(rpc) {
        required=businessPermission(method);
        if(method=="AttachmentService.uploadDocumentAttachment")required="FORM_WRITE";
        if(method=="InventoryService.postStockDocument") {
            const auto kind=payload.value("arguments").toObject().value("request").toObject().value("documentType").toString();
            if(QStringList{"SCLL","SHLL","YFLL","QTLL"}.contains(kind))required="POST_PRODUCTION";
        }
    }
    else {
        if(payload.value("kind").toString()!="query" || sql.size()>100000 || sql.contains(';') || sql.contains("--") || sql.contains("/*") || sql.contains(QLatin1Char('"')) || sql.contains(QLatin1Char('$')) || sql.contains(QLatin1Char('\\')))return error(QStringLiteral("不支持的查询格式"));
        const bool read=cleaned.startsWith("select ") || cleaned.startsWith("with ");
        if(read){
            if(QRegularExpression("\\b(from|join)\\s+\\w+\\s*\\.").match(cleaned).hasMatch())return error(QStringLiteral("不允许跨数据库结构查询"),Status::Forbidden);
            if(QRegularExpression("\\b(insert|update|delete|drop|alter|create|copy|grant|revoke|call|set|into|password_hash|legacy_merge_rows|network_requests|network_identity|pg_[a-z_]+|information_schema)\\b").match(cleaned).hasMatch())return error(QStringLiteral("查询包含受限内容"),Status::Forbidden);
            QSet<QString> functions={"coalesce","sum","count","max","min","round","abs","cast","strftime","date","substr","substring","instr","length","replace","trim","upper","lower","group_concat","nullif","in","exists","as","over","and","or","not","where","join","from","on"};
            auto ctes=QRegularExpression("(?:with(?: recursive)?|,)\\s*([a-z_]\\w*)\\s*\\([^()]*\\)\\s+as\\s*\\(").globalMatch(cleaned);
            QSet<int> ctePositions;
            while(ctes.hasNext())ctePositions.insert(ctes.next().capturedStart(1));
            auto it=QRegularExpression("\\b([a-z_]\\w*)\\s*\\(").globalMatch(cleaned);
            while(it.hasNext()){const auto match=it.next();const auto name=match.captured(1);if(!ctePositions.contains(match.capturedStart(1)) && !functions.contains(name))return error(QStringLiteral("查询函数不在允许范围：%1").arg(name),Status::Forbidden);}
            if(cleaned.contains("audit_logs"))required="VIEW_AUDIT";
            else required="VIEW_INVENTORY";
            // A column-restricted view prevents credentials from ever reaching a desktop query.
            // Replace every unquoted users identifier, including comma joins and subqueries.
            const auto tokens=QRegularExpression("'(?:''|[^'])*'|\\busers\\b",QRegularExpression::CaseInsensitiveOption);
            auto matches=tokens.globalMatch(sql);QList<QPair<int,int>> positions;
            while(matches.hasNext()){auto m=matches.next();if(!m.captured().startsWith(QLatin1Char('\'')))positions.append({m.capturedStart(),m.capturedLength()});}
            for(auto i=positions.crbegin();i!=positions.crend();++i)sql.replace(i->first,i->second,"users_public");
            payload.insert("sql",sql);
        } else if(!begin && !commit && !rollback){
            const auto target=QRegularExpression("^(?:insert(?: or ignore)? into|update|delete from)\\s+([a-z_]+)\\b").match(cleaned);
            if(!target.hasMatch())return error(QStringLiteral("只允许基础资料维护和业务服务接口"),Status::Forbidden);
            const auto table=target.captured(1);required=permissionForTable(table);write=true;
            if(table=="audit_logs" && !cleaned.startsWith("insert"))required="__DENIED__";
            if(table=="sales_outbound_details" && (cleaned.startsWith("delete") || QRegularExpression("\\b(receipt_status|receipt_date|receipt_attachment_id|received_by)\\b").match(cleaned).hasMatch()))required="__DENIED__";
            if(table=="inspection_notices"){
                const auto set=cleaned.section(" set ",1).section(" where ",0,0);
                if(!cleaned.startsWith("update"))required="__DENIED__";
                const QSet<QString> columns={"template_file_attachment_id","template_payload","archive_path","updated_by","updated_at"};
                auto assignments=QRegularExpression("\\b([a-z_]+)\\s*=").globalMatch(set);
                while(assignments.hasNext())if(!columns.contains(assignments.next().captured(1)))required="__DENIED__";
            }
            // No arbitrary subqueries or administrative SQL in maintenance statements.
            const bool formCleanup=table=="attachments" && cleaned=="update attachments set is_deleted=1,deleted_by=?,deleted_at=? where id in (select attachment_id from document_forms where document_id=? and form_kind=?)";
            if(!formCleanup && QRegularExpression("\\b(select|with|returning|pg_[a-z_]+|information_schema)\\b").match(cleaned).hasMatch())required="__DENIED__";
            if(!formCleanup && QRegularExpression("\\b(from|join|using|table|password_hash|users|network_requests|network_identity)\\b").match(cleaned.mid(target.capturedEnd())).hasMatch())required="__DENIED__";
            // Check function calls too: maintenance must not invoke arbitrary SQL functions.
            const QSet<QString> safe={table,"values","coalesce","strftime","date","nullif","in","max","min","conflict","where","and","or","not"};
            auto calls=QRegularExpression("\\b([a-z_]\\w*)\\s*\\(").globalMatch(cleaned);
            while(calls.hasNext())if(!safe.contains(calls.next().captured(1)))required="__DENIED__";
        }
    }
    if(!has(required))return error(QStringLiteral("当前账号没有执行此操作的权限"),Status::Forbidden);
    if(rpc && !admin && QStringList{"InventoryService.reversePostedDocument","InventoryService.reverseItem","InventoryService.reverseTransfer"}.contains(method)) {
        const auto arguments=payload.value("arguments").toObject();QSqlQuery owner(raw);
        if(method=="InventoryService.reversePostedDocument") {
            owner.prepare("SELECT created_by FROM business_documents WHERE id=?");owner.addBindValue(arguments.value("documentId").toString().toLongLong());
        } else {
            owner.prepare("SELECT d.created_by FROM business_documents d JOIN business_document_items i ON i.document_id=d.id WHERE i.id=?");
            owner.addBindValue(arguments.value("request").toObject().value("sourceItemId").toString().toLongLong());
        }
        if(!owner.exec() || !owner.next() || owner.value(0).toLongLong()!=session->user)return error(QStringLiteral("只有管理员或原单创建人可以撤销单据。"),Status::Forbidden);
    }
    QString message;
    if(rollback){while(database.depth()>0)database.rollback();return QHttpServerResponse(QJsonObject{});}
    if(begin){if(!database.begin(&message))return error(message,Status::Conflict);session->begun.insert(requestId,fingerprint);return QHttpServerResponse(QJsonObject{});}
    const int initialDepth=database.depth();
    if(commit && initialDepth==0)return error(QStringLiteral("事务已失效，请重新开始操作。"));
    const bool operationTransaction=!commit && (write || initialDepth>0);
    if(operationTransaction && !database.begin(&message))return error(message,Status::Conflict);
    QJsonObject result;
    if(!commit)result=rpc?dispatchBusinessRpc(database.database(),session->user,method,payload.value("arguments").toObject()):database.execute(payload);
    if(result.contains("error")){
        if(operationTransaction)while(database.depth()>initialDepth)database.rollback();
        return QHttpServerResponse(result);
    }
    if(write || commit){
        QSqlQuery save(raw);save.prepare("INSERT INTO network_requests(request_id,user_id,payload_hash,result) VALUES(?,?,?,?::jsonb)");
        save.addBindValue(requestId);save.addBindValue(session->user);save.addBindValue(fingerprint);save.addBindValue(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
        if(!save.exec()){while(database.depth()>0)database.rollback();return error(QStringLiteral("无法保存提交确认信息，事务已回滚"));}
    }
    if(operationTransaction || commit){if(!database.commit(&message)){while(database.depth()>0)database.rollback();return error(message);}}
    return QHttpServerResponse(result);
}
