#include "network/RemoteBackend.h"
#include "network/QueryBridge.h"
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>
#include <QDateTime>

bool RemoteBackend::isRemote(const QSqlDatabase &database)
{
    const auto bridge=dynamic_cast<QueryBridge *>(database.driver());
    return bridge && bridge->remote();
}
QJsonObject RemoteBackend::rpc(const QSqlDatabase &database,const QString &method,const QJsonObject &arguments,QString *error)
{
    const auto bridge=dynamic_cast<QueryBridge *>(database.driver());
    auto response=bridge->call({{"kind","rpc"},{"method",method},{"arguments",arguments}});
    if(error) *error=response.value("error").toString();
    return response;
}
QSqlDatabase RemoteBackend::connect(const QUrl &url,const QByteArray &token)
{
    auto executor=[url,token,lastFailure=qint64(0),lastError=QJsonObject{}](QJsonObject payload) mutable {
        if(QDateTime::currentMSecsSinceEpoch()-lastFailure<2000)return lastError;
        payload.insert("requestId",QUuid::createUuid().toString(QUuid::WithoutBraces));
        // The same ID is reused after a lost response. Server stores committed write results.
        for(int attempt=0;attempt<2;++attempt) {
            QNetworkAccessManager manager;
            QNetworkRequest request(url.resolved(QUrl("/api/v2/data")));
            request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
            request.setRawHeader("Authorization","Bearer "+token);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
            request.setTransferTimeout(30000);
            auto *reply=manager.post(request,QJsonDocument(payload).toJson(QJsonDocument::Compact));
            QEventLoop loop;
            QTimer deadline; deadline.setSingleShot(true); deadline.start(30000);
            QObject::connect(&deadline,&QTimer::timeout,reply,&QNetworkReply::abort);
            QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
            loop.exec(QEventLoop::ExcludeUserInputEvents);
            const auto doc=QJsonDocument::fromJson(reply->readAll());
            if(doc.isObject()) return doc.object();
            if(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==401)
                return QJsonObject{{"error",QStringLiteral("登录已失效，请关闭窗口并重新登录。")}};
        }
        lastFailure=QDateTime::currentMSecsSinceEpoch();
        lastError={{"error",QStringLiteral("连接服务器失败。本次提交结果未确认，请恢复连接后刷新单据列表，避免重复录单。")}};
        return lastError;
    };
    auto database=QSqlDatabase::addDatabase(new QueryBridge(executor,true),"remote_"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    database.open(); return database;
}
