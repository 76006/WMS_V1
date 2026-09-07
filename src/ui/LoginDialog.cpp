#include "ui/LoginDialog.h"

#include "core/PasswordHasher.h"

#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QVBoxLayout>

LoginDialog::LoginDialog(QSqlDatabase database,
                         const QString &databaseFilePath,
                         QWidget *parent)
    : QDialog(parent), m_database(std::move(database))
{
    setWindowTitle(QStringLiteral("冰美肌库存管理 - 登录"));
    setModal(true);
    setMinimumWidth(430);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(42, 36, 42, 32);
    root->setSpacing(14);

    auto *title = new QLabel(QStringLiteral("冰美肌库存管理"), this);
    title->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 600; color: #111827;"));
    auto *subtitle = new QLabel(QStringLiteral("请输入账号和密码"), this);
    subtitle->setObjectName(QStringLiteral("mutedText"));
    root->addWidget(title);
    root->addWidget(subtitle);
    root->addSpacing(10);

    auto *form = new QFormLayout;
    form->setSpacing(12);
    m_usernameEdit = new QLineEdit(this);
    m_usernameEdit->setPlaceholderText(QStringLiteral("用户名"));
    m_usernameEdit->setText(QStringLiteral("admin"));
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(QStringLiteral("密码"));
    form->addRow(QStringLiteral("用户名"), m_usernameEdit);
    form->addRow(QStringLiteral("密码"), m_passwordEdit);
    root->addLayout(form);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #b91c1c;"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->hide();
    root->addWidget(m_errorLabel);

    m_loginButton = new QPushButton(QStringLiteral("登录"), this);
    m_loginButton->setProperty("primary", true);
    m_loginButton->setDefault(true);
    root->addWidget(m_loginButton);

    auto *firstUse = new QLabel(QStringLiteral("首次使用：admin / Admin@123，登录后请及时修改密码。"), this);
    firstUse->setObjectName(QStringLiteral("mutedText"));
    firstUse->setWordWrap(true);
    root->addWidget(firstUse);

    auto *databaseLabel = new QLabel(
        QStringLiteral("数据库文件：%1").arg(QFileInfo(databaseFilePath).absoluteFilePath()), this);
    databaseLabel->setObjectName(QStringLiteral("mutedText"));
    databaseLabel->setWordWrap(true);
    databaseLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(databaseLabel);

    connect(m_loginButton, &QPushButton::clicked, this, &LoginDialog::authenticate);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &LoginDialog::authenticate);
    m_passwordEdit->setFocus();
}

Session LoginDialog::session() const
{
    return m_session;
}

void LoginDialog::authenticate()
{
    const QString username = m_usernameEdit->text().trimmed();
    const QString password = m_passwordEdit->text();
    if (username.isEmpty() || password.isEmpty()) {
        m_errorLabel->setText(QStringLiteral("请输入用户名和密码。"));
        m_errorLabel->show();
        return;
    }

    m_loginButton->setEnabled(false);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT u.id, u.username, u.display_name, u.password_hash, r.code "
        "FROM users u JOIN roles r ON r.id=u.role_id "
        "WHERE u.username=? AND u.is_active=1"));
    query.addBindValue(username);
    if (!query.exec()) {
        m_errorLabel->setText(QStringLiteral("登录查询失败：%1").arg(query.lastError().text()));
        m_errorLabel->show();
        m_loginButton->setEnabled(true);
        return;
    }
    if (!query.next() || !PasswordHasher::verifyPassword(password, query.value(3).toString())) {
        m_errorLabel->setText(QStringLiteral("用户名或密码错误。"));
        m_errorLabel->show();
        m_passwordEdit->selectAll();
        m_loginButton->setEnabled(true);
        return;
    }

    m_session.userId = query.value(0).toLongLong();
    m_session.username = query.value(1).toString();
    m_session.displayName = query.value(2).toString();
    m_session.roleCode = query.value(4).toString();

    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE users SET last_login_at=? WHERE id=?"));
    update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    update.addBindValue(m_session.userId);
    update.exec();

    QSqlQuery audit(m_database);
    audit.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id, action, entity_type, entity_id, detail) "
        "VALUES(?, 'LOGIN', 'user', ?, '')"));
    audit.addBindValue(m_session.userId);
    audit.addBindValue(m_session.userId);
    audit.exec();
    accept();
}

