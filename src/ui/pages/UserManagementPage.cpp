#include "ui/pages/UserManagementPage.h"

#include "services/UserService.h"

#include <QAbstractItemView>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

#include <utility>

namespace { constexpr int IdRole = Qt::UserRole + 1; }

UserManagementPage::UserManagementPage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QVBoxLayout(panel);
    auto *toolbar = new QHBoxLayout;
    m_keywordEdit = new QLineEdit(panel);
    m_keywordEdit->setPlaceholderText(QStringLiteral("用户名、显示名或角色"));
    auto *search = new QPushButton(QStringLiteral("查询"), panel);
    auto *add = new QPushButton(QStringLiteral("新增用户"), panel);
    add->setProperty("primary", true);
    m_editButton = new QPushButton(QStringLiteral("编辑"), panel);
    m_resetButton = new QPushButton(QStringLiteral("重置密码"), panel);
    toolbar->addWidget(new QLabel(QStringLiteral("用户与角色"), panel));
    toolbar->addWidget(m_keywordEdit, 1);
    toolbar->addWidget(search);
    toolbar->addWidget(add);
    toolbar->addWidget(m_editButton);
    toolbar->addWidget(m_resetButton);
    layout->addLayout(toolbar);
    m_table = new QTableWidget(0, 7, panel);
    m_table->setHorizontalHeaderLabels({QStringLiteral("用户名"), QStringLiteral("显示名"),
                                        QStringLiteral("角色"), QStringLiteral("状态"),
                                        QStringLiteral("最近登录"), QStringLiteral("创建时间"),
                                        QStringLiteral("更新时间")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(m_table);
    root->addWidget(panel, 1);
    connect(search, &QPushButton::clicked, this, &UserManagementPage::refresh);
    connect(m_keywordEdit, &QLineEdit::returnPressed, this, &UserManagementPage::refresh);
    connect(add, &QPushButton::clicked, this, &UserManagementPage::addUser);
    connect(m_editButton, &QPushButton::clicked, this, &UserManagementPage::editUser);
    connect(m_resetButton, &QPushButton::clicked, this, &UserManagementPage::resetPassword);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &UserManagementPage::updateActions);
    refresh();
}

qlonglong UserManagementPage::selectedUserId() const
{
    return m_table->currentRow() < 0 ? 0 : m_table->item(m_table->currentRow(), 0)->data(IdRole).toLongLong();
}

void UserManagementPage::refresh()
{
    m_table->setRowCount(0);
    const QString keyword = QStringLiteral("%%1%").arg(m_keywordEdit->text().trimmed());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT u.id,u.username,u.display_name,r.code||' - '||r.name,u.is_active,u.last_login_at,"
        "u.created_at,u.updated_at FROM users u JOIN roles r ON r.id=u.role_id "
        "WHERE u.username LIKE ? OR u.display_name LIKE ? OR r.code LIKE ? OR r.name LIKE ? "
        "ORDER BY u.id"));
    for (int i = 0; i < 4; ++i) query.addBindValue(keyword);
    query.exec();
    while (query.next()) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *username = new QTableWidgetItem(query.value(1).toString());
        username->setData(IdRole, query.value(0));
        m_table->setItem(row, 0, username);
        m_table->setItem(row, 1, new QTableWidgetItem(query.value(2).toString()));
        m_table->setItem(row, 2, new QTableWidgetItem(query.value(3).toString()));
        m_table->setItem(row, 3, new QTableWidgetItem(query.value(4).toBool()
                                                          ? QStringLiteral("启用") : QStringLiteral("停用")));
        for (int column = 4; column < 7; ++column)
            m_table->setItem(row, column, new QTableWidgetItem(query.value(column + 1).toString()));
    }
    if (m_table->rowCount() > 0) m_table->selectRow(0);
    updateActions();
}

void UserManagementPage::addUser()
{
    bool ok = false;
    const QString username = QInputDialog::getText(this, QStringLiteral("新增用户"),
        QStringLiteral("用户名（3到32位字母、数字、点、横线或下划线）"), QLineEdit::Normal, {}, &ok);
    if (!ok) return;
    const QString displayName = QInputDialog::getText(this, QStringLiteral("新增用户"),
        QStringLiteral("显示名"), QLineEdit::Normal, {}, &ok);
    if (!ok) return;
    const QStringList roles = {QStringLiteral("ADMIN"), QStringLiteral("WAREHOUSE"),
                               QStringLiteral("PRODUCTION"), QStringLiteral("QUERY")};
    const QString role = QInputDialog::getItem(this, QStringLiteral("新增用户"),
                                                QStringLiteral("角色"), roles, 1, false, &ok);
    if (!ok) return;
    const QString password = QInputDialog::getText(this, QStringLiteral("新增用户"),
        QStringLiteral("初始密码（至少8位）"), QLineEdit::Password, {}, &ok);
    if (!ok) return;
    UserService service(m_database, m_session.userId);
    QString error;
    if (!service.createUser(username, displayName, role, password, nullptr, &error))
        QMessageBox::warning(this, QStringLiteral("创建失败"), error);
    else refresh();
}

void UserManagementPage::editUser()
{
    const int row = m_table->currentRow();
    if (row < 0) return;
    bool ok = false;
    const QString displayName = QInputDialog::getText(this, QStringLiteral("编辑用户"),
        QStringLiteral("显示名"), QLineEdit::Normal, m_table->item(row, 1)->text(), &ok);
    if (!ok) return;
    const QStringList roles = {QStringLiteral("ADMIN"), QStringLiteral("WAREHOUSE"),
                               QStringLiteral("PRODUCTION"), QStringLiteral("QUERY")};
    const QString currentRole = m_table->item(row, 2)->text().section(QStringLiteral(" - "), 0, 0);
    const QString role = QInputDialog::getItem(this, QStringLiteral("编辑用户"),
        QStringLiteral("角色"), roles, qMax(0, roles.indexOf(currentRole)), false, &ok);
    if (!ok) return;
    const bool active = QMessageBox::question(this, QStringLiteral("账号状态"),
        QStringLiteral("是否启用这个账号？"), QMessageBox::Yes | QMessageBox::No,
        m_table->item(row, 3)->text() == QStringLiteral("启用") ? QMessageBox::Yes : QMessageBox::No)
        == QMessageBox::Yes;
    UserService service(m_database, m_session.userId);
    QString error;
    if (!service.updateUser(selectedUserId(), displayName, role, active, &error))
        QMessageBox::warning(this, QStringLiteral("更新失败"), error);
    else refresh();
}

void UserManagementPage::resetPassword()
{
    bool ok = false;
    const QString password = QInputDialog::getText(this, QStringLiteral("重置密码"),
        QStringLiteral("新密码（至少8位）"), QLineEdit::Password, {}, &ok);
    if (!ok) return;
    UserService service(m_database, m_session.userId);
    QString error;
    if (!service.resetPassword(selectedUserId(), password, &error))
        QMessageBox::warning(this, QStringLiteral("重置失败"), error);
    else QMessageBox::information(this, QStringLiteral("重置完成"), QStringLiteral("密码已更新。"));
}

void UserManagementPage::updateActions()
{
    const bool enabled = m_session.isAdministrator() && selectedUserId() > 0;
    m_editButton->setEnabled(enabled);
    m_resetButton->setEnabled(enabled);
}
