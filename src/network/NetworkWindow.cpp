#include "network/NetworkWindow.h"
#include "core/SystemProfile.h"

#include <QApplication>
#include <QDateTime>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
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
    setWindowTitle(SystemProfile::windowTitle() + QStringLiteral(" · 联网库存查询"));
    resize(1180, 720);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral("局域网试用：登录与库存查询。当前连接测试数据库，出入库等业务将在后续版本接入。"), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    QSettings settings;
    m_address = new QLineEdit(serverUrl.isEmpty()
        ? settings.value("network/serverUrl", "http://127.0.0.1:18080").toString() : serverUrl, this);
    m_address->setPlaceholderText(QStringLiteral("例如 http://192.168.1.100:18080"));
    m_username = new QLineEdit(settings.value("network/username", "admin").toString(), this);
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("服务器地址"), m_address);
    form->addRow(QStringLiteral("用户名"), m_username);
    form->addRow(QStringLiteral("密码"), m_password);
    layout->addLayout(form);
    auto *buttons = new QHBoxLayout;
    m_login = new QPushButton(QStringLiteral("连接并登录"), this);
    m_logout = new QPushButton(QStringLiteral("退出登录"), this);
    buttons->addWidget(m_login);
    buttons->addWidget(m_logout);
    buttons->addStretch();
    layout->addLayout(buttons);
    m_status = new QLabel(QStringLiteral("尚未连接"), this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto *search = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("物料编码、名称、规格或批次"));
    m_refresh = new QPushButton(QStringLiteral("查询 / 刷新"), this);
    m_previous = new QPushButton(QStringLiteral("上一页"), this);
    m_next = new QPushButton(QStringLiteral("下一页"), this);
    search->addWidget(m_search, 1);
    search->addWidget(m_refresh);
    search->addWidget(m_previous);
    search->addWidget(m_next);
    layout->addLayout(search);
    m_table = new QTableWidget(0, 10, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("物料编码"),QStringLiteral("物料名称"),
        QStringLiteral("规格"),QStringLiteral("单位"),QStringLiteral("仓库"),QStringLiteral("库位"),
        QStringLiteral("批次"),QStringLiteral("库存数量"),QStringLiteral("最低库存"),QStringLiteral("更新时间（UTC）")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table, 1);
    m_timer = new QTimer(this);
    m_timer->setInterval(15000);
    connect(m_timer, &QTimer::timeout, this, [this] { if (!m_busy) refresh(); });
    connect(m_login, &QPushButton::clicked, this, &NetworkWindow::login);
    connect(m_password, &QLineEdit::returnPressed, this, &NetworkWindow::login);
    connect(m_logout, &QPushButton::clicked, this, &NetworkWindow::logout);
    const auto searchNow = [this] { if (!m_busy) { m_offset = 0; refresh(); } };
    connect(m_refresh, &QPushButton::clicked, this, searchNow);
    connect(m_search, &QLineEdit::returnPressed, this, searchNow);
    connect(m_previous, &QPushButton::clicked, this, [this] { m_offset = qMax(0, m_offset-100); refresh(); });
    connect(m_next, &QPushButton::clicked, this, [this] { m_offset += 100; refresh(); });
    setBusy(false);
}

void NetworkWindow::setBusy(bool busy)
{
    m_busy = busy;
    const bool loggedIn = !m_token.isEmpty();
    m_login->setEnabled(!busy && !loggedIn);
    for (auto *edit : {m_address,m_username,m_password}) edit->setEnabled(!busy && !loggedIn);
    m_refresh->setEnabled(!busy && loggedIn);
    m_search->setEnabled(!busy && loggedIn);
    m_previous->setEnabled(!busy && loggedIn && m_offset>0);
    m_next->setEnabled(!busy && loggedIn && m_more);
    m_logout->setEnabled(!busy && loggedIn);
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
    request("/api/v1/login", &body, [this](int, const QJsonObject &data, const QString &error) {
        m_password->clear();
        if (!error.isEmpty()) {
            m_status->setText(error);
            if (m_smoke) QApplication::exit(3);
            return;
        }
        if (data.value("product").toString() != SystemProfile::profileId()
            || data.value("apiVersion").toInt() != 1 || data.value("token").toString().isEmpty()) {
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
        m_offset = 0;
        m_timer->start();
        refresh();
    });
}

void NetworkWindow::refresh()
{
    if (m_busy || m_token.isEmpty()) return;
    QUrl url("/api/v1/inventory");
    QUrlQuery query;
    query.addQueryItem("q",m_search->text().trimmed());
    query.addQueryItem("limit","100");
    query.addQueryItem("offset",QString::number(m_offset));
    url.setQuery(query);
    m_status->setText(QStringLiteral("正在获取最新库存…"));
    request(url.toString(QUrl::FullyEncoded), nullptr, [this](int status, const QJsonObject &data, const QString &error) {
        m_table->setRowCount(0);
        m_more = false;
        if (!error.isEmpty()) {
            if (status == 401) { m_token.clear(); m_timer->stop(); }
            m_status->setText(QStringLiteral("库存未更新：%1").arg(error));
            setBusy(false);
            if (m_smoke) QApplication::exit(5);
            return;
        }
        if (data.value("product").toString() != SystemProfile::profileId() || !data.value("items").isArray()) {
            m_status->setText(QStringLiteral("库存响应格式或系统身份不匹配"));
            m_token.clear(); m_timer->stop(); setBusy(false);
            if (m_smoke) QApplication::exit(6);
            return;
        }
        const auto rows = data.value("items").toArray();
        const QStringList keys={"code","name","specification","unit","warehouse","location","batch","quantity","minimumStock","updatedAt"};
        m_table->setRowCount(rows.size());
        for (int r=0;r<rows.size();++r)
            for (int c=0;c<keys.size();++c)
                m_table->setItem(r,c,new QTableWidgetItem(rows[r].toObject().value(keys[c]).toString()));
        m_table->resizeColumnsToContents();
        m_more = data.value("hasMore").toBool();
        m_status->setText(QStringLiteral("已连接 · 第 %1 页，本页 %2 条 · 最后刷新 %3 · 每15秒自动刷新")
            .arg(m_offset/100+1).arg(rows.size()).arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
        setBusy(false);
        if (m_smoke) QApplication::exit(rows.isEmpty() ? 7 : 0);
    });
}

void NetworkWindow::logout()
{
    if (m_busy) return;
    m_timer->stop();
    QJsonObject body;
    request("/api/v1/logout", &body, [this](int, const QJsonObject &, const QString &) {
        m_token.clear(); m_table->setRowCount(0); m_more=false;
        m_status->setText(QStringLiteral("已退出登录")); setBusy(false);
    });
}

void NetworkWindow::startSmokeCheck()
{
    m_smoke = true;
    m_username->setText(qEnvironmentVariable("WMS_TEST_USERNAME", "admin"));
    m_password->setText(qEnvironmentVariable("WMS_TEST_PASSWORD"));
    QTimer::singleShot(30000, this, [] { QApplication::exit(8); });
    QTimer::singleShot(0, this, [this] { login(); });
}
