#include "ui/pages/AuditLogPage.h"

#include "import/XlsxExporter.h"

#include <QAbstractItemView>
#include <QDate>
#include <QDateEdit>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

#include <utility>

AuditLogPage::AuditLogPage(QSqlDatabase database, QWidget *parent)
    : QWidget(parent), m_database(std::move(database))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QVBoxLayout(panel);
    auto *toolbar = new QHBoxLayout;
    m_fromDate = new QDateEdit(QDate::currentDate().addMonths(-1), panel);
    m_toDate = new QDateEdit(QDate::currentDate(), panel);
    for (QDateEdit *edit : {m_fromDate, m_toDate}) {
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    }
    m_keywordEdit = new QLineEdit(panel);
    m_keywordEdit->setPlaceholderText(QStringLiteral("用户、动作、对象或详情"));
    auto *search = new QPushButton(QStringLiteral("查询"), panel);
    auto *exportButton = new QPushButton(QStringLiteral("导出Excel"), panel);
    toolbar->addWidget(new QLabel(QStringLiteral("日期"), panel));
    toolbar->addWidget(m_fromDate);
    toolbar->addWidget(new QLabel(QStringLiteral("至"), panel));
    toolbar->addWidget(m_toDate);
    toolbar->addWidget(m_keywordEdit, 1);
    toolbar->addWidget(search);
    toolbar->addWidget(exportButton);
    layout->addLayout(toolbar);

    m_table = new QTableWidget(0, 7, panel);
    m_table->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("用户"),
        QStringLiteral("动作"), QStringLiteral("对象类型"), QStringLiteral("对象ID"),
        QStringLiteral("详情"), QStringLiteral("日志ID")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    layout->addWidget(m_table);
    root->addWidget(panel, 1);

    connect(search, &QPushButton::clicked, this, &AuditLogPage::refresh);
    connect(m_keywordEdit, &QLineEdit::returnPressed, this, &AuditLogPage::refresh);
    connect(exportButton, &QPushButton::clicked, this, &AuditLogPage::exportLogs);
    refresh();
}

void AuditLogPage::refresh()
{
    m_table->setRowCount(0);
    const QString keyword = QStringLiteral("%%1%").arg(m_keywordEdit->text().trimmed());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT a.created_at,COALESCE(u.display_name||' ('||u.username||')','系统'),"
        "a.action,a.entity_type,COALESCE(CAST(a.entity_id AS TEXT),''),a.detail,a.id "
        "FROM audit_logs a LEFT JOIN users u ON u.id=a.user_id "
        "WHERE date(a.created_at)>=? AND date(a.created_at)<=? AND "
        "(COALESCE(u.username,'') LIKE ? OR COALESCE(u.display_name,'') LIKE ? OR "
        "a.action LIKE ? OR a.entity_type LIKE ? OR a.detail LIKE ?) "
        "ORDER BY a.id DESC LIMIT 2000"));
    query.addBindValue(m_fromDate->date().toString(Qt::ISODate));
    query.addBindValue(m_toDate->date().toString(Qt::ISODate));
    for (int i = 0; i < 5; ++i) query.addBindValue(keyword);
    if (!query.exec()) return;
    while (query.next()) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        for (int column = 0; column < 7; ++column)
            m_table->setItem(row, column, new QTableWidgetItem(query.value(column).toString()));
    }
}

void AuditLogPage::exportLogs()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出操作日志"),
        QStringLiteral("操作日志-%1.xlsx").arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd"))),
        QStringLiteral("Excel 工作簿 (*.xlsx)"));
    if (path.isEmpty()) return;
    QStringList headers;
    for (int c = 0; c < m_table->columnCount(); ++c) headers << m_table->horizontalHeaderItem(c)->text();
    QList<QList<QVariant>> rows;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QList<QVariant> row;
        for (int c = 0; c < m_table->columnCount(); ++c) row << m_table->item(r, c)->text();
        rows << row;
    }
    QString error;
    if (!XlsxExporter::writeSingleSheet(path, QStringLiteral("操作日志"), headers, rows, &error))
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
    else
        QMessageBox::information(this, QStringLiteral("导出完成"), QStringLiteral("操作日志已导出。"));
}
