#include "ui/pages/BatchTracePage.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

#include <utility>

namespace {
constexpr int MaterialIdRole = Qt::UserRole + 1;
constexpr int BatchNoRole = Qt::UserRole + 2;
}

BatchTracePage::BatchTracePage(QSqlDatabase database, QWidget *parent)
    : QWidget(parent), m_database(std::move(database))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);
    auto *summaryPanel = new QFrame(this);
    summaryPanel->setObjectName(QStringLiteral("panel"));
    auto *summaryLayout = new QVBoxLayout(summaryPanel);
    summaryLayout->setContentsMargins(18, 16, 18, 18);
    summaryLayout->addWidget(new QLabel(QStringLiteral("批次库存与追溯"), summaryPanel));
    auto *filters = new QHBoxLayout;
    m_keywordEdit = new QLineEdit(summaryPanel);
    m_keywordEdit->setPlaceholderText(QStringLiteral("物料编码、名称或批次号"));
    m_warehouseCombo = new QComboBox(summaryPanel);
    auto *search = new QPushButton(QStringLiteral("查询"), summaryPanel);
    filters->addWidget(m_keywordEdit, 1);
    filters->addWidget(m_warehouseCombo);
    filters->addWidget(search);
    summaryLayout->addLayout(filters);
    m_batchTable = new QTableWidget(0, 10, summaryPanel);
    m_batchTable->setHorizontalHeaderLabels({QStringLiteral("物料编码"), QStringLiteral("物料名称"),
                                             QStringLiteral("批次号"), QStringLiteral("当前库存"),
                                             QStringLiteral("库存库位数"), QStringLiteral("首次入库"),
                                             QStringLiteral("供应商"), QStringLiteral("使用生产批次"),
                                             QStringLiteral("最后变动"), QStringLiteral("状态")});
    m_batchTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_batchTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_batchTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_batchTable->verticalHeader()->setVisible(false);
    m_batchTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_batchTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    summaryLayout->addWidget(m_batchTable, 1);
    root->addWidget(summaryPanel, 1);

    auto *historyPanel = new QFrame(this);
    historyPanel->setObjectName(QStringLiteral("panel"));
    auto *historyLayout = new QVBoxLayout(historyPanel);
    historyLayout->addWidget(new QLabel(QStringLiteral("所选批次库存流水"), historyPanel));
    m_historyTable = new QTableWidget(0, 10, historyPanel);
    m_historyTable->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("单据号"),
                                               QStringLiteral("业务类型"), QStringLiteral("入库"),
                                               QStringLiteral("出库"), QStringLiteral("变动前"),
                                               QStringLiteral("变动后"), QStringLiteral("仓库/库位"),
                                               QStringLiteral("生产批次"), QStringLiteral("操作人")});
    m_historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_historyTable->verticalHeader()->setVisible(false);
    m_historyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Stretch);
    historyLayout->addWidget(m_historyTable);
    root->addWidget(historyPanel, 1);

    connect(search, &QPushButton::clicked, this, &BatchTracePage::loadBatches);
    connect(m_keywordEdit, &QLineEdit::returnPressed, this, &BatchTracePage::loadBatches);
    connect(m_warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &BatchTracePage::loadBatches);
    connect(m_batchTable, &QTableWidget::itemSelectionChanged, this, &BatchTracePage::loadHistory);
    refresh();
}

