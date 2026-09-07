#include "ui/pages/SystemSettingsPage.h"
#include "services/UserService.h"
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <utility>

SystemSettingsPage::SystemSettingsPage(QSqlDatabase database, Session session,
                                       const QString &databaseFilePath, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QVBoxLayout(panel);
    layout->addWidget(new QLabel(QStringLiteral("当前用户与数据库"), panel));
    auto *info = new QFormLayout;
    info->addRow(QStringLiteral("当前用户"), new QLabel(
        QStringLiteral("%1（%2 / %3）").arg(m_session.displayName, m_session.username, m_session.roleCode), panel));
    auto *path = new QLabel(databaseFilePath, panel);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setWordWrap(true);
    info->addRow(QStringLiteral("数据库文件"), path);
    layout->addLayout(info);
    layout->addSpacing(12);
    layout->addWidget(new QLabel(QStringLiteral("修改密码"), panel));
    auto *form = new QFormLayout;
    m_currentPassword = new QLineEdit(panel);
    m_newPassword = new QLineEdit(panel);
    m_confirmPassword = new QLineEdit(panel);
    for (QLineEdit *edit : {m_currentPassword, m_newPassword, m_confirmPassword})
        edit->setEchoMode(QLineEdit::Password);
#ifndef NDEBUG
    if (m_session.isAdministrator()) m_currentPassword->setPlaceholderText(QStringLiteral("Debug测试管理员可留空"));
#endif
    form->addRow(QStringLiteral("当前密码"), m_currentPassword);
    form->addRow(QStringLiteral("新密码（至少8位）"), m_newPassword);
    form->addRow(QStringLiteral("确认新密码"), m_confirmPassword);
    layout->addLayout(form);
    auto *change = new QPushButton(QStringLiteral("修改密码"), panel);
    change->setProperty("primary", true);
    layout->addWidget(change, 0, Qt::AlignRight);
    root->addWidget(panel);
    root->addStretch();
    connect(change, &QPushButton::clicked, this, &SystemSettingsPage::changePassword);
}

void SystemSettingsPage::changePassword()
{
    if (m_newPassword->text() != m_confirmPassword->text()) {
        QMessageBox::warning(this, QStringLiteral("密码不一致"), QStringLiteral("两次输入的新密码不一致。"));
        return;
    }
    UserService service(m_database, m_session.userId);
    QString error;
    if (!service.changeOwnPassword(m_currentPassword->text(), m_newPassword->text(), &error)) {
        QMessageBox::warning(this, QStringLiteral("修改失败"), error);
        return;
    }
    m_currentPassword->clear();
    m_newPassword->clear();
    m_confirmPassword->clear();
    QMessageBox::information(this, QStringLiteral("修改完成"), QStringLiteral("密码已更新。"));
}
