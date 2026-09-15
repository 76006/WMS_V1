#include "ui/pages/SystemSettingsPage.h"
#include "services/UserService.h"
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QSet>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QUuid>
#include <utility>

SystemSettingsPage::SystemSettingsPage(QSqlDatabase database, Session session,
                                       const QString &databaseFilePath, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session)),
      m_databaseFilePath(databaseFilePath)
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

    if (m_session.canManageSystem()) {
        auto *cleanupPanel = new QFrame(this);
        cleanupPanel->setObjectName(QStringLiteral("panel"));
        auto *cleanupLayout = new QVBoxLayout(cleanupPanel);
        auto *cleanupTitle = new QLabel(QStringLiteral("测试数据清理"), cleanupPanel);
        cleanupTitle->setObjectName(QStringLiteral("sectionTitle"));
        cleanupLayout->addWidget(cleanupTitle);
        auto *cleanupHint = new QLabel(
            QStringLiteral("可选择清理库存业务、送检或日志。物料档案、BOM、仓库、库位、用户和权限始终保留；清理前强制备份数据库。"),
            cleanupPanel);
        cleanupHint->setObjectName(QStringLiteral("mutedText"));
        cleanupHint->setWordWrap(true);
        cleanupLayout->addWidget(cleanupHint);
        auto *cleanupButton = new QPushButton(QStringLiteral("选择并删除业务数据"), cleanupPanel);
        cleanupButton->setStyleSheet(QStringLiteral(
            "QPushButton { color:#b42318; border:1px solid #f04438; }"
            "QPushButton:hover { background:#fff1f0; }"));
        cleanupLayout->addWidget(cleanupButton, 0, Qt::AlignRight);
        root->addWidget(cleanupPanel);
        connect(cleanupButton, &QPushButton::clicked,
                this, &SystemSettingsPage::clearBusinessData);
    }
    connect(change, &QPushButton::clicked, this, &SystemSettingsPage::changePassword);
    connect(saveRules, &QPushButton::clicked, this, &SystemSettingsPage::saveNumberRules);
    loadNumberRules();
}

