#include "ui/pages/InventoryPage.h"

#include <QComboBox>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QTableView>
#include <QVBoxLayout>

#include <utility>

InventoryPage::InventoryPage(QSqlDatabase database, QWidget *parent)
    : QWidget(parent), m_database(std::move(database))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *toolbar = new QHBoxLayout;
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("物料编码、名称、规格或SN"));
    m_searchEdit->setClearButtonEnabled(true);
    m_categoryCombo = new QComboBox(this);
    m_categoryCombo->setMinimumWidth(130);
    m_warehouseCombo = new QComboBox(this);
    m_warehouseCombo->setMinimumWidth(150);
    m_batchEdit = new QLineEdit(this);
    m_batchEdit->setPlaceholderText(QStringLiteral("批次号"));
    m_batchEdit->setClearButtonEnabled(true);
    m_batchEdit->setMaximumWidth(180);
    auto *searchButton = new QPushButton(QStringLiteral("查询"), this);
    searchButton->setProperty("primary", true);
    auto *clearButton = new QPushButton(QStringLiteral("清空条件"), this);

    toolbar->addWidget(m_searchEdit, 1);
    toolbar->addWidget(m_categoryCombo);
    toolbar->addWidget(m_warehouseCombo);
    toolbar->addWidget(m_batchEdit);
    toolbar->addWidget(searchButton);
    toolbar->addWidget(clearButton);
    root->addLayout(toolbar);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(12, 12, 12, 12);
    m_table = new QTableView(panel);
    m_model = new QSqlQueryModel(this);
    m_table->setModel(m_model);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(false);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    panelLayout->addWidget(m_table);
    root->addWidget(panel, 1);

    connect(searchButton, &QPushButton::clicked, this, &InventoryPage::refresh);
    connect(clearButton, &QPushButton::clicked, this, &InventoryPage::clearFilters);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &InventoryPage::refresh);
    connect(m_batchEdit, &QLineEdit::returnPressed, this, &InventoryPage::refresh);
    connect(m_categoryCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &InventoryPage::refresh);
    connect(m_warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &InventoryPage::refresh);

    loadFilters();
    refresh();
}

void InventoryPage::loadFilters()
{
    const QVariant selectedCategory = m_categoryCombo->currentData();
    const QVariant selectedWarehouse = m_warehouseCombo->currentData();
    m_categoryCombo->blockSignals(true);
    m_warehouseCombo->blockSignals(true);

    m_categoryCombo->clear();
    m_categoryCombo->addItem(QStringLiteral("全部分类"), QVariant());
    QSqlQuery categories(m_database);
    categories.exec(QStringLiteral(
        "SELECT id, name FROM material_categories WHERE is_active=1 ORDER BY sort_order, name"));
    while (categories.next()) {
        m_categoryCombo->addItem(categories.value(1).toString(), categories.value(0));
    }
    int index = m_categoryCombo->findData(selectedCategory);
    m_categoryCombo->setCurrentIndex(index >= 0 ? index : 0);

    m_warehouseCombo->clear();
    m_warehouseCombo->addItem(QStringLiteral("全部仓库"), QVariant());
    QSqlQuery warehouses(m_database);
    warehouses.exec(QStringLiteral("SELECT id, code, name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (warehouses.next()) {
        m_warehouseCombo->addItem(
            QStringLiteral("%1 - %2").arg(warehouses.value(1).toString(), warehouses.value(2).toString()),
            warehouses.value(0));
    }
    index = m_warehouseCombo->findData(selectedWarehouse);
    m_warehouseCombo->setCurrentIndex(index >= 0 ? index : 0);

    m_categoryCombo->blockSignals(false);
    m_warehouseCombo->blockSignals(false);
}

void InventoryPage::clearFilters()
{
    m_searchEdit->clear();
    m_batchEdit->clear();
    m_categoryCombo->setCurrentIndex(0);
    m_warehouseCombo->setCurrentIndex(0);
    refresh();
}

void InventoryPage::refresh()
{
    loadFilters();
    const QString keyword = QStringLiteral("%%1%").arg(m_searchEdit->text().trimmed());
    const QString batch = QStringLiteral("%%1%").arg(m_batchEdit->text().trimmed());

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT s.id, m.code, m.name, m.specification, c.name, m.unit, "
        "w.name, l.code, s.batch_no, s.quantity, m.minimum_stock, "
        "CASE WHEN total.total_quantity<=m.minimum_stock THEN '库存不足' ELSE '' END, "
        "(SELECT MAX(il.occurred_at) FROM inventory_ledger il WHERE il.material_id=m.id "
        " AND il.warehouse_id=s.warehouse_id AND il.location_id=s.location_id "
        " AND il.batch_no=s.batch_no AND il.quantity_in>0), "
        "(SELECT MAX(il.occurred_at) FROM inventory_ledger il WHERE il.material_id=m.id "
        " AND il.warehouse_id=s.warehouse_id AND il.location_id=s.location_id "
        " AND il.batch_no=s.batch_no AND il.quantity_out>0) "
        "FROM stock_balances s "
        "JOIN materials m ON m.id=s.material_id "
        "LEFT JOIN material_categories c ON c.id=m.category_id "
        "JOIN warehouses w ON w.id=s.warehouse_id "
        "JOIN locations l ON l.id=s.location_id "
        "JOIN (SELECT material_id, SUM(quantity) total_quantity FROM stock_balances GROUP BY material_id) total "
        " ON total.material_id=m.id "
        "WHERE m.is_active=1 AND s.quantity>0 "
        "AND (? IS NULL OR m.category_id=?) AND (? IS NULL OR s.warehouse_id=?) "
        "AND s.batch_no LIKE ? "
        "AND (m.code LIKE ? OR m.name LIKE ? OR m.specification LIKE ? OR EXISTS(" 
        " SELECT 1 FROM serial_numbers sn WHERE sn.material_id=m.id AND sn.serial_no LIKE ?" 
        ")) ORDER BY m.code, w.code, l.code, s.batch_no"));
    const QVariant category = m_categoryCombo->currentData();
    const QVariant warehouse = m_warehouseCombo->currentData();
    query.addBindValue(category);
    query.addBindValue(category);
    query.addBindValue(warehouse);
    query.addBindValue(warehouse);
    query.addBindValue(batch);
    for (int i = 0; i < 4; ++i) query.addBindValue(keyword);
    query.exec();
    m_model->setQuery(std::move(query));

    const QStringList headers = {
        QStringLiteral("ID"), QStringLiteral("物料编码"), QStringLiteral("物料名称"),
        QStringLiteral("规格型号"), QStringLiteral("分类"), QStringLiteral("单位"),
        QStringLiteral("仓库"), QStringLiteral("库位"), QStringLiteral("批次"),
        QStringLiteral("当前库存"), QStringLiteral("最低库存"), QStringLiteral("状态"),
        QStringLiteral("最近入库"), QStringLiteral("最近出库")
    };
    for (int column = 0; column < headers.size(); ++column) {
        m_model->setHeaderData(column, Qt::Horizontal, headers.at(column));
    }
    m_table->hideColumn(0);
    m_table->resizeColumnsToContents();
    m_table->setColumnWidth(2, qMax(m_table->columnWidth(2), 160));
    m_table->setColumnWidth(3, qMax(m_table->columnWidth(3), 150));
}

