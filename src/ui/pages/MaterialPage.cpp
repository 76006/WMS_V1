#include "ui/pages/MaterialPage.h"

#include "ui/dialogs/MaterialDialog.h"

#include <QComboBox>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QTableView>
#include <QVBoxLayout>

#include <utility>

MaterialPage::MaterialPage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *toolbar = new QHBoxLayout;
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索物料编码、名称、规格或品牌"));
    m_searchEdit->setClearButtonEnabled(true);
    m_categoryCombo = new QComboBox(this);
    m_categoryCombo->setMinimumWidth(140);
    auto *searchButton = new QPushButton(QStringLiteral("查询"), this);
    m_addButton = new QPushButton(QStringLiteral("新增物料"), this);
    m_addButton->setProperty("primary", true);
    m_editButton = new QPushButton(QStringLiteral("编辑"), this);
    const bool canEdit = m_session.isAdministrator();
    m_addButton->setEnabled(canEdit);
    m_editButton->setEnabled(canEdit);
    if (!canEdit) {
        m_addButton->setToolTip(QStringLiteral("只有管理员可以维护物料资料"));
        m_editButton->setToolTip(m_addButton->toolTip());
    }
    toolbar->addWidget(m_searchEdit, 1);
    toolbar->addWidget(m_categoryCombo);
    toolbar->addWidget(searchButton);
    toolbar->addSpacing(14);
    toolbar->addWidget(m_addButton);
    toolbar->addWidget(m_editButton);
    root->addLayout(toolbar);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(12, 12, 12, 12);
    m_table = new QTableView(panel);
    m_model = new QSqlQueryModel(this);
    m_table->setModel(m_model);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    panelLayout->addWidget(m_table);
    root->addWidget(panel, 1);

    connect(searchButton, &QPushButton::clicked, this, &MaterialPage::refresh);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MaterialPage::refresh);
    connect(m_categoryCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MaterialPage::refresh);
    connect(m_addButton, &QPushButton::clicked, this, &MaterialPage::addMaterial);
    connect(m_editButton, &QPushButton::clicked, this, &MaterialPage::editMaterial);
    connect(m_table, &QTableView::doubleClicked, this, [this] {
        if (m_session.isAdministrator()) editMaterial();
    });
    loadCategories();
    refresh();
}

void MaterialPage::loadCategories()
{
    const QVariant current = m_categoryCombo->currentData();
    m_categoryCombo->blockSignals(true);
    m_categoryCombo->clear();
    m_categoryCombo->addItem(QStringLiteral("全部分类"), QVariant());
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT id, name FROM material_categories WHERE is_active=1 ORDER BY sort_order, name"));
    while (query.next()) {
        m_categoryCombo->addItem(query.value(1).toString(), query.value(0));
    }
    const int index = m_categoryCombo->findData(current);
    m_categoryCombo->setCurrentIndex(index >= 0 ? index : 0);
    m_categoryCombo->blockSignals(false);
}

void MaterialPage::refresh()
{
    const QString keyword = QStringLiteral("%%1%").arg(m_searchEdit->text().trimmed());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT m.id, m.code, m.name, m.specification, c.name, m.brand, m.unit, "
        "COALESCE((SELECT SUM(s.quantity) FROM stock_balances s WHERE s.material_id=m.id),0) total_stock, "
        "m.minimum_stock, CASE WHEN COALESCE((SELECT SUM(s.quantity) FROM stock_balances s WHERE s.material_id=m.id),0) "
        "<=m.minimum_stock THEN '库存不足' ELSE '' END warning, "
        "CASE m.require_batch WHEN 1 THEN '是' ELSE '否' END, "
        "CASE m.require_serial WHEN 1 THEN '是' ELSE '否' END, "
        "COALESCE(w.name,''), COALESCE(l.code,''), m.notes "
        "FROM materials m LEFT JOIN material_categories c ON c.id=m.category_id "
        "LEFT JOIN warehouses w ON w.id=m.default_warehouse_id "
        "LEFT JOIN locations l ON l.id=m.default_location_id "
        "WHERE m.is_active=1 AND (? IS NULL OR m.category_id=?) "
        "AND (m.code LIKE ? OR m.name LIKE ? OR m.specification LIKE ? OR m.brand LIKE ?) "
        "ORDER BY m.code"));
    const QVariant category = m_categoryCombo->currentData();
    query.addBindValue(category);
    query.addBindValue(category);
    for (int i = 0; i < 4; ++i) query.addBindValue(keyword);
    query.exec();
    m_model->setQuery(std::move(query));

    const QStringList headers = {QStringLiteral("ID"), QStringLiteral("物料编码"), QStringLiteral("物料名称"),
        QStringLiteral("规格型号"), QStringLiteral("分类"), QStringLiteral("品牌"), QStringLiteral("单位"),
        QStringLiteral("当前库存"), QStringLiteral("最低库存"), QStringLiteral("状态"), QStringLiteral("批次管理"),
        QStringLiteral("SN管理"), QStringLiteral("默认仓库"), QStringLiteral("默认库位"), QStringLiteral("备注")};
    for (int i = 0; i < headers.size(); ++i) m_model->setHeaderData(i, Qt::Horizontal, headers.at(i));
    m_table->hideColumn(0);
    m_table->resizeColumnsToContents();
    m_table->setColumnWidth(2, qMax(m_table->columnWidth(2), 150));
    m_table->setColumnWidth(3, qMax(m_table->columnWidth(3), 150));
}

qlonglong MaterialPage::selectedMaterialId() const
{
    const QModelIndex current = m_table->currentIndex();
    if (!current.isValid()) return 0;
    return m_model->index(current.row(), 0).data().toLongLong();
}

void MaterialPage::addMaterial()
{
    MaterialDialog dialog(m_database, 0, this);
    if (dialog.exec() == QDialog::Accepted) {
        refresh();
        emit dataChanged();
    }
}

void MaterialPage::editMaterial()
{
    const qlonglong id = selectedMaterialId();
    if (id <= 0) {
        QMessageBox::information(this, QStringLiteral("请选择物料"), QStringLiteral("请先在表格中选择一条物料记录。"));
        return;
    }
    MaterialDialog dialog(m_database, id, this);
    if (dialog.exec() == QDialog::Accepted) {
        refresh();
        emit dataChanged();
    }
}