void SystemSettingsPage::clearBusinessData()
{
    if (!m_session.canManageSystem()) return;

    auto tableCount = [this](const QString &tableName) -> qlonglong {
        QSqlQuery query(m_database);
        if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(tableName)) || !query.next()) {
            return -1;
        }
        return query.value(0).toLongLong();
    };
    const qlonglong documentCount = tableCount(QStringLiteral("business_documents"));
    const qlonglong ledgerCount = tableCount(QStringLiteral("inventory_ledger"));
    const qlonglong inspectionCount = tableCount(QStringLiteral("inspection_notices"));
    const qlonglong auditCount = tableCount(QStringLiteral("audit_logs"));
    if (documentCount < 0 || ledgerCount < 0 || inspectionCount < 0 || auditCount < 0) {
        QMessageBox::warning(this, QStringLiteral("读取失败"),
                             QStringLiteral("无法统计待清理数据，请检查数据库连接。"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("选择要删除的业务数据"));
    dialog.setMinimumWidth(620);
    auto *dialogLayout = new QVBoxLayout(&dialog);
    auto *warning = new QLabel(
        QStringLiteral("删除不可撤销。系统会先在“我的文档”中创建完整数据库备份；备份失败时不会删除任何数据。"),
        &dialog);
    warning->setWordWrap(true);
    dialogLayout->addWidget(warning);

    auto *businessCheck = new QCheckBox(
        QStringLiteral("库存及全部业务单据（%1 张单据、%2 条库存流水）")
            .arg(documentCount).arg(ledgerCount), &dialog);
    businessCheck->setChecked(true);
    auto *inspectionCheck = new QCheckBox(
        QStringLiteral("材料送检数据（%1 张送检单）").arg(inspectionCount), &dialog);
    inspectionCheck->setChecked(true);
    auto *auditCheck = new QCheckBox(
        QStringLiteral("历史操作日志（%1 条；本次清理记录仍会保留）").arg(auditCount), &dialog);
    auto *sequenceCheck = new QCheckBox(QStringLiteral("重置所有单据流水号"), &dialog);
    dialogLayout->addWidget(businessCheck);
    dialogLayout->addWidget(inspectionCheck);
    dialogLayout->addWidget(auditCheck);
    dialogLayout->addWidget(sequenceCheck);

    auto *preserved = new QLabel(
        QStringLiteral("始终保留：物料档案、物料图片、BOM、分类/项目、仓库、库位、用户、角色和编号规则。我的文档中的历史表单文件不会删除。"),
        &dialog);
    preserved->setObjectName(QStringLiteral("mutedText"));
    preserved->setWordWrap(true);
    dialogLayout->addWidget(preserved);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("下一步"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialogLayout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    const bool clearBusiness = businessCheck->isChecked();
    const bool clearInspections = inspectionCheck->isChecked();
    const bool clearAudit = auditCheck->isChecked();
    const bool resetSequences = sequenceCheck->isChecked();
    if (!clearBusiness && !clearInspections && !clearAudit && !resetSequences) {
        QMessageBox::information(this, QStringLiteral("没有选择"),
                                 QStringLiteral("没有选择任何要删除或重置的数据。"));
        return;
    }
    if (clearInspections && !clearBusiness) {
        QSqlQuery linked(m_database);
        if (!linked.exec(QStringLiteral(
                "SELECT COUNT(*) FROM inspection_notices WHERE linked_document_id IS NOT NULL"))
            || !linked.next()) {
            QMessageBox::warning(this, QStringLiteral("检查失败"), linked.lastError().text());
            return;
        }
        if (linked.value(0).toLongLong() > 0) {
            QMessageBox::warning(
                this, QStringLiteral("送检数据仍被入库单使用"),
                QStringLiteral("存在已经关联入库单的送检记录。请同时选择“库存及全部业务单据”，保证关联数据完整清理。"));
            return;
        }
    }

    bool confirmed = false;
    const QString typed = QInputDialog::getText(
        this, QStringLiteral("最终确认"),
        QStringLiteral("此操作会删除所选业务数据。请输入“确认删除”继续："),
        QLineEdit::Normal, QString(), &confirmed);
    if (!confirmed || typed.trimmed() != QStringLiteral("确认删除")) {
        if (confirmed) {
            QMessageBox::information(this, QStringLiteral("已取消"),
                                     QStringLiteral("确认文字不正确，没有删除任何数据。"));
        }
        return;
    }

    QString backupRoot = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (backupRoot.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法备份"),
                             QStringLiteral("无法找到“我的文档”文件夹，没有删除任何数据。"));
        return;
    }
    backupRoot = QDir(backupRoot).filePath(QStringLiteral("冰美肌仓库系统表单/数据库备份"));
    if (!QDir().mkpath(backupRoot)) {
        QMessageBox::warning(this, QStringLiteral("无法备份"),
                             QStringLiteral("无法创建数据库备份文件夹，没有删除任何数据。"));
        return;
    }
    const QString backupPath = QDir(backupRoot).filePath(
        QStringLiteral("清理前-%1.db")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))));
    QString quotedBackupPath = QDir::toNativeSeparators(backupPath);
    quotedBackupPath.replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString backupConnectionName = QStringLiteral("business-cleanup-backup-%1")
                                             .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString backupError;
    bool backupOk = false;
    {
        QSqlDatabase backupDatabase = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), backupConnectionName);
        backupDatabase.setDatabaseName(m_databaseFilePath);
        backupDatabase.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=10000"));
        if (!backupDatabase.open()) {
            backupError = backupDatabase.lastError().text();
        } else {
            QSqlQuery backupQuery(backupDatabase);
            backupOk = backupQuery.exec(QStringLiteral("VACUUM INTO '%1'").arg(quotedBackupPath));
            if (!backupOk) backupError = backupQuery.lastError().text();
            backupQuery.finish();
            backupDatabase.close();
        }
    }
    QSqlDatabase::removeDatabase(backupConnectionName);
    if (!backupOk) {
        QFile::remove(backupPath);
        QMessageBox::warning(this, QStringLiteral("备份失败"),
                             QStringLiteral("数据库备份失败，没有删除任何数据：\n%1")
                                 .arg(backupError));
        return;
    }

    if (!m_database.transaction()) {
        QMessageBox::warning(this, QStringLiteral("清理失败"), m_database.lastError().text());
        return;
    }
    QString error;
    auto execute = [this, &error](const QString &sql) {
        QSqlQuery query(m_database);
        if (query.exec(sql)) return true;
        error = query.lastError().text();
        return false;
    };
    bool ok = true;

    if (clearBusiness) {
        if (clearInspections) {
            ok = ok && execute(QStringLiteral(
                "UPDATE inspection_notices SET linked_document_id=NULL WHERE linked_document_id IS NOT NULL"));
        } else {
            ok = ok && execute(QStringLiteral(
                "UPDATE inspection_notices SET linked_document_id=NULL,status="
                "CASE inspection_result WHEN 'QUALIFIED' THEN 'QUALIFIED' "
                "WHEN 'UNQUALIFIED' THEN 'UNQUALIFIED' ELSE 'PENDING' END "
                "WHERE linked_document_id IS NOT NULL"));
        }
    }
    if (clearInspections) {
        ok = ok && execute(QStringLiteral(
            "UPDATE inbound_inspection_details SET inspection_notice_id=NULL,requires_inspection=0,"
            "inspection_no='',inspection_date=NULL,inspector_name='',"
            "inspection_result='NOT_REQUIRED',conclusion='',inspection_attachment_id=NULL"));
        ok = ok && execute(QStringLiteral("DELETE FROM inspection_notice_items"));
        ok = ok && execute(QStringLiteral("DELETE FROM inspection_notices"));
        ok = ok && execute(QStringLiteral(
            "DELETE FROM attachments WHERE business_type='inspection_notice'"));
    }
    if (clearBusiness) {
        ok = ok && execute(QStringLiteral("UPDATE inventory_ledger SET reversal_of_ledger_id=NULL"));
        ok = ok && execute(QStringLiteral("UPDATE business_document_items SET source_item_id=NULL"));
        ok = ok && execute(QStringLiteral("UPDATE business_documents SET source_document_id=NULL"));
        ok = ok && execute(QStringLiteral("DELETE FROM inventory_ledger_serials"));
        ok = ok && execute(QStringLiteral("DELETE FROM inventory_count_items"));
        ok = ok && execute(QStringLiteral("DELETE FROM inventory_counts"));
        ok = ok && execute(QStringLiteral("DELETE FROM inventory_ledger"));
        ok = ok && execute(QStringLiteral("DELETE FROM inbound_inspection_details"));
        ok = ok && execute(QStringLiteral("DELETE FROM sales_outbound_details"));
        ok = ok && execute(QStringLiteral("DELETE FROM document_forms"));
        ok = ok && execute(QStringLiteral("DELETE FROM serial_numbers"));
        ok = ok && execute(QStringLiteral("DELETE FROM stock_balances"));
        ok = ok && execute(QStringLiteral("DELETE FROM batches"));
        ok = ok && execute(QStringLiteral("DELETE FROM business_document_items"));
        ok = ok && execute(QStringLiteral("DELETE FROM business_documents"));
        ok = ok && execute(QStringLiteral("DELETE FROM production_runs"));
        ok = ok && execute(QStringLiteral(
            "DELETE FROM attachments WHERE business_type='business_document'"));
    }
    if (clearAudit) ok = ok && execute(QStringLiteral("DELETE FROM audit_logs"));
    if (resetSequences) {
        ok = ok && execute(QStringLiteral(
            "UPDATE number_rules SET sequence_date='',current_sequence=0"));
    }
    if (ok) {
        QSqlQuery audit(m_database);
        audit.prepare(QStringLiteral(
            "INSERT INTO audit_logs(user_id,action,entity_type,detail) "
            "VALUES(?,'BUSINESS_DATA_PURGE','system',?)"));
        audit.addBindValue(m_session.userId);
        audit.addBindValue(QStringLiteral("业务=%1；送检=%2；日志=%3；重置流水=%4；备份=%5")
                               .arg(clearBusiness).arg(clearInspections).arg(clearAudit)
                               .arg(resetSequences, 0, 10).arg(QDir::toNativeSeparators(backupPath)));
        ok = audit.exec();
        if (!ok) error = audit.lastError().text();
    }
    if (!ok || !m_database.commit()) {
        if (error.isEmpty()) error = m_database.lastError().text();
        m_database.rollback();
        QMessageBox::warning(this, QStringLiteral("清理失败"),
                             QStringLiteral("数据库已回滚，没有发生部分删除。\n\n%1\n\n备份文件：%2")
                                 .arg(error, QDir::toNativeSeparators(backupPath)));
        return;
    }

    loadNumberRules();
    emit businessDataCleared();
    QMessageBox::information(this, QStringLiteral("清理完成"),
                             QStringLiteral("所选业务数据已删除，基础资料已保留。\n\n清理前备份：%1")
                                 .arg(QDir::toNativeSeparators(backupPath)));
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
