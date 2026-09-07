#include "ui/pages/DashboardPage.h"

#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

DashboardPage::DashboardPage(QSqlDatabase database, QWidget *parent)
    : QWidget(parent), m_database(std::move(database))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(16);

    auto *metrics = new QGridLayout;
    metrics->setHorizontalSpacing(14);
    metrics->setVerticalSpacing(14);
    metrics->addWidget(createMetricCard(QStringLiteral("物料种类"), &m_materialCount), 0, 0);
    metrics->addWidget(createMetricCard(QStringLiteral("当前总库存"), &m_totalQuantity), 0, 1);
    metrics->addWidget(createMetricCard(QStringLiteral("今日入库"), &m_todayInbound), 0, 2);
    metrics->addWidget(createMetricCard(QStringLiteral("今日出库"), &m_todayOutbound), 1, 0);
    metrics->addWidget(createMetricCard(QStringLiteral("待盘点任务"), &m_pendingCounts), 1, 1);
    metrics->addWidget(createMetricCard(QStringLiteral("库存不足物料"), &m_lowStock), 1, 2);
    for (int column = 0; column < 3; ++column) {
        metrics->setColumnStretch(column, 1);
    }
    root->addLayout(metrics);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(16, 14, 16, 16);
    auto *title = new QLabel(QStringLiteral("最近出入库记录"), panel);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    panelLayout->addWidget(title);
    m_recentTable = new QTableWidget(panel);
    m_recentTable->setColumnCount(8);
    m_recentTable->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("业务类型"),
                                               QStringLiteral("单据号"), QStringLiteral("物料编码"),
                                               QStringLiteral("物料名称"), QStringLiteral("入库"),
                                               QStringLiteral("出库"), QStringLiteral("仓库/库位")});
    m_recentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_recentTable->setAlternatingRowColors(true);
    m_recentTable->verticalHeader()->hide();
    m_recentTable->horizontalHeader()->setStretchLastSection(true);
    panelLayout->addWidget(m_recentTable);
    root->addWidget(panel, 1);
    refresh();
}

QFrame *DashboardPage::createMetricCard(const QString &label, QLabel **valueLabel)
{
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("metricCard"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 15, 18, 15);
    auto *caption = new QLabel(label, card);
    caption->setObjectName(QStringLiteral("metricLabel"));
    auto *value = new QLabel(QStringLiteral("0"), card);
    value->setObjectName(QStringLiteral("metricValue"));
    layout->addWidget(caption);
    layout->addWidget(value);
    *valueLabel = value;
    return card;
}

QVariant DashboardPage::scalar(const QString &sql) const
{
    QSqlQuery query(m_database);
    if (query.exec(sql) && query.next()) {
        return query.value(0);
    }
    return {};
}

void DashboardPage::refresh()
{
    m_materialCount->setText(scalar(QStringLiteral(
        "SELECT COUNT(*) FROM materials WHERE is_active=1")).toString());
    m_totalQuantity->setText(QString::number(scalar(QStringLiteral(
        "SELECT COALESCE(SUM(quantity),0) FROM stock_balances")).toDouble(), 'f', 3)
                                     .remove(QRegularExpression(QStringLiteral("\\.?0+$"))));
    m_todayInbound->setText(scalar(QStringLiteral(
        "SELECT COUNT(*) FROM business_documents WHERE stock_direction='IN' AND status!='DRAFT' "
        "AND document_date=date('now','localtime')")).toString());
    m_todayOutbound->setText(scalar(QStringLiteral(
        "SELECT COUNT(*) FROM business_documents WHERE stock_direction='OUT' AND status!='DRAFT' "
        "AND document_date=date('now','localtime')")).toString());
    m_pendingCounts->setText(scalar(QStringLiteral(
        "SELECT COUNT(*) FROM inventory_counts WHERE status='DRAFT'")).toString());
    m_lowStock->setText(scalar(QStringLiteral(
        "SELECT COUNT(*) FROM materials m WHERE m.is_active=1 AND "
        "COALESCE((SELECT SUM(s.quantity) FROM stock_balances s WHERE s.material_id=m.id),0)<=m.minimum_stock"))
                                .toString());

    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT l.occurred_at, l.business_type, d.document_no, m.code, m.name, "
        "l.quantity_in, l.quantity_out, w.name || ' / ' || loc.code "
        "FROM inventory_ledger l "
        "JOIN business_documents d ON d.id=l.document_id "
        "JOIN materials m ON m.id=l.material_id "
        "JOIN warehouses w ON w.id=l.warehouse_id "
        "JOIN locations loc ON loc.id=l.location_id "
        "ORDER BY l.id DESC LIMIT 12"));
    m_recentTable->setRowCount(0);
    while (query.next()) {
        const int row = m_recentTable->rowCount();
        m_recentTable->insertRow(row);
        for (int column = 0; column < 8; ++column) {
            QString text;
            if (column == 5 || column == 6) {
                text = QString::number(query.value(column).toDouble(), 'f', 6)
                           .remove(QRegularExpression(QStringLiteral("\\.?0+$")));
            } else {
                text = query.value(column).toString();
            }
            m_recentTable->setItem(row, column, new QTableWidgetItem(text));
        }
    }
    m_recentTable->resizeColumnsToContents();
}

