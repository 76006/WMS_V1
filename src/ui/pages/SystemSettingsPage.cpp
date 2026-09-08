#include "ui/pages/SystemSettingsPage.h"
#include "services/UserService.h"
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QTableWidget>
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

    auto *numberPanel = new QFrame(this);
    numberPanel->setObjectName(QStringLiteral("panel"));
    auto *numberLayout = new QVBoxLayout(numberPanel);
    auto *numberTitle = new QHBoxLayout;
    numberTitle->addWidget(new QLabel(QStringLiteral("单据编号规则"), numberPanel));
    numberTitle->addStretch();
    auto *saveRules = new QPushButton(QStringLiteral("保存编号规则"), numberPanel);
    saveRules->setProperty("primary", true);
    saveRules->setEnabled(m_session.canManageSystem());
    numberTitle->addWidget(saveRules);
    numberLayout->addLayout(numberTitle);
    auto *ruleHint = new QLabel(QStringLiteral("编号格式：前缀 + 日期(yyyyMMdd) + 当日流水号。可修改前缀和流水号位数。"), numberPanel);
    ruleHint->setObjectName(QStringLiteral("mutedText"));
    numberLayout->addWidget(ruleHint);
    m_numberRulesTable = new QTableWidget(0, 5, numberPanel);
    m_numberRulesTable->setHorizontalHeaderLabels({QStringLiteral("单据类型"), QStringLiteral("前缀"),
        QStringLiteral("流水号位数"), QStringLiteral("当前日期"), QStringLiteral("当前流水")});
    m_numberRulesTable->verticalHeader()->hide();
    m_numberRulesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    if (!m_session.canManageSystem()) m_numberRulesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    numberLayout->addWidget(m_numberRulesTable);
    root->addWidget(numberPanel, 1);
    connect(change, &QPushButton::clicked, this, &SystemSettingsPage::changePassword);
    connect(saveRules, &QPushButton::clicked, this, &SystemSettingsPage::saveNumberRules);
    loadNumberRules();
}

void SystemSettingsPage::loadNumberRules()
{
    m_numberRulesTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT document_type,prefix,sequence_width,sequence_date,current_sequence "
        "FROM number_rules ORDER BY document_type"));
    while (query.next()) {
        const int row = m_numberRulesTable->rowCount();
        m_numberRulesTable->insertRow(row);
        for (int column = 0; column < 5; ++column) {
            auto *item = new QTableWidgetItem(query.value(column).toString());
            if (column == 0 || column >= 3) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            m_numberRulesTable->setItem(row, column, item);
        }
    }
}

void SystemSettingsPage::saveNumberRules()
{
    if (!m_session.canManageSystem()) return;
    if (!m_database.transaction()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), m_database.lastError().text());
        return;
    }
    QSqlQuery update(m_database);
    QSqlQuery audit(m_database);
    bool ok = true;
    QString error;
    QSet<QString> prefixes;
    for (int row = 0; row < m_numberRulesTable->rowCount(); ++row) {
        const QString type = m_numberRulesTable->item(row, 0)->text();
        const QString prefix = m_numberRulesTable->item(row, 1)->text().trimmed().toUpper();
        bool widthOk = false;
        const int width = m_numberRulesTable->item(row, 2)->text().toInt(&widthOk);
        if (prefix.isEmpty() || prefix.size() > 12 || !widthOk || width < 3 || width > 8) {
            ok = false;
            error = QStringLiteral("%1 的前缀不能为空且最长12位，流水号位数应为3到8。").arg(type);
            break;
        }
        if (prefixes.contains(prefix)) {
            ok = false;
            error = QStringLiteral("单据前缀 %1 重复，请为不同单据类型设置不同前缀。").arg(prefix);
            break;
        }
        prefixes.insert(prefix);
        update.prepare(QStringLiteral("UPDATE number_rules SET prefix=?,sequence_width=? WHERE document_type=?"));
        update.addBindValue(prefix);
        update.addBindValue(width);
        update.addBindValue(type);
        if (!update.exec()) {
            ok = false;
            error = update.lastError().text();
            break;
        }
    }
    if (ok) {
        audit.prepare(QStringLiteral(
            "INSERT INTO audit_logs(user_id,action,entity_type,detail) VALUES(?,'NUMBER_RULES_UPDATE','number_rules',?)"));
        audit.addBindValue(m_session.userId);
        audit.addBindValue(QStringLiteral("已更新单据编号规则"));
        ok = audit.exec();
        if (!ok) error = audit.lastError().text();
    }
    if (!ok || !m_database.commit()) {
        m_database.rollback();
        QMessageBox::warning(this, QStringLiteral("保存失败"), error.isEmpty() ? m_database.lastError().text() : error);
        return;
    }
    loadNumberRules();
    QMessageBox::information(this, QStringLiteral("保存完成"), QStringLiteral("单据编号规则已更新。"));
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
