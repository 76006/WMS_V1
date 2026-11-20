#include "network/NetworkWindow.h"
#include "core/SystemProfile.h"
#include "network/RemoteBackend.h"
#include "ui/MainWindow.h"
#include "services/InventoryService.h"
#include "services/OfficeTemplateService.h"
#include <QSqlQuery>
#include <QUuid>
#include <QFile>

#include <QApplication>
#include <QDateTime>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QDebug>
#include <QLineEdit>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>

NetworkWindow::NetworkWindow(const QString &serverUrl, QWidget *parent) : QWidget(parent)
{
    setWindowTitle(SystemProfile::windowTitle() + QStringLiteral(" · 联网登录"));
    resize(580, 310);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral("连接服务器后进入完整仓库管理界面，所有业务数据由服务器统一保存。"), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    QSettings settings;
    m_address = new QLineEdit(serverUrl.isEmpty()
        ? settings.value("network/serverUrl", SystemProfile::isMeiJingLing()
            ? "http://127.0.0.1:18081" : "http://127.0.0.1:18080").toString() : serverUrl, this);
    m_address->setPlaceholderText(QStringLiteral("例如 http://192.168.1.100:18080"));
    m_username = new QLineEdit(settings.value("network/username", "admin").toString(), this);
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    if(QUrl(m_address->text()).host()=="127.0.0.1")
        m_password->setText(qEnvironmentVariable("WMS_DEMO_LOGIN_PASSWORD"));
    form->addRow(QStringLiteral("服务器地址"), m_address);
    form->addRow(QStringLiteral("用户名"), m_username);
    form->addRow(QStringLiteral("密码"), m_password);
    layout->addLayout(form);
    auto *buttons = new QHBoxLayout;
    m_login = new QPushButton(QStringLiteral("连接并登录"), this);
    buttons->addWidget(m_login);
    buttons->addStretch();
    layout->addLayout(buttons);
    m_status = new QLabel(QStringLiteral("尚未连接"), this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    connect(m_login, &QPushButton::clicked, this, &NetworkWindow::login);
    connect(m_password, &QLineEdit::returnPressed, this, &NetworkWindow::login);
    setBusy(false);
}

void NetworkWindow::setBusy(bool busy)
{
    m_busy = busy;
    const bool loggedIn = !m_token.isEmpty();
    m_login->setEnabled(!busy && !loggedIn);
    for (auto *edit : {m_address,m_username,m_password}) edit->setEnabled(!busy && !loggedIn);

}

void NetworkWindow::request(const QString &path, const QJsonObject *body,
                           std::function<void(int,const QJsonObject &,const QString &)> done)
{
    setBusy(true);
    QUrl url = m_baseUrl.resolved(QUrl(path));
    QNetworkRequest req(url);
    req.setTransferTimeout(10000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setRawHeader("Accept", "application/json");
    if (!m_token.isEmpty()) req.setRawHeader("Authorization", "Bearer " + m_token);
    QNetworkReply *reply;
    if (body) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_network.post(req, QJsonDocument(*body).toJson(QJsonDocument::Compact));
    } else reply = m_network.get(req);
    m_pending = reply;
    QTimer::singleShot(10000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this,reply,done=std::move(done)] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        QString message;
        if (reply->error() != QNetworkReply::NoError || status != 200 || !doc.isObject())
            message = doc.object().value("error").toString(QStringLiteral("连接失败或服务器响应无效，请检查地址与网络"));
        m_pending.clear();
        reply->deleteLater();
        setBusy(false);
        done(status, doc.object(), message);
    });
}

