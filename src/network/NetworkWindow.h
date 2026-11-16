#pragma once

#include <QWidget>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>
#include <functional>

class QLineEdit;
class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;
class QNetworkReply;
class QJsonObject;

class NetworkWindow final : public QWidget
{
    Q_OBJECT
public:
    explicit NetworkWindow(const QString &serverUrl = {}, QWidget *parent = nullptr);
    // Used only by the opt-in offscreen integration smoke check.
    void startSmokeCheck();
private:
    void login();
    void refresh();
    void logout();
    void setBusy(bool busy);
    void request(const QString &path, const QJsonObject *body,
                 std::function<void(int, const QJsonObject &, const QString &)> done);
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_pending;
    QLineEdit *m_address, *m_username, *m_password, *m_search;
    QPushButton *m_login, *m_refresh, *m_previous, *m_next, *m_logout;
    QLabel *m_status;
    QTableWidget *m_table;
    QTimer *m_timer;
    QUrl m_baseUrl;
    QByteArray m_token;
    int m_offset = 0;
    bool m_more = false;
    bool m_busy = false;
    bool m_smoke = false;
};