void BatchTracePage::refresh()
{
    const QVariant previous = m_warehouseCombo->currentData();
    m_warehouseCombo->blockSignals(true);
    m_warehouseCombo->clear();
    m_warehouseCombo->addItem(QStringLiteral("全部仓库"), 0);
    QSqlQuery warehouses(m_database);
    warehouses.exec(QStringLiteral("SELECT id,code,name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (warehouses.next())
        m_warehouseCombo->addItem(QStringLiteral("%1 - %2").arg(warehouses.value(1).toString(),
                                                                 warehouses.value(2).toString()),
                                  warehouses.value(0));
    const int selected = m_warehouseCombo->findData(previous);
    if (selected >= 0) m_warehouseCombo->setCurrentIndex(selected);
    m_warehouseCombo->blockSignals(false);
    loadBatches();
}

void BatchTracePage::loadBatches()
{
    m_batchTable->setRowCount(0);
    m_historyTable->setRowCount(0);
    const QString keyword = QStringLiteral("%%1%").arg(m_keywordEdit->text().trimmed());
    const qlonglong warehouseId = m_warehouseCombo->currentData().toLongLong();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT m.id,m.code,m.name,b.batch_no,"
        "COALESCE((SELECT SUM(s.quantity) FROM stock_balances s WHERE s.material_id=b.material_id "
        "AND s.batch_no=b.batch_no AND (?=0 OR s.warehouse_id=?)),0),"
        "(SELECT COUNT(*) FROM stock_balances s WHERE s.material_id=b.material_id "
        "AND s.batch_no=b.batch_no AND s.quantity>0 AND (?=0 OR s.warehouse_id=?)),"
        "b.first_in_at,b.supplier,(SELECT MAX(l.occurred_at) FROM inventory_ledger l "
        "WHERE l.material_id=b.material_id AND l.batch_no=b.batch_no "
        "AND (?=0 OR l.warehouse_id=?)),"
        "COALESCE((SELECT GROUP_CONCAT(DISTINCT pr.batch_no) "
        "FROM business_document_items i JOIN business_documents d ON d.id=i.document_id "
        "JOIN production_runs pr ON pr.id=d.production_run_id "
        "WHERE i.material_id=b.material_id AND i.batch_no=b.batch_no AND d.document_type='SCLL' "
        "AND (?=0 OR i.warehouse_id=?)),'') "
        "FROM batches b JOIN materials m ON m.id=b.material_id "
        "WHERE (m.code LIKE ? OR m.name LIKE ? OR b.batch_no LIKE ?) "
        "ORDER BY m.code,b.batch_no"));
    query.addBindValue(warehouseId);
    query.addBindValue(warehouseId);
    query.addBindValue(warehouseId);
    query.addBindValue(warehouseId);
    query.addBindValue(warehouseId);
    query.addBindValue(warehouseId);
    query.addBindValue(warehouseId);
    query.addBindValue(warehouseId);
    query.addBindValue(keyword);
    query.addBindValue(keyword);
    query.addBindValue(keyword);
    query.exec();
    while (query.next()) {
        const int row = m_batchTable->rowCount();
        m_batchTable->insertRow(row);
        auto *code = new QTableWidgetItem(query.value(1).toString());
        code->setData(MaterialIdRole, query.value(0));
        code->setData(BatchNoRole, query.value(3));
        m_batchTable->setItem(row, 0, code);
        m_batchTable->setItem(row, 1, new QTableWidgetItem(query.value(2).toString()));
        m_batchTable->setItem(row, 2, new QTableWidgetItem(query.value(3).toString()));
        m_batchTable->setItem(row, 3, new QTableWidgetItem(query.value(4).toString()));
        m_batchTable->setItem(row, 4, new QTableWidgetItem(query.value(5).toString()));
        m_batchTable->setItem(row, 5, new QTableWidgetItem(query.value(6).toString()));
        m_batchTable->setItem(row, 6, new QTableWidgetItem(query.value(7).toString()));
        m_batchTable->setItem(row, 7, new QTableWidgetItem(query.value(9).toString()));
        m_batchTable->setItem(row, 8, new QTableWidgetItem(query.value(8).toString()));
        m_batchTable->setItem(row, 9, new QTableWidgetItem(query.value(4).toDouble() > 0.0000001
                                                              ? QStringLiteral("有库存")
                                                              : QStringLiteral("已用完")));
    }
    if (m_batchTable->rowCount() > 0) m_batchTable->selectRow(0);
}

void BatchTracePage::loadHistory()
{
    m_historyTable->setRowCount(0);
    const int row = m_batchTable->currentRow();
    if (row < 0) return;
    const QTableWidgetItem *code = m_batchTable->item(row, 0);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT l.occurred_at,d.document_no,l.business_type,l.quantity_in,l.quantity_out,"
        "l.quantity_before,l.quantity_after,w.code||' / '||loc.code,COALESCE(pr.batch_no,''),u.display_name "
        "FROM inventory_ledger l JOIN business_documents d ON d.id=l.document_id "
        "JOIN warehouses w ON w.id=l.warehouse_id JOIN locations loc ON loc.id=l.location_id "
        "JOIN users u ON u.id=l.operator_id LEFT JOIN production_runs pr ON pr.id=d.production_run_id "
        "WHERE l.material_id=? AND l.batch_no=? "
        "AND (?=0 OR l.warehouse_id=?) ORDER BY l.id DESC"));
    query.addBindValue(code->data(MaterialIdRole));
    query.addBindValue(code->data(BatchNoRole));
    query.addBindValue(m_warehouseCombo->currentData());
    query.addBindValue(m_warehouseCombo->currentData());
    query.exec();
    while (query.next()) {
        const int target = m_historyTable->rowCount();
        m_historyTable->insertRow(target);
        for (int column = 0; column < 10; ++column)
            m_historyTable->setItem(target, column,
                                    new QTableWidgetItem(query.value(column).toString()));
    }
}