void NetworkWindow::login()
{
    if (m_busy || !m_token.isEmpty()) return;
    const QUrl url(m_address->text().trimmed());
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty()
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
        || (!url.path().isEmpty() && url.path() != "/") || url.port(80)<1) {
        m_status->setText(QStringLiteral("请输入完整服务器地址，例如 http://192.168.1.100:18080"));
        if (m_smoke) QApplication::exit(2);
        return;
    }
    if (m_username->text().trimmed().isEmpty() || m_password->text().isEmpty()) {
        m_status->setText(QStringLiteral("请输入用户名和密码"));
        if (m_smoke) QApplication::exit(2);
        return;
    }
    m_baseUrl = url;
    const QJsonObject body{{"username",m_username->text().trimmed()}, {"password",m_password->text()},
        {"product",SystemProfile::profileId()}};
    m_status->setText(QStringLiteral("正在连接服务器…"));
    request("/api/v2/login", &body, [this](int, const QJsonObject &data, const QString &error) {
        m_password->clear();
        if (!error.isEmpty()) {
            m_status->setText(error);
            if (m_smoke) QApplication::exit(3);
            return;
        }
        if (data.value("product").toString() != SystemProfile::profileId()
            || data.value("apiVersion").toInt() != 2 || data.value("token").toString().isEmpty()) {
            m_status->setText(QStringLiteral("服务器系统或接口版本不匹配"));
            if (m_smoke) QApplication::exit(4);
            return;
        }
        m_token = data.value("token").toString().toLatin1();
        if (!m_smoke) {
            QSettings settings;
            settings.setValue("network/serverUrl",m_baseUrl.toString());
            settings.setValue("network/username",m_username->text().trimmed());
        }
        Session session;
        session.userId=data.value("userId").toString().toLongLong();
        session.username=data.value("username").toString();
        session.displayName=data.value("displayName").toString();
        session.roleCode=data.value("roleCode").toString();
        session.testPasswordDefaults = data.value("testPasswordDefaults").toBool();
        for(const auto &permission:data.value("permissions").toArray())session.permissions.insert(permission.toString());
        auto database=RemoteBackend::connect(m_baseUrl,m_token);
        auto *window=new MainWindow(database,session,m_baseUrl.toString());
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->show();hide();
        const auto connectionName=database.connectionName();
        connect(window,&QObject::destroyed,this,[this,connectionName] {
            QTimer::singleShot(0,this,[this,connectionName]{
                QSqlDatabase::removeDatabase(connectionName);
                QJsonObject body;
                request("/api/v2/logout",&body,[this](int,const QJsonObject &,const QString &){
                    m_token.clear();m_status->setText(QStringLiteral("已退出登录"));setBusy(false);show();
                });
            });
            show();
        });
        if(m_smoke)QTimer::singleShot(0,this,[window,database,session]{
            for(int index=0;index<23;++index)QMetaObject::invokeMethod(window,"showPage",Qt::DirectConnection,Q_ARG(int,index));
            if(qEnvironmentVariableIsSet("WMS_TEST_UI_WRITE")) {
                QString error;
                QSqlQuery lookup(database);
                lookup.exec("SELECT m.id,w.id,l.id FROM materials m CROSS JOIN warehouses w JOIN locations l ON l.warehouse_id=w.id WHERE m.code='TEST-RAW' AND w.code='TEST-A'");
                if(lookup.next()) {
                    StockMovementRequest line;line.materialId=lookup.value(0).toLongLong();line.warehouseId=lookup.value(1).toLongLong();line.locationId=lookup.value(2).toLongLong();line.quantity=1;line.batchNo="UI-SMOKE";
                    StockDocumentRequest request;request.documentType="QTRK";request.documentDate=QDate::currentDate();request.handlerName="UI integration";request.lines.append(line);request.submissionToken=QUuid::createUuid().toString();
                    InventoryService service(database,session.userId);PostedDocument posted;PostedDocumentEdit edit;
                    if(service.postStockDocument(request,true,&posted,&error) && service.loadPostedDocument(posted.documentId,&edit,&error)) {
                        edit.lines[0].quantity=2;
                        if(service.revisePostedDocument(edit,&error)) {
                            OfficeTemplateService::materialLines(database,request.lines,&error);
                            if(error.isEmpty())service.reversePostedDocument(posted.documentId,"UI integration","UI integration",nullptr,&error);
                        }
                    }
                } else error="Missing UI integration fixtures";
                if(!error.isEmpty()) {
                    auto errors=QCoreApplication::instance()->property("networkErrors").toStringList();errors.append(error);QCoreApplication::instance()->setProperty("networkErrors",errors);
                    QFile log(qEnvironmentVariable("WMS_NETWORK_LOG"));if(log.open(QIODevice::WriteOnly|QIODevice::Append))log.write(error.toUtf8());
                }
            }
            QApplication::exit(QCoreApplication::instance()->property("networkErrors").toStringList().isEmpty()?0:9);
        });
    });
}

void NetworkWindow::startSmokeCheck()
{
    m_smoke = true;
    auto *dialogs=new QTimer(this);
    connect(dialogs,&QTimer::timeout,this,[]{
        for(auto *widget:QApplication::topLevelWidgets()) {
            if(auto *box=qobject_cast<QMessageBox *>(widget);box && box->isVisible()) {
                qWarning().noquote()<<box->text();
                auto errors=QCoreApplication::instance()->property("networkErrors").toStringList();
                errors.append(box->text());QCoreApplication::instance()->setProperty("networkErrors",errors);
                box->reject();
            }
        }
    });
    dialogs->start(100);
    m_username->setText(qEnvironmentVariable("WMS_TEST_USERNAME", "admin"));
    m_password->setText(qEnvironmentVariable("WMS_TEST_PASSWORD"));
    QTimer::singleShot(120000, this, [] { QApplication::exit(8); });
    QTimer::singleShot(0, this, [this] { login(); });
}
