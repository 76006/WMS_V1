#include "ui/pages/MaterialPage.h"

#include "import/LegacyInventoryImporter.h"
#include "import/XlsxExporter.h"
#include "services/InventoryService.h"
#include "services/MaterialCodeService.h"
#include "ui/dialogs/MaterialDialog.h"
#include "ui/widgets/ComboBoxSearch.h"
#include "ui/widgets/TableExcelExport.h"

#include <QComboBox>
#include <QCheckBox>
#include <QBrush>
#include <QColor>
#include <QDateEdit>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QMap>
#include <QPushButton>
#include <QPixmap>
#include <QScreen>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QTableView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <utility>

namespace {
constexpr int BomMaterialIdRole = Qt::UserRole + 1;
constexpr int BomItemIdRole = Qt::UserRole + 2;
constexpr int BomCumulativeQuantityRole = Qt::UserRole + 3;
constexpr int BomProductCodeRole = Qt::UserRole + 4;
constexpr int BomLookupCodeRole = Qt::UserRole + 20;
constexpr int BomLookupNameRole = Qt::UserRole + 21;
constexpr int BomLookupSpecificationRole = Qt::UserRole + 22;
constexpr int BomLookupCategoryRole = Qt::UserRole + 23;
constexpr int BomLookupUnitRole = Qt::UserRole + 24;
constexpr int BomLookupProcessingRole = Qt::UserRole + 25;
constexpr int BomLookupActiveRole = Qt::UserRole + 26;

void loadBomMaterialOptions(QSqlDatabase database, QComboBox *comboBox,
                            qlonglong productId, qlonglong currentMaterialId = 0)
{
    comboBox->clear();
    QSqlQuery materials(database);
    materials.prepare(QStringLiteral(
        "SELECT m.id,m.code,m.name,m.specification,COALESCE(c.name,''),m.unit,"
        "m.processing_method,m.is_active FROM materials m "
        "LEFT JOIN material_categories c ON c.id=m.category_id "
        "WHERE m.id<>? AND COALESCE(c.code,'')<>'FINISHED' "
        "AND (m.is_active=1 OR m.id=?) ORDER BY m.code"));
    materials.addBindValue(productId);
    materials.addBindValue(currentMaterialId);
    if (!materials.exec()) return;

    while (materials.next()) {
        QString label = QStringLiteral("%1 - %2")
                            .arg(materials.value(1).toString(), materials.value(2).toString());
        const QString specification = materials.value(3).toString().trimmed();
        if (!specification.isEmpty()) label += QStringLiteral("（%1）").arg(specification);
        if (!materials.value(7).toBool()) label += QStringLiteral("（已停用）");
        const int index = comboBox->count();
        comboBox->addItem(label, materials.value(0));
        comboBox->setItemData(index, materials.value(1), BomLookupCodeRole);
        comboBox->setItemData(index, materials.value(2), BomLookupNameRole);
        comboBox->setItemData(index, materials.value(3), BomLookupSpecificationRole);
        comboBox->setItemData(index, materials.value(4), BomLookupCategoryRole);
        comboBox->setItemData(index, materials.value(5), BomLookupUnitRole);
        comboBox->setItemData(index, materials.value(6), BomLookupProcessingRole);
        comboBox->setItemData(index, materials.value(7), BomLookupActiveRole);
    }
}

int resolveBomMaterialIndex(const QComboBox *comboBox)
{
    if (!comboBox) return -1;
    const QString keyword = comboBox->currentText().trimmed();
    if (keyword.isEmpty()) return -1;

    const int currentIndex = comboBox->currentIndex();
    if (currentIndex >= 0
        && keyword.compare(comboBox->itemText(currentIndex), Qt::CaseInsensitive) == 0) {
        return currentIndex;
    }

    for (int index = 0; index < comboBox->count(); ++index) {
        if (keyword.compare(comboBox->itemData(index, BomLookupCodeRole).toString(),
                            Qt::CaseInsensitive) == 0) {
            return index;
        }
    }

    int matchedIndex = -1;
    int matchCount = 0;
    for (int index = 0; index < comboBox->count(); ++index) {
        const QStringList searchable = {
            comboBox->itemData(index, BomLookupCodeRole).toString(),
            comboBox->itemData(index, BomLookupNameRole).toString(),
            comboBox->itemData(index, BomLookupSpecificationRole).toString(),
            comboBox->itemData(index, BomLookupCategoryRole).toString(),
            comboBox->itemData(index, BomLookupProcessingRole).toString()
        };
        bool matched = false;
        for (const QString &value : searchable) {
            if (value.contains(keyword, Qt::CaseInsensitive)) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;
        matchedIndex = index;
        ++matchCount;
    }
    return matchCount == 1 ? matchedIndex : -1;
}

void updateBomMaterialSummary(const QComboBox *comboBox, QLabel *summaryLabel)
{
    const int index = resolveBomMaterialIndex(comboBox);
    if (index < 0) {
        const QString text = QStringLiteral(
            "请输入完整物料号，或输入名称/规格关键词后从候选项中选择。");
        summaryLabel->setText(text);
        summaryLabel->setToolTip(text);
        return;
    }
    const QString text =
        QStringLiteral("已识别：%1 - %2\n规格：%3｜类别：%4｜单位：%5｜加工方式：%6｜状态：%7")
            .arg(comboBox->itemData(index, BomLookupCodeRole).toString(),
                 comboBox->itemData(index, BomLookupNameRole).toString(),
                 comboBox->itemData(index, BomLookupSpecificationRole).toString(),
                 comboBox->itemData(index, BomLookupCategoryRole).toString(),
                 comboBox->itemData(index, BomLookupUnitRole).toString(),
                 comboBox->itemData(index, BomLookupProcessingRole).toString(),
                 comboBox->itemData(index, BomLookupActiveRole).toBool()
                     ? QStringLiteral("正常") : QStringLiteral("停用"));
    summaryLabel->setText(text);
    summaryLabel->setToolTip(text);
}

void configureBomMaterialSummary(QLabel *summaryLabel)
{
    summaryLabel->setObjectName(QStringLiteral("mutedText"));
    summaryLabel->setWordWrap(true);
    summaryLabel->setMinimumHeight(64);
    summaryLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summaryLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
}
}

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
    m_statusCombo = new QComboBox(this);
    m_statusCombo->setMinimumWidth(110);
    m_statusCombo->addItem(QStringLiteral("全部状态"), QVariant());
    m_statusCombo->addItem(QStringLiteral("正常"), true);
    m_statusCombo->addItem(QStringLiteral("停用"), false);
    auto *searchButton = new QPushButton(QStringLiteral("查询"), this);
    m_addButton = new QPushButton(QStringLiteral("新增物料"), this);
    m_addButton->setProperty("primary", true);
    m_editButton = new QPushButton(QStringLiteral("编辑"), this);
    m_editStockButton = new QPushButton(QStringLiteral("编辑库存"), this);
    m_deleteButton = new QPushButton(QStringLiteral("删除物料"), this);
    m_batchEditButton = new QPushButton(QStringLiteral("批量编辑"), this);
    m_importButton = new QPushButton(QStringLiteral("导入Excel"), this);
    m_exportButton = new QPushButton(QStringLiteral("导出Excel"), this);
    m_projectButton = new QPushButton(QStringLiteral("项目代码"), this);
    const bool canEdit = m_session.canManageMaterials();
    m_addButton->setEnabled(canEdit);
    m_editButton->setEnabled(canEdit);
    m_editStockButton->setEnabled(m_session.canManageWarehouse());
    m_deleteButton->setEnabled(canEdit);
    m_batchEditButton->setEnabled(canEdit);
    m_importButton->setEnabled(canEdit);
    m_projectButton->setEnabled(canEdit);
    if (!canEdit) {
        m_addButton->setToolTip(QStringLiteral("当前角色没有物料维护权限"));
        m_editButton->setToolTip(m_addButton->toolTip());
        m_deleteButton->setToolTip(m_addButton->toolTip());
        m_batchEditButton->setToolTip(m_addButton->toolTip());
        m_importButton->setToolTip(m_addButton->toolTip());
        m_projectButton->setToolTip(m_addButton->toolTip());
    }
    if (!m_session.canManageWarehouse())
        m_editStockButton->setToolTip(QStringLiteral("当前角色没有库存调整权限"));
    toolbar->addWidget(m_searchEdit, 1);
    toolbar->addWidget(m_categoryCombo);
    toolbar->addWidget(m_statusCombo);
    toolbar->addWidget(searchButton);
    toolbar->addSpacing(14);
    toolbar->addWidget(m_addButton);
    toolbar->addWidget(m_editButton);
    toolbar->addWidget(m_editStockButton);
    toolbar->addWidget(m_deleteButton);
    toolbar->addWidget(m_batchEditButton);
    toolbar->addWidget(m_importButton);
    toolbar->addWidget(m_exportButton);
    toolbar->addWidget(m_projectButton);
    root->addLayout(toolbar);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(12, 12, 12, 12);
    auto *listHeader = new QHBoxLayout;
    auto *listTitle = new QLabel(QStringLiteral("物料清单"), panel);
    listTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_countLabel = new QLabel(panel);
    m_countLabel->setObjectName(QStringLiteral("mutedText"));
    m_countLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    listHeader->addWidget(listTitle);
    listHeader->addStretch();
    listHeader->addWidget(m_countLabel);
    panelLayout->addLayout(listHeader);
    m_table = new QTableView(panel);
    m_model = new QSqlQueryModel(this);
    m_table->setModel(m_model);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    panelLayout->addWidget(m_table);

    auto *bomPanel = new QFrame(this);
    bomPanel->setObjectName(QStringLiteral("panel"));
    auto *bomLayout = new QVBoxLayout(bomPanel);
    bomLayout->setContentsMargins(12, 12, 12, 12);
    auto *bomToolbar = new QHBoxLayout;
    bomToolbar->addWidget(new QLabel(QStringLiteral("成品BOM"), bomPanel));
    m_bomProductCombo = new QComboBox(bomPanel);
    m_bomProductCombo->setMinimumWidth(320);
    bomToolbar->addWidget(m_bomProductCombo, 1);
    m_importBomButton = new QPushButton(QStringLiteral("导入BOM"), bomPanel);
    m_importBomButton->setProperty("primary", true);
    m_viewBomMaterialButton = new QPushButton(QStringLiteral("查看物料详情"), bomPanel);
    m_viewBomMaterialButton->setEnabled(false);
    m_addBomChildButton = new QPushButton(QStringLiteral("添加下级"), bomPanel);
    m_editBomQuantityButton = new QPushButton(QStringLiteral("调整选中节点"), bomPanel);
    m_removeBomItemButton = new QPushButton(QStringLiteral("删除选中节点"), bomPanel);
    m_clearBomButton = new QPushButton(QStringLiteral("清空整棵BOM"), bomPanel);
    m_clearBomButton->setProperty("danger", true);
    auto *expandButton = new QPushButton(QStringLiteral("全部展开"), bomPanel);
    auto *collapseButton = new QPushButton(QStringLiteral("全部折叠"), bomPanel);
    for (QPushButton *button : {m_importBomButton, m_addBomChildButton,
                                m_editBomQuantityButton, m_removeBomItemButton,
                                m_clearBomButton}) {
        button->setEnabled(canEdit);
        if (!canEdit) button->setToolTip(QStringLiteral("当前角色没有物料维护权限"));
    }
    bomToolbar->addWidget(m_importBomButton);
    bomToolbar->addWidget(m_viewBomMaterialButton);
    bomToolbar->addWidget(m_addBomChildButton);
    bomToolbar->addWidget(m_editBomQuantityButton);
    bomToolbar->addWidget(m_removeBomItemButton);
    bomToolbar->addWidget(m_clearBomButton);
    bomToolbar->addWidget(expandButton);
    bomToolbar->addWidget(collapseButton);
    bomLayout->addLayout(bomToolbar);

    auto *bomHint = new QLabel(
        QStringLiteral("BOM按成品、半成品、原材料和辅料分层显示。选中任意物料后可查看完整档案和库存批次；有维护权限时还可添加下级、调整节点或删除子树。成品根节点的用量固定为1。"),
        bomPanel);
    bomHint->setObjectName(QStringLiteral("mutedText"));
    bomHint->setWordWrap(true);
    bomLayout->addWidget(bomHint);
    m_bomTree = new QTreeWidget(bomPanel);
    m_bomTree->setColumnCount(8);
    m_bomTree->setHeaderLabels({QStringLiteral("BOM层级/物料"), QStringLiteral("规格型号"),
        QStringLiteral("物料类别"), QStringLiteral("单位"), QStringLiteral("节点用量"),
        QStringLiteral("累计用量"), QStringLiteral("加工方式"), QStringLiteral("当前库存")});
    m_bomTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_bomTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bomTree->setAlternatingRowColors(true);
    m_bomTree->setUniformRowHeights(true);
    m_bomTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bomTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    bomLayout->addWidget(m_bomTree, 1);

    m_viewTabs = new QTabWidget(this);
    m_viewTabs->addTab(bomPanel, QStringLiteral("BOM层级"));
    m_viewTabs->addTab(panel, QStringLiteral("全部物料"));
    root->addWidget(m_viewTabs, 1);

    connect(searchButton, &QPushButton::clicked, this, &MaterialPage::refresh);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MaterialPage::refresh);
    connect(m_categoryCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MaterialPage::refresh);
    connect(m_statusCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MaterialPage::refresh);
    connect(m_addButton, &QPushButton::clicked, this, &MaterialPage::addMaterial);
    connect(m_editButton, &QPushButton::clicked, this, &MaterialPage::editMaterial);
    connect(m_editStockButton, &QPushButton::clicked, this, &MaterialPage::editStock);
    connect(m_deleteButton, &QPushButton::clicked, this, &MaterialPage::deleteMaterials);
    connect(m_batchEditButton, &QPushButton::clicked, this, &MaterialPage::batchEditMaterials);
    connect(m_importButton, &QPushButton::clicked, this, &MaterialPage::importMaterials);
    connect(m_exportButton, &QPushButton::clicked, this, &MaterialPage::exportMaterials);
    connect(m_projectButton, &QPushButton::clicked, this, &MaterialPage::manageProjects);
    connect(m_bomProductCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MaterialPage::refreshBomTree);
    connect(m_importBomButton, &QPushButton::clicked, this, &MaterialPage::importBom);
    connect(m_viewBomMaterialButton, &QPushButton::clicked,
            this, &MaterialPage::viewBomMaterialDetails);
    connect(m_addBomChildButton, &QPushButton::clicked, this, &MaterialPage::addBomChild);
    connect(m_editBomQuantityButton, &QPushButton::clicked,
            this, &MaterialPage::editBomQuantity);
    connect(m_removeBomItemButton, &QPushButton::clicked, this, &MaterialPage::removeBomItem);
    connect(m_clearBomButton, &QPushButton::clicked, this, &MaterialPage::clearBom);
    connect(expandButton, &QPushButton::clicked, m_bomTree, &QTreeWidget::expandAll);
    connect(collapseButton, &QPushButton::clicked, m_bomTree, &QTreeWidget::collapseAll);
    connect(m_table, &QTableView::doubleClicked, this, [this] {
        if (m_session.canManageMaterials()) editMaterial();
    });
    connect(m_bomTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { viewBomMaterialDetails(); });
    connect(m_bomTree, &QTreeWidget::itemSelectionChanged, this, [this, canEdit] {
        const QTreeWidgetItem *item = m_bomTree->currentItem();
        m_viewBomMaterialButton->setEnabled(item);
        m_addBomChildButton->setEnabled(canEdit && item);
        m_editBomQuantityButton->setEnabled(canEdit && item);
        m_removeBomItemButton->setEnabled(canEdit && item);
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
        "SELECT m.id, m.code, m.name, m.specification, c.name, m.processing_method, m.brand, m.unit, "
        "m.unit_usage, "
        "COALESCE((SELECT SUM(s.quantity) FROM stock_balances s WHERE s.material_id=m.id),0) total_stock, "
        "m.minimum_stock, CASE WHEN COALESCE((SELECT SUM(s.quantity) FROM stock_balances s WHERE s.material_id=m.id),0) "
        "<=m.minimum_stock THEN '库存不足' ELSE '' END warning, "
        "CASE m.is_active WHEN 1 THEN '正常' ELSE '停用' END, "
        "CASE m.require_batch WHEN 1 THEN '是' ELSE '否' END, "
        "CASE m.require_serial WHEN 1 THEN '是' ELSE '否' END, "
        "COALESCE(w.name,''), COALESCE(l.code,''), m.notes "
        "FROM materials m LEFT JOIN material_categories c ON c.id=m.category_id "
        "LEFT JOIN warehouses w ON w.id=m.default_warehouse_id "
        "LEFT JOIN locations l ON l.id=m.default_location_id "
        "WHERE (? IS NULL OR m.category_id=?) AND (? IS NULL OR m.is_active=?) "
        "AND (m.code LIKE ? OR m.name LIKE ? OR m.specification LIKE ? OR m.brand LIKE ?) "
        "ORDER BY m.code"));
    const QVariant category = m_categoryCombo->currentData();
    query.addBindValue(category);
    query.addBindValue(category);
    const QVariant status = m_statusCombo->currentData();
    query.addBindValue(status);
    query.addBindValue(status);
    for (int i = 0; i < 4; ++i) query.addBindValue(keyword);
    query.exec();
    m_model->setQuery(std::move(query));
    while (m_model->canFetchMore(QModelIndex())) m_model->fetchMore(QModelIndex());

    const QStringList headers = {QStringLiteral("ID"), QStringLiteral("物料编码"), QStringLiteral("物料名称"),
        QStringLiteral("规格型号"), QStringLiteral("物料类别"), QStringLiteral("加工方式"),
        QStringLiteral("品牌"), QStringLiteral("单位"), QStringLiteral("单台用量"),
        QStringLiteral("当前库存"), QStringLiteral("最低库存"), QStringLiteral("库存提示"),
        QStringLiteral("物料状态"), QStringLiteral("批次管理"), QStringLiteral("SN管理"),
        QStringLiteral("默认仓库"), QStringLiteral("默认库位"), QStringLiteral("备注")};
    for (int i = 0; i < headers.size(); ++i) m_model->setHeaderData(i, Qt::Horizontal, headers.at(i));
    m_table->hideColumn(0);
    m_table->resizeColumnsToContents();
    m_table->setColumnWidth(2, qMax(m_table->columnWidth(2), 150));
    m_table->setColumnWidth(3, qMax(m_table->columnWidth(3), 150));

    QSqlQuery totalQuery(m_database);
    int totalCount = 0;
    if (totalQuery.exec(QStringLiteral("SELECT COUNT(*) FROM materials")) && totalQuery.next())
        totalCount = totalQuery.value(0).toInt();
    m_countLabel->setText(QStringLiteral("当前显示 %1 条 / 物料总数 %2 条")
                              .arg(m_model->rowCount()).arg(totalCount));
    refreshBomProducts();
}

void MaterialPage::refreshBomProducts()
{
    const QVariant selected = m_bomProductCombo->currentData();
    m_bomProductCombo->blockSignals(true);
    m_bomProductCombo->clear();
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT m.id,m.code,m.name FROM materials m "
        "JOIN material_categories c ON c.id=m.category_id "
        "WHERE c.code='FINISHED' ORDER BY m.code"));
    while (query.next()) {
        const int index = m_bomProductCombo->count();
        m_bomProductCombo->addItem(
            QStringLiteral("%1 - %2").arg(query.value(1).toString(), query.value(2).toString()),
            query.value(0));
        m_bomProductCombo->setItemData(index, query.value(1), BomProductCodeRole);
    }
    const int selectedIndex = m_bomProductCombo->findData(selected);
    if (selectedIndex >= 0) m_bomProductCombo->setCurrentIndex(selectedIndex);
    m_bomProductCombo->blockSignals(false);
    refreshBomTree();
}

void MaterialPage::refreshBomTree()
{
    m_bomTree->clear();
    const qlonglong productId = m_bomProductCombo->currentData().toLongLong();
    const bool canEdit = m_session.canManageMaterials();
    m_addBomChildButton->setEnabled(false);
    m_viewBomMaterialButton->setEnabled(false);
    m_editBomQuantityButton->setEnabled(false);
    m_removeBomItemButton->setEnabled(false);
    m_clearBomButton->setEnabled(false);
    if (productId <= 0) return;

    QSqlQuery product(m_database);
    product.prepare(QStringLiteral(
        "SELECT m.code,m.name,m.specification,c.name,m.unit,m.processing_method,"
        "COALESCE((SELECT SUM(s.quantity) FROM stock_balances s WHERE s.material_id=m.id),0) "
        "FROM materials m LEFT JOIN material_categories c ON c.id=m.category_id WHERE m.id=?"));
    product.addBindValue(productId);
    if (!product.exec() || !product.next()) return;

    auto *rootItem = new QTreeWidgetItem(m_bomTree);
    rootItem->setText(0, QStringLiteral("%1 - %2")
                             .arg(product.value(0).toString(), product.value(1).toString()));
    rootItem->setText(1, product.value(2).toString());
    rootItem->setText(2, product.value(3).toString());
    rootItem->setText(3, product.value(4).toString());
    rootItem->setText(4, QStringLiteral("1"));
    rootItem->setText(5, QStringLiteral("1"));
    rootItem->setText(6, product.value(5).toString());
    rootItem->setText(7, QString::number(product.value(6).toDouble(), 'g', 12));
    rootItem->setData(0, BomMaterialIdRole, productId);
    rootItem->setData(0, BomItemIdRole, 0);
    rootItem->setData(0, BomCumulativeQuantityRole, 1.0);
    QFont rootFont = rootItem->font(0);
    rootFont.setBold(true);
    rootItem->setFont(0, rootFont);

    QHash<qlonglong, QTreeWidgetItem *> itemById;
    QSqlQuery items(m_database);
    items.prepare(QStringLiteral(
        "SELECT b.id,b.parent_item_id,b.component_material_id,b.quantity,"
        "m.code,m.name,m.specification,c.name,m.unit,m.processing_method,"
        "COALESCE((SELECT SUM(s.quantity) FROM stock_balances s WHERE s.material_id=m.id),0) "
        "FROM material_bom_items b JOIN materials m ON m.id=b.component_material_id "
        "LEFT JOIN material_categories c ON c.id=m.category_id "
        "WHERE b.product_material_id=? ORDER BY b.id"));
    items.addBindValue(productId);
    if (items.exec()) {
        while (items.next()) {
            const qlonglong itemId = items.value(0).toLongLong();
            const qlonglong parentId = items.value(1).toLongLong();
            QTreeWidgetItem *parent = parentId > 0 ? itemById.value(parentId, rootItem) : rootItem;
            auto *item = new QTreeWidgetItem(parent);
            const double nodeQuantity = items.value(3).toDouble();
            const double cumulative = parent->data(0, BomCumulativeQuantityRole).toDouble()
                * nodeQuantity;
            item->setText(0, QStringLiteral("%1 - %2")
                                 .arg(items.value(4).toString(), items.value(5).toString()));
            item->setText(1, items.value(6).toString());
            item->setText(2, items.value(7).toString());
            item->setText(3, items.value(8).toString());
            item->setText(4, QString::number(nodeQuantity, 'g', 12));
            item->setText(5, QString::number(cumulative, 'g', 12));
            item->setText(6, items.value(9).toString());
            item->setText(7, QString::number(items.value(10).toDouble(), 'g', 12));
            item->setData(0, BomMaterialIdRole, items.value(2));
            item->setData(0, BomItemIdRole, itemId);
            item->setData(0, BomCumulativeQuantityRole, cumulative);
            itemById.insert(itemId, item);
        }
    }
    rootItem->setExpanded(true);
    m_bomTree->setCurrentItem(rootItem);
    m_addBomChildButton->setEnabled(canEdit);
    m_viewBomMaterialButton->setEnabled(true);
    m_editBomQuantityButton->setEnabled(canEdit);
    m_removeBomItemButton->setEnabled(canEdit);
    m_clearBomButton->setEnabled(canEdit && !itemById.isEmpty());
}

void MaterialPage::viewBomMaterialDetails()
{
    const QTreeWidgetItem *selectedItem = m_bomTree->currentItem();
    const qlonglong materialId = selectedItem
        ? selectedItem->data(0, BomMaterialIdRole).toLongLong() : 0;
    if (materialId <= 0) {
        QMessageBox::information(this, QStringLiteral("请选择物料"),
                                 QStringLiteral("请先在BOM中选择需要查看的物料。"));
        return;
    }

    QSqlQuery material(m_database);
    material.prepare(QStringLiteral(
        "SELECT m.code,m.name,m.specification,COALESCE(c.name,''),m.brand,m.unit,"
        "m.unit_usage,m.processing_method,m.minimum_stock,COALESCE(w.code,''),"
        "COALESCE(w.name,''),COALESCE(l.code,''),COALESCE(l.name,''),"
        "m.require_batch,m.require_serial,m.notes,m.is_active,m.created_at,m.updated_at,"
        "COALESCE((SELECT SUM(s.quantity) FROM stock_balances s WHERE s.material_id=m.id),0) "
        "FROM materials m LEFT JOIN material_categories c ON c.id=m.category_id "
        "LEFT JOIN warehouses w ON w.id=m.default_warehouse_id "
        "LEFT JOIN locations l ON l.id=m.default_location_id WHERE m.id=?"));
    material.addBindValue(materialId);
    if (!material.exec() || !material.next()) {
        const QString error = material.lastError().text().isEmpty()
            ? QStringLiteral("该物料已不存在，请刷新后重试。")
            : material.lastError().text();
        QMessageBox::warning(this, QStringLiteral("读取物料失败"), error);
        return;
    }

    const auto displayValue = [](const QString &value) {
        return value.trimmed().isEmpty() ? QStringLiteral("未设置") : value.trimmed();
    };
    const auto codeAndName = [&displayValue](const QString &code, const QString &name) {
        if (code.trimmed().isEmpty()) return QStringLiteral("未设置");
        return name.trimmed().isEmpty() ? code.trimmed()
                                        : QStringLiteral("%1 - %2").arg(code.trimmed(), name.trimmed());
    };

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("物料详情 - %1").arg(material.value(0).toString()));
    dialog.setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    dialog.resize(1120, 720);
    auto *layout = new QVBoxLayout(&dialog);

    auto *heading = new QLabel(
        QStringLiteral("%1 - %2").arg(material.value(0).toString(), material.value(1).toString()),
        &dialog);
    heading->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;"));
    heading->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(heading);

    auto *profileBox = new QGroupBox(QStringLiteral("物料完整档案"), &dialog);
    auto *profileLayout = new QHBoxLayout(profileBox);
    auto *fieldWidget = new QWidget(profileBox);
    auto *fields = new QGridLayout(fieldWidget);
    fields->setColumnStretch(1, 1);
    fields->setColumnStretch(3, 1);
    const auto addField = [fields, fieldWidget](int row, int pairColumn,
                                                const QString &caption, const QString &value) {
        const int column = pairColumn * 2;
        auto *captionLabel = new QLabel(caption + QStringLiteral("："), fieldWidget);
        captionLabel->setStyleSheet(QStringLiteral("font-weight:600;"));
        auto *valueLabel = new QLabel(value, fieldWidget);
        valueLabel->setWordWrap(true);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        fields->addWidget(captionLabel, row, column, Qt::AlignTop);
        fields->addWidget(valueLabel, row, column + 1);
    };
    addField(0, 0, QStringLiteral("物料编码"), material.value(0).toString());
    addField(0, 1, QStringLiteral("物料名称"), material.value(1).toString());
    addField(1, 0, QStringLiteral("规格型号"), displayValue(material.value(2).toString()));
    addField(1, 1, QStringLiteral("物料类别"), displayValue(material.value(3).toString()));
    addField(2, 0, QStringLiteral("品牌"), displayValue(material.value(4).toString()));
    addField(2, 1, QStringLiteral("单位"), material.value(5).toString());
    addField(3, 0, QStringLiteral("单台用量"),
             QString::number(material.value(6).toDouble(), 'g', 12));
    addField(3, 1, QStringLiteral("加工方式"), displayValue(material.value(7).toString()));
    addField(4, 0, QStringLiteral("当前总库存"),
             QString::number(material.value(19).toDouble(), 'g', 12));
    addField(4, 1, QStringLiteral("最低库存"),
             QString::number(material.value(8).toDouble(), 'g', 12));
    addField(5, 0, QStringLiteral("物料状态"),
             material.value(16).toBool() ? QStringLiteral("正常") : QStringLiteral("停用"));
    addField(5, 1, QStringLiteral("默认仓库"),
             codeAndName(material.value(9).toString(), material.value(10).toString()));
    addField(6, 0, QStringLiteral("默认库位"),
             codeAndName(material.value(11).toString(), material.value(12).toString()));
    addField(6, 1, QStringLiteral("批次管理"),
             material.value(13).toBool() ? QStringLiteral("是") : QStringLiteral("否"));
    addField(7, 0, QStringLiteral("SN管理"),
             material.value(14).toBool() ? QStringLiteral("是") : QStringLiteral("否"));
    addField(7, 1, QStringLiteral("创建时间"), displayValue(material.value(17).toString()));
    addField(8, 0, QStringLiteral("更新时间"), displayValue(material.value(18).toString()));
    addField(8, 1, QStringLiteral("备注"), displayValue(material.value(15).toString()));
    profileLayout->addWidget(fieldWidget, 1);

    auto *imageBox = new QGroupBox(QStringLiteral("物料图片"), profileBox);
    auto *imageLayout = new QVBoxLayout(imageBox);
    auto *imageLabel = new QLabel(QStringLiteral("暂无图片"), imageBox);
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setMinimumSize(190, 150);
    imageLabel->setMaximumSize(240, 190);
    imageLabel->setFrameShape(QFrame::StyledPanel);
    auto *imageName = new QLabel(imageBox);
    imageName->setAlignment(Qt::AlignCenter);
    imageName->setWordWrap(true);
    QSqlQuery imageQuery(m_database);
    imageQuery.prepare(QStringLiteral(
        "SELECT original_file_name,image_data FROM material_images WHERE material_id=?"));
    imageQuery.addBindValue(materialId);
    if (imageQuery.exec() && imageQuery.next()) {
        QPixmap pixmap;
        if (pixmap.loadFromData(imageQuery.value(1).toByteArray())) {
            imageLabel->setText({});
            imageLabel->setPixmap(pixmap.scaled(imageLabel->maximumSize(), Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation));
            imageName->setText(imageQuery.value(0).toString());
        }
    }
    imageLayout->addWidget(imageLabel, 1);
    imageLayout->addWidget(imageName);
    profileLayout->addWidget(imageBox);
    layout->addWidget(profileBox);

    auto *stockTitle = new QLabel(QStringLiteral("分仓、分库位、分批次库存"), &dialog);
    stockTitle->setStyleSheet(QStringLiteral("font-size:15px;font-weight:600;"));
    layout->addWidget(stockTitle);
    auto *stockTable = new QTableWidget(0, 7, &dialog);
    stockTable->setHorizontalHeaderLabels({
        QStringLiteral("仓库"), QStringLiteral("库位"), QStringLiteral("批次"),
        QStringLiteral("供应商"), QStringLiteral("库存数量"), QStringLiteral("首次入库时间"),
        QStringLiteral("库存更新时间")});
    stockTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stockTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    stockTable->setAlternatingRowColors(true);
    stockTable->verticalHeader()->hide();
    QSqlQuery stockQuery(m_database);
    stockQuery.prepare(QStringLiteral(
        "SELECT w.code,w.name,l.code,l.name,s.batch_no,COALESCE(b.supplier,''),"
        "s.quantity,COALESCE(b.first_in_at,''),s.updated_at "
        "FROM stock_balances s JOIN warehouses w ON w.id=s.warehouse_id "
        "JOIN locations l ON l.id=s.location_id "
        "LEFT JOIN batches b ON b.material_id=s.material_id AND b.batch_no=s.batch_no "
        "WHERE s.material_id=? ORDER BY w.code,l.code,s.batch_no"));
    stockQuery.addBindValue(materialId);
    if (stockQuery.exec()) {
        while (stockQuery.next()) {
            const int row = stockTable->rowCount();
            stockTable->insertRow(row);
            const QStringList values = {
                codeAndName(stockQuery.value(0).toString(), stockQuery.value(1).toString()),
                codeAndName(stockQuery.value(2).toString(), stockQuery.value(3).toString()),
                stockQuery.value(4).toString().trimmed().isEmpty()
                    ? QStringLiteral("无批次") : stockQuery.value(4).toString(),
                stockQuery.value(5).toString().trimmed().isEmpty()
                    ? QStringLiteral("未记录") : stockQuery.value(5).toString(),
                QString::number(stockQuery.value(6).toDouble(), 'g', 12),
                displayValue(stockQuery.value(7).toString()),
                displayValue(stockQuery.value(8).toString())
            };
            for (int column = 0; column < values.size(); ++column)
                stockTable->setItem(row, column, new QTableWidgetItem(values.at(column)));
        }
    }
    stockTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    stockTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    if (stockTable->rowCount() == 0)
        stockTitle->setText(QStringLiteral("分仓、分库位、分批次库存（暂无库存记录）"));
    layout->addWidget(stockTable, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

qlonglong MaterialPage::selectedMaterialId() const
{
    if (m_viewTabs && m_viewTabs->currentIndex() == 0 && m_bomTree->currentItem())
        return m_bomTree->currentItem()->data(0, BomMaterialIdRole).toLongLong();
    const QModelIndex current = m_table->currentIndex();
    if (!current.isValid()) return 0;
    return m_model->index(current.row(), 0).data().toLongLong();
}

QList<qlonglong> MaterialPage::selectedMaterialIds() const
{
    QList<qlonglong> ids;
    if (m_viewTabs && m_viewTabs->currentIndex() == 0) {
        for (const QTreeWidgetItem *item : m_bomTree->selectedItems()) {
            const qlonglong id = item->data(0, BomMaterialIdRole).toLongLong();
            if (id > 0 && !ids.contains(id)) ids.append(id);
        }
        return ids;
    }
    if (!m_table->selectionModel()) return ids;
    const QModelIndexList selected = m_table->selectionModel()->selectedRows(0);
    for (const QModelIndex &index : selected) {
        const qlonglong id = index.data().toLongLong();
        if (id > 0 && !ids.contains(id)) ids.append(id);
    }
    if (ids.isEmpty()) {
        const qlonglong current = selectedMaterialId();
        if (current > 0) ids.append(current);
    }
    return ids;
}

void MaterialPage::addMaterial()
{
    MaterialDialog dialog(m_database, 0, m_session.userId, this);
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
    MaterialDialog dialog(m_database, id, m_session.userId, this);
    if (dialog.exec() == QDialog::Accepted) {
        refresh();
        emit dataChanged();
    }
}

void MaterialPage::editStock()
{
    if (!m_session.canManageWarehouse()) return;
    const qlonglong materialId = selectedMaterialId();
    if (materialId <= 0) {
        QMessageBox::information(this, QStringLiteral("请选择物料"),
                                 QStringLiteral("请先在BOM或物料清单中选择一条物料。"));
        return;
    }

    QSqlQuery material(m_database);
    material.prepare(QStringLiteral(
        "SELECT code,name,require_batch,require_serial,is_active,"
        "default_warehouse_id,default_location_id FROM materials WHERE id=?"));
    material.addBindValue(materialId);
    if (!material.exec() || !material.next()) {
        QMessageBox::warning(this, QStringLiteral("读取失败"),
                             material.lastError().text().isEmpty()
                                 ? QStringLiteral("所选物料已不存在。")
                                 : material.lastError().text());
        return;
    }
    const QString materialCode = material.value(0).toString();
    const QString materialName = material.value(1).toString();
    const bool requireBatch = material.value(2).toBool();
    const bool requireSerial = material.value(3).toBool();
    const bool isActive = material.value(4).toBool();
    const qlonglong defaultWarehouseId = material.value(5).toLongLong();
    const qlonglong defaultLocationId = material.value(6).toLongLong();
    if (!isActive) {
        QMessageBox::warning(this, QStringLiteral("不能编辑库存"),
                             QStringLiteral("该物料已停用，请先恢复为正常状态。"));
        return;
    }
    if (requireSerial) {
        QMessageBox::information(
            this, QStringLiteral("SN物料需同步调整SN"),
            QStringLiteral("该物料启用了SN管理，不能只修改库存数字。请通过入库管理或出库管理同步录入具体SN。"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("编辑库存 - %1").arg(materialCode));
    dialog.resize(820, 640);
    auto *layout = new QVBoxLayout(&dialog);
    auto *heading = new QLabel(
        QStringLiteral("%1 - %2").arg(materialCode, materialName), &dialog);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    layout->addWidget(heading);
    auto *hint = new QLabel(
        QStringLiteral("可直接把指定仓库、库位和批次的库存改为目标数量。保存后系统自动生成盘点调整单和库存流水，不会覆盖历史记录。"),
        &dialog);
    hint->setObjectName(QStringLiteral("mutedText"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto *warehouseCombo = new QComboBox(&dialog);
    auto *locationCombo = new QComboBox(&dialog);
    auto *batchCombo = new QComboBox(&dialog);
    batchCombo->setEditable(requireBatch);
    batchCombo->setInsertPolicy(QComboBox::NoInsert);
    if (requireBatch)
        batchCombo->lineEdit()->setPlaceholderText(QStringLiteral("选择已有批次或输入新批次"));
    auto *supplierEdit = new QLineEdit(&dialog);
    supplierEdit->setEnabled(requireBatch);
    supplierEdit->setPlaceholderText(requireBatch
        ? QStringLiteral("批次供应商，可不填") : QStringLiteral("未启用批次管理"));
    auto *currentLabel = new QLabel(QStringLiteral("0"), &dialog);
    currentLabel->setStyleSheet(QStringLiteral("font-weight:600;"));
    auto *targetSpin = new QDoubleSpinBox(&dialog);
    targetSpin->setDecimals(6);
    targetSpin->setRange(0.0, 999999999999.0);
    auto *reasonEdit = new QLineEdit(QStringLiteral("物料维护直接编辑库存"), &dialog);
    auto *dateEdit = new QDateEdit(QDate::currentDate(), &dialog);
    dateEdit->setCalendarPopup(true);
    dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    form->addRow(QStringLiteral("仓库 *"), warehouseCombo);
    form->addRow(QStringLiteral("库位 *"), locationCombo);
    form->addRow(QStringLiteral("批次%1").arg(requireBatch ? QStringLiteral(" *") : QString()),
                 batchCombo);
    form->addRow(QStringLiteral("供应商"), supplierEdit);
    form->addRow(QStringLiteral("当前库存"), currentLabel);
    form->addRow(QStringLiteral("目标库存 *"), targetSpin);
    form->addRow(QStringLiteral("调整原因 *"), reasonEdit);
    form->addRow(QStringLiteral("调整日期 *"), dateEdit);
    layout->addLayout(form);

    auto *stockTitle = new QLabel(QStringLiteral("现有库存位置（双击可载入编辑）"), &dialog);
    stockTitle->setStyleSheet(QStringLiteral("font-weight:600;"));
    layout->addWidget(stockTitle);
    auto *stockTable = new QTableWidget(0, 5, &dialog);
    stockTable->setHorizontalHeaderLabels({QStringLiteral("仓库"), QStringLiteral("库位"),
        QStringLiteral("批次"), QStringLiteral("供应商"), QStringLiteral("库存数量")});
    stockTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stockTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    stockTable->setSelectionMode(QAbstractItemView::SingleSelection);
    stockTable->setAlternatingRowColors(true);
    stockTable->verticalHeader()->hide();
    stockTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    stockTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    QSqlQuery stockRows(m_database);
    stockRows.prepare(QStringLiteral(
        "SELECT s.warehouse_id,w.code,s.location_id,l.code,s.batch_no,"
        "COALESCE(b.supplier,''),s.quantity FROM stock_balances s "
        "JOIN warehouses w ON w.id=s.warehouse_id JOIN locations l ON l.id=s.location_id "
        "LEFT JOIN batches b ON b.material_id=s.material_id AND b.batch_no=s.batch_no "
        "WHERE s.material_id=? ORDER BY w.code,l.code,s.batch_no"));
    stockRows.addBindValue(materialId);
    if (stockRows.exec()) {
        while (stockRows.next()) {
            const int row = stockTable->rowCount();
            stockTable->insertRow(row);
            auto *warehouseItem = new QTableWidgetItem(stockRows.value(1).toString());
            warehouseItem->setData(Qt::UserRole, stockRows.value(0));
            warehouseItem->setData(Qt::UserRole + 1, stockRows.value(2));
            warehouseItem->setData(Qt::UserRole + 2, stockRows.value(4));
            stockTable->setItem(row, 0, warehouseItem);
            stockTable->setItem(row, 1, new QTableWidgetItem(stockRows.value(3).toString()));
            stockTable->setItem(row, 2, new QTableWidgetItem(
                stockRows.value(4).toString().isEmpty()
                    ? QStringLiteral("无批次") : stockRows.value(4).toString()));
            stockTable->setItem(row, 3, new QTableWidgetItem(stockRows.value(5).toString()));
            stockTable->setItem(row, 4, new QTableWidgetItem(
                QString::number(stockRows.value(6).toDouble(), 'g', 12)));
        }
    }
    if (stockTable->rowCount() == 0)
        stockTitle->setText(QStringLiteral("现有库存位置：暂无，可在上方选择位置后新增库存"));
    layout->addWidget(stockTable, 1);

    std::function<void()> refreshCurrentStock;
    std::function<void()> loadBatches;
    std::function<void()> loadLocations;
    const auto selectedBatch = [batchCombo, requireBatch] {
        return requireBatch ? batchCombo->currentText().trimmed() : QStringLiteral("");
    };
    refreshCurrentStock = [this, materialId, warehouseCombo, locationCombo, batchCombo,
                           supplierEdit, currentLabel, targetSpin, selectedBatch, requireBatch] {
        double quantity = 0.0;
        QString supplier;
        if (warehouseCombo->currentIndex() >= 0 && locationCombo->currentIndex() >= 0) {
            QSqlQuery current(m_database);
            current.prepare(QStringLiteral(
                "SELECT quantity FROM stock_balances WHERE material_id=? AND warehouse_id=? "
                "AND location_id=? AND batch_no=?"));
            current.addBindValue(materialId);
            current.addBindValue(warehouseCombo->currentData());
            current.addBindValue(locationCombo->currentData());
            current.addBindValue(selectedBatch());
            if (current.exec() && current.next()) quantity = current.value(0).toDouble();
            if (requireBatch && !selectedBatch().isEmpty()) {
                QSqlQuery batch(m_database);
                batch.prepare(QStringLiteral(
                    "SELECT supplier FROM batches WHERE material_id=? AND batch_no=?"));
                batch.addBindValue(materialId);
                batch.addBindValue(selectedBatch());
                if (batch.exec() && batch.next()) supplier = batch.value(0).toString();
            }
        }
        currentLabel->setText(QString::number(quantity, 'g', 12));
        targetSpin->setValue(quantity);
        targetSpin->setProperty("systemQuantity", quantity);
        supplierEdit->setText(supplier);
        supplierEdit->setProperty("originalSupplier", supplier);
        batchCombo->setToolTip(requireBatch && selectedBatch().isEmpty()
            ? QStringLiteral("该物料必须填写批次号") : QString());
    };
    loadBatches = [this, materialId, locationCombo, warehouseCombo, batchCombo, requireBatch,
                   refreshCurrentStock] {
        const QString previous = requireBatch ? batchCombo->currentText().trimmed() : QString();
        batchCombo->blockSignals(true);
        batchCombo->clear();
        if (requireBatch && warehouseCombo->currentIndex() >= 0
            && locationCombo->currentIndex() >= 0) {
            QSqlQuery batches(m_database);
            batches.prepare(QStringLiteral(
                "SELECT DISTINCT batch_no FROM stock_balances WHERE material_id=? "
                "AND warehouse_id=? AND location_id=? AND batch_no<>'' ORDER BY batch_no"));
            batches.addBindValue(materialId);
            batches.addBindValue(warehouseCombo->currentData());
            batches.addBindValue(locationCombo->currentData());
            if (batches.exec()) {
                while (batches.next()) batchCombo->addItem(batches.value(0).toString());
            }
            if (!previous.isEmpty()) batchCombo->setEditText(previous);
            else if (batchCombo->count() == 0) batchCombo->setEditText(QStringLiteral(""));
        } else {
            batchCombo->addItem(QStringLiteral("无批次"), QStringLiteral(""));
            batchCombo->setCurrentIndex(0);
        }
        batchCombo->blockSignals(false);
        refreshCurrentStock();
    };
    loadLocations = [this, warehouseCombo, locationCombo, defaultLocationId, loadBatches] {
        const qlonglong previous = locationCombo->currentData().toLongLong();
        locationCombo->blockSignals(true);
        locationCombo->clear();
        QSqlQuery locations(m_database);
        locations.prepare(QStringLiteral(
            "SELECT id,code,name FROM locations WHERE warehouse_id=? AND is_active=1 ORDER BY code"));
        locations.addBindValue(warehouseCombo->currentData());
        if (locations.exec()) {
            while (locations.next()) {
                const QString name = locations.value(2).toString();
                locationCombo->addItem(name.isEmpty() ? locations.value(1).toString()
                    : QStringLiteral("%1 - %2").arg(locations.value(1).toString(), name),
                    locations.value(0));
            }
        }
        int index = locationCombo->findData(previous > 0 ? previous : defaultLocationId);
        if (index < 0 && locationCombo->count() > 0) index = 0;
        locationCombo->setCurrentIndex(index);
        locationCombo->blockSignals(false);
        loadBatches();
    };

    QSqlQuery warehouses(m_database);
    warehouses.exec(QStringLiteral(
        "SELECT id,code,name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (warehouses.next()) {
        warehouseCombo->addItem(
            QStringLiteral("%1 - %2").arg(warehouses.value(1).toString(),
                                           warehouses.value(2).toString()),
            warehouses.value(0));
    }
    int warehouseIndex = warehouseCombo->findData(defaultWarehouseId);
    if (warehouseIndex < 0 && warehouseCombo->count() > 0) warehouseIndex = 0;
    warehouseCombo->setCurrentIndex(warehouseIndex);
    connect(warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
            [loadLocations](int) { loadLocations(); });
    connect(locationCombo, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
            [loadBatches](int) { loadBatches(); });
    connect(batchCombo, &QComboBox::currentTextChanged, &dialog,
            [refreshCurrentStock](const QString &) { refreshCurrentStock(); });
    loadLocations();

    connect(stockTable, &QTableWidget::cellDoubleClicked, &dialog,
            [warehouseCombo, locationCombo, batchCombo, stockTable,
             loadLocations, loadBatches, refreshCurrentStock, requireBatch](int row, int) {
                const QTableWidgetItem *item = stockTable->item(row, 0);
                if (!item) return;
                const int warehouseIndex = warehouseCombo->findData(item->data(Qt::UserRole));
                if (warehouseIndex >= 0) warehouseCombo->setCurrentIndex(warehouseIndex);
                loadLocations();
                const int locationIndex = locationCombo->findData(item->data(Qt::UserRole + 1));
                if (locationIndex >= 0) locationCombo->setCurrentIndex(locationIndex);
                loadBatches();
                if (requireBatch) batchCombo->setEditText(item->data(Qt::UserRole + 2).toString());
                refreshCurrentStock();
            });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                         &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存库存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked, &dialog,
            [this, &dialog, materialId, materialCode, warehouseCombo, locationCombo, batchCombo,
             supplierEdit, currentLabel, targetSpin, reasonEdit, dateEdit,
             selectedBatch, requireBatch] {
                if (warehouseCombo->currentIndex() < 0 || locationCombo->currentIndex() < 0) {
                    QMessageBox::warning(&dialog, QStringLiteral("资料不完整"),
                                         QStringLiteral("请选择有效的仓库和库位。"));
                    return;
                }
                const QString batchNo = selectedBatch();
                if (requireBatch && batchNo.isEmpty()) {
                    QMessageBox::warning(&dialog, QStringLiteral("资料不完整"),
                                         QStringLiteral("该物料启用了批次管理，必须填写批次号。"));
                    batchCombo->setFocus();
                    return;
                }
                const QString reason = reasonEdit->text().trimmed();
                if (reason.isEmpty()) {
                    QMessageBox::warning(&dialog, QStringLiteral("资料不完整"),
                                         QStringLiteral("请填写库存调整原因。"));
                    reasonEdit->setFocus();
                    return;
                }
                const double systemQuantity = targetSpin->property("systemQuantity").toDouble();
                const double targetQuantity = targetSpin->value();
                const QString supplier = supplierEdit->text().trimmed();
                const QString originalSupplier =
                    supplierEdit->property("originalSupplier").toString().trimmed();
                if (std::abs(targetQuantity - systemQuantity) <= 0.0000001
                    && supplier == originalSupplier) {
                    QMessageBox::information(&dialog, QStringLiteral("没有变化"),
                                             QStringLiteral("库存数量和供应商均未发生变化。"));
                    return;
                }
                if (QMessageBox::warning(
                        &dialog, QStringLiteral("确认直接编辑库存"),
                        QStringLiteral("确认将 %1 在当前位置%2的库存由 %3 修改为 %4？\n\n"
                                       "系统将自动生成盘点调整单和库存流水。")
                            .arg(materialCode,
                                 batchNo.isEmpty() ? QString() : QStringLiteral("、批次 %1").arg(batchNo),
                                 QString::number(systemQuantity, 'g', 12),
                                 QString::number(targetQuantity, 'g', 12)),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                    != QMessageBox::Yes) {
                    return;
                }
                InventoryCountRequest request;
                request.documentDate = dateEdit->date();
                request.handlerName = m_session.displayName;
                request.notes = reason;
                request.submissionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
                InventoryCountLine line;
                line.materialId = materialId;
                line.warehouseId = warehouseCombo->currentData().toLongLong();
                line.locationId = locationCombo->currentData().toLongLong();
                line.batchNo = batchNo;
                line.systemQuantity = systemQuantity;
                line.actualQuantity = targetQuantity;
                line.supplier = supplier.isEmpty() ? QStringLiteral("") : supplier;
                line.differenceReason = reason;
                request.lines.append(line);
                InventoryService service(m_database, m_session.userId);
                PostedDocument posted;
                QString error;
                if (!service.postInventoryCount(request, &posted, &error)) {
                    QMessageBox::warning(&dialog, QStringLiteral("保存库存失败"), error);
                    return;
                }
                dialog.accept();
                emit dataChanged();
                QMessageBox::information(
                    this, QStringLiteral("库存已更新"),
                    QStringLiteral("物料 %1 的库存已修改，调整单号：%2")
                        .arg(materialCode, posted.documentNumber));
            });
    dialog.exec();
}

void MaterialPage::batchEditMaterials()
{
    const QList<qlonglong> materialIds = selectedMaterialIds();
    if (materialIds.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("请选择物料"),
                                 QStringLiteral("请按住 Ctrl 或 Shift 选择需要批量修改的物料。"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("批量编辑物料"));
    dialog.setMinimumWidth(520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(QStringLiteral("已选择 %1 条物料。仅勾选的字段会被统一修改。")
                                .arg(materialIds.size()), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *form = new QFormLayout;

    auto *categoryApply = new QCheckBox(QStringLiteral("修改物料类别"), &dialog);
    auto *categoryCombo = new QComboBox(&dialog);
    QSqlQuery categories(m_database);
    categories.exec(QStringLiteral(
        "SELECT id,name FROM material_categories WHERE is_active=1 ORDER BY sort_order,name"));
    while (categories.next()) categoryCombo->addItem(categories.value(1).toString(), categories.value(0));

    auto *processingApply = new QCheckBox(QStringLiteral("修改加工方式"), &dialog);
    auto *processingCombo = new QComboBox(&dialog);
    processingCombo->setEditable(true);
    processingCombo->addItem(QStringLiteral("未设置"), QStringLiteral(""));
    processingCombo->addItem(QStringLiteral("外购"), QStringLiteral("外购"));
    processingCombo->addItem(QStringLiteral("自制"), QStringLiteral("自制"));
    processingCombo->addItem(QStringLiteral("委外加工"), QStringLiteral("委外加工"));

    auto *unitApply = new QCheckBox(QStringLiteral("修改单位"), &dialog);
    auto *unitEdit = new QLineEdit(&dialog);
    unitEdit->setText(QStringLiteral("个"));

    auto *usageApply = new QCheckBox(QStringLiteral("修改单台用量"), &dialog);
    auto *usageSpin = new QDoubleSpinBox(&dialog);
    usageSpin->setDecimals(6);
    usageSpin->setRange(0, 999999999999.0);

    auto *minimumApply = new QCheckBox(QStringLiteral("修改最低库存"), &dialog);
    auto *minimumSpin = new QDoubleSpinBox(&dialog);
    minimumSpin->setDecimals(6);
    minimumSpin->setRange(0, 999999999999.0);

    auto *statusApply = new QCheckBox(QStringLiteral("修改物料状态"), &dialog);
    auto *statusCombo = new QComboBox(&dialog);
    statusCombo->addItem(QStringLiteral("正常"), true);
    statusCombo->addItem(QStringLiteral("停用"), false);

    form->addRow(categoryApply, categoryCombo);
    form->addRow(processingApply, processingCombo);
    form->addRow(unitApply, unitEdit);
    form->addRow(usageApply, usageSpin);
    form->addRow(minimumApply, minimumSpin);
    form->addRow(statusApply, statusCombo);
    layout->addLayout(form);

    const QList<QPair<QCheckBox *, QWidget *>> controls = {
        {categoryApply, categoryCombo}, {processingApply, processingCombo},
        {unitApply, unitEdit}, {usageApply, usageSpin},
        {minimumApply, minimumSpin}, {statusApply, statusCombo}};
    for (const auto &control : controls) {
        control.second->setEnabled(false);
        connect(control.first, &QCheckBox::toggled, control.second, &QWidget::setEnabled);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("应用修改"));
    buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    QStringList assignments;
    QList<QVariant> values;
    QStringList changedFields;
    if (categoryApply->isChecked()) {
        assignments.append(QStringLiteral("category_id=?"));
        values.append(categoryCombo->currentData());
        changedFields.append(QStringLiteral("物料类别"));
    }
    if (processingApply->isChecked()) {
        assignments.append(QStringLiteral("processing_method=?"));
        QString processingMethod = processingCombo->currentText().trimmed();
        if (processingMethod.isEmpty() || processingMethod == QStringLiteral("未设置"))
            processingMethod = QStringLiteral("");
        values.append(processingMethod);
        changedFields.append(QStringLiteral("加工方式"));
    }
    if (unitApply->isChecked()) {
        if (unitEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("批量修改失败"), QStringLiteral("单位不能为空。"));
            return;
        }
        assignments.append(QStringLiteral("unit=?"));
        values.append(unitEdit->text().trimmed());
        changedFields.append(QStringLiteral("单位"));
    }
    if (usageApply->isChecked()) {
        assignments.append(QStringLiteral("unit_usage=?"));
        values.append(usageSpin->value());
        changedFields.append(QStringLiteral("单台用量"));
    }
    if (minimumApply->isChecked()) {
        assignments.append(QStringLiteral("minimum_stock=?"));
        values.append(minimumSpin->value());
        changedFields.append(QStringLiteral("最低库存"));
    }
    if (statusApply->isChecked()) {
        assignments.append(QStringLiteral("is_active=?"));
        values.append(statusCombo->currentData().toBool());
        changedFields.append(QStringLiteral("物料状态"));
    }
    if (assignments.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("未修改"), QStringLiteral("没有勾选需要修改的字段。"));
        return;
    }
    if (statusApply->isChecked() && !statusCombo->currentData().toBool()) {
        QStringList occupied;
        for (qlonglong materialId : std::as_const(materialIds)) {
            QSqlQuery stock(m_database);
            stock.prepare(QStringLiteral(
                "SELECT m.code,COALESCE(SUM(s.quantity),0) FROM materials m "
                "LEFT JOIN stock_balances s ON s.material_id=m.id WHERE m.id=? GROUP BY m.id"));
            stock.addBindValue(materialId);
            if (stock.exec() && stock.next() && stock.value(1).toDouble() > 0.0000001)
                occupied.append(stock.value(0).toString());
        }
        if (!occupied.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("不能批量停用"),
                QStringLiteral("以下物料仍有库存，请先完成库存处理：\n%1")
                    .arg(occupied.join(QStringLiteral("、"))));
            return;
        }
    }

    QSqlQuery begin(m_database);
    if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        QMessageBox::warning(this, QStringLiteral("批量修改失败"), begin.lastError().text());
        return;
    }
    const QString sql = QStringLiteral("UPDATE materials SET %1,updated_at=? WHERE id=?")
                            .arg(assignments.join(QLatin1Char(',')));
    for (qlonglong materialId : std::as_const(materialIds)) {
        QSqlQuery update(m_database);
        update.prepare(sql);
        for (const QVariant &value : std::as_const(values)) update.addBindValue(value);
        update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        update.addBindValue(materialId);
        if (!update.exec() || update.numRowsAffected() != 1) {
            m_database.rollback();
            QMessageBox::warning(this, QStringLiteral("批量修改失败"), update.lastError().text());
            return;
        }
        QSqlQuery audit(m_database);
        audit.prepare(QStringLiteral(
            "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) VALUES(?,?,?,?,?)"));
        audit.addBindValue(m_session.userId);
        audit.addBindValue(QStringLiteral("MATERIAL_BATCH_UPDATE"));
        audit.addBindValue(QStringLiteral("material"));
        audit.addBindValue(materialId);
        audit.addBindValue(QStringLiteral("批量修改：%1").arg(changedFields.join(QStringLiteral("、"))));
        if (!audit.exec()) {
            m_database.rollback();
            QMessageBox::warning(this, QStringLiteral("批量修改失败"), audit.lastError().text());
            return;
        }
    }
    if (!m_database.commit()) {
        m_database.rollback();
        QMessageBox::warning(this, QStringLiteral("批量修改失败"), m_database.lastError().text());
        return;
    }
    QMessageBox::information(this, QStringLiteral("批量修改完成"),
                             QStringLiteral("已更新 %1 条物料。").arg(materialIds.size()));
    refresh();
    emit dataChanged();
}

void MaterialPage::manageProjects()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("项目代码维护"));
    dialog.resize(620, 430);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(QStringLiteral(
        "项目代码用于自动生成物料编码。代码创建后不可修改；停用后不再用于新物料。"), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *table = new QTableWidget(0, 4, &dialog);
    table->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("项目代码"),
                                      QStringLiteral("项目名称"), QStringLiteral("状态")});
    table->hideColumn(0);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(table, 1);

    auto reload = [this, table] {
        table->setRowCount(0);
        QSqlQuery query(m_database);
        query.exec(QStringLiteral(
            "SELECT id,code,name,is_active FROM material_projects ORDER BY code"));
        while (query.next()) {
            const int row = table->rowCount();
            table->insertRow(row);
            table->setItem(row, 0, new QTableWidgetItem(query.value(0).toString()));
            table->setItem(row, 1, new QTableWidgetItem(query.value(1).toString()));
            table->setItem(row, 2, new QTableWidgetItem(query.value(2).toString()));
            table->setItem(row, 3, new QTableWidgetItem(query.value(3).toBool()
                ? QStringLiteral("启用") : QStringLiteral("停用")));
        }
        if (table->rowCount() > 0) table->selectRow(0);
    };
    auto selectedId = [table] {
        const int row = table->currentRow();
        return row >= 0 && table->item(row, 0)
            ? table->item(row, 0)->text().toLongLong() : 0;
    };
    auto writeAudit = [this](const QString &action, qlonglong id, const QString &detail) {
        QSqlQuery audit(m_database);
        audit.prepare(QStringLiteral(
            "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) VALUES(?,?,?,?,?)"));
        audit.addBindValue(m_session.userId);
        audit.addBindValue(action);
        audit.addBindValue(QStringLiteral("material_project"));
        audit.addBindValue(id);
        audit.addBindValue(detail);
        audit.exec();
    };

    auto *actions = new QHBoxLayout;
    auto *addButton = new QPushButton(QStringLiteral("新增项目代码"), &dialog);
    addButton->setProperty("primary", true);
    auto *renameButton = new QPushButton(QStringLiteral("修改名称"), &dialog);
    auto *toggleButton = new QPushButton(QStringLiteral("启用/停用"), &dialog);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), &dialog);
    actions->addWidget(addButton);
    actions->addWidget(renameButton);
    actions->addWidget(toggleButton);
    actions->addStretch();
    actions->addWidget(closeButton);
    layout->addLayout(actions);

    connect(addButton, &QPushButton::clicked, &dialog, [this, &dialog, reload, writeAudit] {
        bool accepted = false;
        QString code = QInputDialog::getText(&dialog, QStringLiteral("新增项目代码"),
            QStringLiteral("项目代码（例如 SM01）"), QLineEdit::Normal, {}, &accepted);
        if (!accepted) return;
        code = MaterialCodeService::normalizeProjectCode(code);
        if (!MaterialCodeService::isValidProjectCode(code)) {
            QMessageBox::warning(&dialog, QStringLiteral("项目代码无效"),
                QStringLiteral("项目代码必须以字母开头，只能包含2至8位大写字母或数字。"));
            return;
        }
        QString name = QInputDialog::getText(&dialog, QStringLiteral("新增项目代码"),
            QStringLiteral("项目名称"), QLineEdit::Normal, code, &accepted).trimmed();
        if (!accepted) return;
        if (name.isEmpty()) name = code;
        QSqlQuery insert(m_database);
        insert.prepare(QStringLiteral(
            "INSERT INTO material_projects(code,name) VALUES(?,?)"));
        insert.addBindValue(code);
        insert.addBindValue(name);
        if (!insert.exec()) {
            QMessageBox::warning(&dialog, QStringLiteral("新增失败"),
                QStringLiteral("项目代码已存在或无法保存：%1").arg(insert.lastError().text()));
            return;
        }
        writeAudit(QStringLiteral("MATERIAL_PROJECT_CREATE"),
                   insert.lastInsertId().toLongLong(), code + QStringLiteral(" - ") + name);
        reload();
    });
    connect(renameButton, &QPushButton::clicked, &dialog,
            [this, &dialog, table, selectedId, reload, writeAudit] {
        const qlonglong id = selectedId();
        if (id <= 0) return;
        bool accepted = false;
        const QString name = QInputDialog::getText(&dialog, QStringLiteral("修改项目名称"),
            QStringLiteral("项目名称"), QLineEdit::Normal,
            table->item(table->currentRow(), 2)->text(), &accepted).trimmed();
        if (!accepted || name.isEmpty()) return;
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral(
            "UPDATE material_projects SET name=?,updated_at=? WHERE id=?"));
        update.addBindValue(name);
        update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        update.addBindValue(id);
        if (!update.exec()) {
            QMessageBox::warning(&dialog, QStringLiteral("修改失败"), update.lastError().text());
            return;
        }
        writeAudit(QStringLiteral("MATERIAL_PROJECT_UPDATE"), id, name);
        reload();
    });
    connect(toggleButton, &QPushButton::clicked, &dialog,
            [this, &dialog, selectedId, reload, writeAudit] {
        const qlonglong id = selectedId();
        if (id <= 0) return;
        QSqlQuery current(m_database);
        current.prepare(QStringLiteral("SELECT code,is_active FROM material_projects WHERE id=?"));
        current.addBindValue(id);
        if (!current.exec() || !current.next()) return;
        const bool enable = !current.value(1).toBool();
        if (!enable) {
            QSqlQuery active(m_database);
            active.exec(QStringLiteral("SELECT COUNT(*) FROM material_projects WHERE is_active=1"));
            if (active.next() && active.value(0).toInt() <= 1) {
                QMessageBox::warning(&dialog, QStringLiteral("不能停用"),
                    QStringLiteral("至少需要保留一个启用的项目代码。"));
                return;
            }
        }
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral(
            "UPDATE material_projects SET is_active=?,updated_at=? WHERE id=?"));
        update.addBindValue(enable);
        update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        update.addBindValue(id);
        if (!update.exec()) {
            QMessageBox::warning(&dialog, QStringLiteral("修改失败"), update.lastError().text());
            return;
        }
        writeAudit(QStringLiteral("MATERIAL_PROJECT_TOGGLE"), id,
                   current.value(0).toString() + (enable ? QStringLiteral(" 启用")
                                                       : QStringLiteral(" 停用")));
        reload();
    });
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(table, &QTableWidget::doubleClicked, renameButton, &QPushButton::click);
    reload();
    TableExcelExport::install(&dialog, QStringLiteral("项目代码记录"));
    dialog.exec();
}

void MaterialPage::deleteMaterials()
{
    if (!m_session.canManageMaterials()) return;
    const QList<qlonglong> ids = selectedMaterialIds();
    if (ids.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("请选择物料"),
                                 QStringLiteral("请先选择需要删除的物料，可按住 Ctrl 或 Shift 多选。"));
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("确认删除物料"),
            QStringLiteral("确认删除选中的 %1 条物料？\n\n"
                           "已经产生库存、单据、批次、SN或盘点记录的物料不会被删除，"
                           "请在编辑页面将其设为停用。").arg(ids.size()))
        != QMessageBox::Yes) return;

    if (!m_database.transaction()) {
        QMessageBox::warning(this, QStringLiteral("删除失败"), m_database.lastError().text());
        return;
    }

    int deletedCount = 0;
    QStringList retainedMaterials;
    QSqlQuery info(m_database);
    QSqlQuery used(m_database);
    QSqlQuery removeImage(m_database);
    QSqlQuery removeMaterial(m_database);
    QSqlQuery audit(m_database);
    info.prepare(QStringLiteral("SELECT code,name FROM materials WHERE id=?"));
    used.prepare(QStringLiteral(
        "SELECT EXISTS(SELECT 1 FROM business_document_items WHERE material_id=?) "
        "OR EXISTS(SELECT 1 FROM stock_balances WHERE material_id=?) "
        "OR EXISTS(SELECT 1 FROM inventory_ledger WHERE material_id=?) "
        "OR EXISTS(SELECT 1 FROM batches WHERE material_id=?) "
        "OR EXISTS(SELECT 1 FROM serial_numbers WHERE material_id=?) "
        "OR EXISTS(SELECT 1 FROM production_runs WHERE product_material_id=?) "
        "OR EXISTS(SELECT 1 FROM inventory_count_items WHERE material_id=?) "
        "OR EXISTS(SELECT 1 FROM material_bom_items WHERE product_material_id=?) "
        "OR EXISTS(SELECT 1 FROM material_bom_items WHERE component_material_id=?)"));
    removeImage.prepare(QStringLiteral("DELETE FROM material_images WHERE material_id=?"));
    removeMaterial.prepare(QStringLiteral("DELETE FROM materials WHERE id=?"));
    audit.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
        "VALUES(?,'MATERIAL_DELETE','material',?,?)"));

    QString error;
    for (qlonglong id : ids) {
        info.bindValue(0, id);
        if (!info.exec() || !info.next()) {
            error = info.lastError().text();
            if (error.isEmpty()) error = QStringLiteral("所选物料已不存在，请刷新后重试。");
            break;
        }
        const QString code = info.value(0).toString();
        const QString name = info.value(1).toString();
        for (int parameter = 0; parameter < 9; ++parameter) used.bindValue(parameter, id);
        if (!used.exec() || !used.next()) {
            error = used.lastError().text();
            break;
        }
        if (used.value(0).toBool()) {
            retainedMaterials.append(QStringLiteral("%1 - %2").arg(code, name));
            continue;
        }

        removeImage.bindValue(0, id);
        if (!removeImage.exec()) {
            error = removeImage.lastError().text();
            break;
        }
        removeMaterial.bindValue(0, id);
        if (!removeMaterial.exec() || removeMaterial.numRowsAffected() != 1) {
            error = removeMaterial.lastError().text();
            if (error.isEmpty()) error = QStringLiteral("物料删除未生效，请刷新后重试。");
            break;
        }
        audit.bindValue(0, m_session.userId);
        audit.bindValue(1, id);
        audit.bindValue(2, QStringLiteral("%1 - %2").arg(code, name));
        if (!audit.exec()) {
            error = audit.lastError().text();
            break;
        }
        ++deletedCount;
    }

    if (!error.isEmpty() || !m_database.commit()) {
        m_database.rollback();
        QMessageBox::warning(this, QStringLiteral("删除失败"),
                             error.isEmpty() ? m_database.lastError().text() : error);
        return;
    }

    QString result = QStringLiteral("已删除 %1 条物料。").arg(deletedCount);
    if (!retainedMaterials.isEmpty()) {
        result += QStringLiteral("\n\n以下 %1 条物料已有业务记录，已保留：\n%2")
                      .arg(retainedMaterials.size())
                      .arg(retainedMaterials.join(QLatin1Char('\n')));
    }
    QMessageBox::information(this, QStringLiteral("删除完成"), result);
    refresh();
    if (deletedCount > 0) emit dataChanged();
}

void MaterialPage::importBom()
{
    if (!m_session.canManageMaterials()) return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择BOM Excel"), {}, QStringLiteral("Excel工作簿 (*.xlsx)"));
    if (path.isEmpty()) return;

    BomImportResult result;
    QString error;
    if (!BomExcelImporter::parseFile(path, &result, &error)) {
        QMessageBox::warning(this, QStringLiteral("BOM解析失败"), error);
        return;
    }
    int semiFinishedCount = 0;
    int leafCount = 0;
    for (int index = 1; index < result.rows.size(); ++index) {
        if (result.rows.at(index).categoryCode == QStringLiteral("SEMI"))
            ++semiFinishedCount;
        else
            ++leafCount;
    }
    if (QMessageBox::question(
            this, QStringLiteral("确认导入BOM"),
            QStringLiteral("已识别成品 %1，共 %2 个BOM节点（半成品 %3 个、原材料/辅料 %4 个）。\n\n"
                           "继续后将创建或更新相关物料，并替换该成品现有的整棵BOM。")
                .arg(result.productCode)
                .arg(result.rows.size() - 1)
                .arg(semiFinishedCount)
                .arg(leafCount)) != QMessageBox::Yes) {
        return;
    }

    int created = 0;
    int updated = 0;
    if (!BomExcelImporter::importRows(m_database, m_session.userId, result,
                                      &created, &updated, &error)) {
        QMessageBox::warning(this, QStringLiteral("BOM导入失败"), error);
        return;
    }
    refresh();
    const int productIndex = m_bomProductCombo->findData(
        result.productCode, BomProductCodeRole);
    if (productIndex >= 0) m_bomProductCombo->setCurrentIndex(productIndex);
    m_viewTabs->setCurrentIndex(0);
    QMessageBox::information(
        this, QStringLiteral("BOM导入完成"),
        QStringLiteral("成品 %1 的BOM已导入：新增物料 %2 条、更新物料 %3 条、层级节点 %4 个。")
            .arg(result.productCode)
            .arg(created)
            .arg(updated)
            .arg(result.rows.size() - 1));
    emit dataChanged();
}

void MaterialPage::addBomChild()
{
    if (!m_session.canManageMaterials()) return;
    QTreeWidgetItem *parentItem = m_bomTree->currentItem();
    const qlonglong productId = m_bomProductCombo->currentData().toLongLong();
    if (!parentItem || productId <= 0) {
        QMessageBox::information(this, QStringLiteral("请选择上级"),
                                 QStringLiteral("请先选择成品或BOM中的一个上级节点。"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("添加BOM下级"));
    dialog.setMinimumWidth(620);
    auto *dialogLayout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto *materialCombo = new QComboBox(&dialog);
    ComboBoxSearch::enableContainsSearch(
        materialCombo, QStringLiteral("输入物料号或名称/规格关键词"));
    materialCombo->setMinimumWidth(460);
    loadBomMaterialOptions(m_database, materialCombo, productId);
    for (QTreeWidgetItem *ancestor = parentItem; ancestor; ancestor = ancestor->parent()) {
        const qlonglong ancestorMaterialId = ancestor->data(0, BomMaterialIdRole).toLongLong();
        const int ancestorIndex = materialCombo->findData(ancestorMaterialId);
        if (ancestorIndex >= 0) materialCombo->removeItem(ancestorIndex);
    }
    if (materialCombo->count() == 0) {
        QMessageBox::information(this, QStringLiteral("没有可选物料"),
                                 QStringLiteral("请先建立半成品、原材料或辅料档案。"));
        return;
    }
    materialCombo->setCurrentIndex(-1);
    auto *materialSummary = new QLabel(&dialog);
    configureBomMaterialSummary(materialSummary);
    updateBomMaterialSummary(materialCombo, materialSummary);
    auto *quantitySpin = new QDoubleSpinBox(&dialog);
    quantitySpin->setDecimals(6);
    quantitySpin->setRange(0.000001, 999999999999.0);
    quantitySpin->setValue(1.0);
    form->addRow(QStringLiteral("上级节点"), new QLabel(parentItem->text(0), &dialog));
    form->addRow(QStringLiteral("下级物料 *"), materialCombo);
    form->addRow(QStringLiteral("识别结果"), materialSummary);
    form->addRow(QStringLiteral("相对上级用量 *"), quantitySpin);
    dialogLayout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("添加"));
    buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    dialogLayout->addWidget(buttons);
    connect(materialCombo, &QComboBox::editTextChanged, &dialog,
            [materialCombo, materialSummary] {
                updateBomMaterialSummary(materialCombo, materialSummary);
            });
    connect(buttons->button(QDialogButtonBox::Ok), &QPushButton::clicked,
            &dialog, [&dialog, materialCombo] {
        const int index = resolveBomMaterialIndex(materialCombo);
        if (index < 0) {
            QMessageBox::warning(
                &dialog, QStringLiteral("请选择下级物料"),
                QStringLiteral("没有唯一识别到物料。请输入完整物料号，或从自动补全结果中选择一条。"));
            materialCombo->setFocus();
            return;
        }
        materialCombo->setCurrentIndex(index);
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    const int selectedIndex = materialCombo->currentIndex();
    const qlonglong componentId = materialCombo->itemData(selectedIndex).toLongLong();
    const QString selected = materialCombo->itemText(selectedIndex);
    const double quantity = quantitySpin->value();

    const qlonglong parentBomItemId = parentItem->data(0, BomItemIdRole).toLongLong();
    QSqlQuery duplicate(m_database);
    duplicate.prepare(parentBomItemId > 0
        ? QStringLiteral("SELECT 1 FROM material_bom_items WHERE product_material_id=? "
                         "AND parent_item_id=? AND component_material_id=?")
        : QStringLiteral("SELECT 1 FROM material_bom_items WHERE product_material_id=? "
                         "AND parent_item_id IS NULL AND component_material_id=?"));
    duplicate.addBindValue(productId);
    if (parentBomItemId > 0) duplicate.addBindValue(parentBomItemId);
    duplicate.addBindValue(componentId);
    if (duplicate.exec() && duplicate.next()) {
        QMessageBox::information(this, QStringLiteral("节点已存在"),
                                 QStringLiteral("该上级下已经有此物料，请直接修改原节点用量。"));
        return;
    }

    QSqlQuery order(m_database);
    order.prepare(parentBomItemId > 0
        ? QStringLiteral("SELECT COALESCE(MAX(sort_order),0)+1 FROM material_bom_items "
                         "WHERE product_material_id=? AND parent_item_id=?")
        : QStringLiteral("SELECT COALESCE(MAX(sort_order),0)+1 FROM material_bom_items "
                         "WHERE product_material_id=? AND parent_item_id IS NULL"));
    order.addBindValue(productId);
    if (parentBomItemId > 0) order.addBindValue(parentBomItemId);
    int sortOrder = 1;
    if (order.exec() && order.next()) sortOrder = order.value(0).toInt();

    if (!m_database.transaction()) {
        QMessageBox::warning(this, QStringLiteral("添加失败"), m_database.lastError().text());
        return;
    }
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO material_bom_items(product_material_id,parent_item_id,"
        "component_material_id,quantity,sort_order) VALUES(?,?,?,?,?)"));
    insert.addBindValue(productId);
    insert.addBindValue(parentBomItemId > 0 ? QVariant(parentBomItemId) : QVariant());
    insert.addBindValue(componentId);
    insert.addBindValue(quantity);
    insert.addBindValue(sortOrder);
    QSqlQuery audit(m_database);
    audit.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
        "VALUES(?,'MATERIAL_BOM_ADD','material',?,?)"));
    audit.addBindValue(m_session.userId);
    audit.addBindValue(productId);
    audit.addBindValue(selected + QStringLiteral("，用量 ")
                           + QString::number(quantity, 'g', 12));
    if (!insert.exec() || !audit.exec() || !m_database.commit()) {
        const QString message = !insert.lastError().text().isEmpty()
            ? insert.lastError().text() : (!audit.lastError().text().isEmpty()
                ? audit.lastError().text() : m_database.lastError().text());
        m_database.rollback();
        QMessageBox::warning(this, QStringLiteral("添加失败"), message);
        return;
    }
    refreshBomTree();
    emit dataChanged();
}

void MaterialPage::editBomQuantity()
{
    if (!m_session.canManageMaterials()) return;
    QTreeWidgetItem *item = m_bomTree->currentItem();
    const qlonglong itemId = item ? item->data(0, BomItemIdRole).toLongLong() : 0;
    const qlonglong materialId = item ? item->data(0, BomMaterialIdRole).toLongLong() : 0;
    const qlonglong productId = m_bomProductCombo->currentData().toLongLong();
    if (!item || materialId <= 0 || productId <= 0) {
        QMessageBox::information(this, QStringLiteral("请选择BOM节点"),
                                 QStringLiteral("请先选择需要调整的成品或下级节点。"));
        return;
    }

    if (itemId <= 0) {
        MaterialDialog dialog(m_database, materialId, m_session.userId, this);
        if (dialog.exec() == QDialog::Accepted) {
            refresh();
            emit dataChanged();
        }
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("调整BOM节点"));
    dialog.setMinimumWidth(620);
    auto *dialogLayout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto *materialCombo = new QComboBox(&dialog);
    ComboBoxSearch::enableContainsSearch(
        materialCombo, QStringLiteral("输入物料号或名称/规格关键词"));
    materialCombo->setMinimumWidth(460);
    loadBomMaterialOptions(m_database, materialCombo, productId, materialId);
    for (QTreeWidgetItem *ancestor = item->parent(); ancestor; ancestor = ancestor->parent()) {
        const qlonglong ancestorMaterialId = ancestor->data(0, BomMaterialIdRole).toLongLong();
        const int ancestorIndex = materialCombo->findData(ancestorMaterialId);
        if (ancestorIndex >= 0) materialCombo->removeItem(ancestorIndex);
    }
    QList<QTreeWidgetItem *> descendants;
    for (int childIndex = 0; childIndex < item->childCount(); ++childIndex)
        descendants.append(item->child(childIndex));
    while (!descendants.isEmpty()) {
        QTreeWidgetItem *descendant = descendants.takeLast();
        const qlonglong descendantMaterialId =
            descendant->data(0, BomMaterialIdRole).toLongLong();
        const int descendantIndex = materialCombo->findData(descendantMaterialId);
        if (descendantIndex >= 0) materialCombo->removeItem(descendantIndex);
        for (int childIndex = 0; childIndex < descendant->childCount(); ++childIndex)
            descendants.append(descendant->child(childIndex));
    }
    const int currentMaterialIndex = materialCombo->findData(materialId);
    materialCombo->setCurrentIndex(currentMaterialIndex);
    auto *materialSummary = new QLabel(&dialog);
    configureBomMaterialSummary(materialSummary);
    updateBomMaterialSummary(materialCombo, materialSummary);
    auto *quantitySpin = new QDoubleSpinBox(&dialog);
    quantitySpin->setDecimals(6);
    quantitySpin->setRange(0.000001, 999999999999.0);
    quantitySpin->setValue(item->text(4).toDouble());
    form->addRow(QStringLiteral("当前节点"), new QLabel(item->text(0), &dialog));
    form->addRow(QStringLiteral("节点物料 *"), materialCombo);
    form->addRow(QStringLiteral("识别结果"), materialSummary);
    form->addRow(QStringLiteral("相对上级用量 *"), quantitySpin);
    dialogLayout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存调整"));
    buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    dialogLayout->addWidget(buttons);
    connect(materialCombo, &QComboBox::editTextChanged, &dialog,
            [materialCombo, materialSummary] {
                updateBomMaterialSummary(materialCombo, materialSummary);
            });
    connect(buttons->button(QDialogButtonBox::Ok), &QPushButton::clicked,
            &dialog, [&dialog, materialCombo] {
        const int index = resolveBomMaterialIndex(materialCombo);
        if (index < 0) {
            QMessageBox::warning(
                &dialog, QStringLiteral("请选择节点物料"),
                QStringLiteral("没有唯一识别到物料。请输入完整物料号，或从自动补全结果中选择一条。"));
            materialCombo->setFocus();
            return;
        }
        materialCombo->setCurrentIndex(index);
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    const int selectedIndex = materialCombo->currentIndex();
    const qlonglong newMaterialId = materialCombo->itemData(selectedIndex).toLongLong();
    const double quantity = quantitySpin->value();
    const qlonglong parentItemId = item->parent()
        ? item->parent()->data(0, BomItemIdRole).toLongLong() : 0;
    QSqlQuery duplicate(m_database);
    duplicate.prepare(parentItemId > 0
        ? QStringLiteral("SELECT 1 FROM material_bom_items WHERE product_material_id=? "
                         "AND parent_item_id=? AND component_material_id=? AND id<>?")
        : QStringLiteral("SELECT 1 FROM material_bom_items WHERE product_material_id=? "
                         "AND parent_item_id IS NULL AND component_material_id=? AND id<>?"));
    duplicate.addBindValue(productId);
    if (parentItemId > 0) duplicate.addBindValue(parentItemId);
    duplicate.addBindValue(newMaterialId);
    duplicate.addBindValue(itemId);
    if (duplicate.exec() && duplicate.next()) {
        QMessageBox::information(this, QStringLiteral("节点已存在"),
                                 QStringLiteral("该上级下已经有此物料，不能将两个节点调整为同一物料。"));
        return;
    }

    if (!m_database.transaction()) {
        QMessageBox::warning(this, QStringLiteral("调整失败"), m_database.lastError().text());
        return;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE material_bom_items SET component_material_id=?,quantity=?,updated_at=? WHERE id=?"));
    update.addBindValue(newMaterialId);
    update.addBindValue(quantity);
    update.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    update.addBindValue(itemId);
    QSqlQuery audit(m_database);
    audit.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
        "VALUES(?,'MATERIAL_BOM_ADJUST','material_bom_item',?,?)"));
    audit.addBindValue(m_session.userId);
    audit.addBindValue(itemId);
    audit.addBindValue(QStringLiteral("节点调整为 %1，相对上级用量 %2")
                           .arg(materialCombo->itemText(selectedIndex),
                                QString::number(quantity, 'g', 12)));
    if (!update.exec() || update.numRowsAffected() != 1
        || !audit.exec() || !m_database.commit()) {
        const QString error = !update.lastError().text().isEmpty()
            ? update.lastError().text() : (!audit.lastError().text().isEmpty()
                ? audit.lastError().text() : m_database.lastError().text());
        m_database.rollback();
        QMessageBox::warning(this, QStringLiteral("调整失败"), error);
        return;
    }
    refreshBomTree();
    emit dataChanged();
}

void MaterialPage::removeBomItem()
{
    if (!m_session.canManageMaterials()) return;
    QTreeWidgetItem *item = m_bomTree->currentItem();
    const qlonglong itemId = item ? item->data(0, BomItemIdRole).toLongLong() : 0;
    if (itemId <= 0) {
        if (item && item->data(0, BomMaterialIdRole).toLongLong() > 0) clearBom();
        else QMessageBox::information(this, QStringLiteral("请选择BOM节点"),
                                      QStringLiteral("请先选择需要删除的BOM节点。"));
        return;
    }
    int subtreeCount = 1;
    QSqlQuery subtree(m_database);
    subtree.prepare(QStringLiteral(
        "WITH RECURSIVE nodes(id) AS (SELECT id FROM material_bom_items WHERE id=? "
        "UNION ALL SELECT b.id FROM material_bom_items b JOIN nodes n ON b.parent_item_id=n.id) "
        "SELECT COUNT(*) FROM nodes"));
    subtree.addBindValue(itemId);
    if (subtree.exec() && subtree.next()) subtreeCount = subtree.value(0).toInt();
    if (QMessageBox::question(
            this, QStringLiteral("确认移除BOM节点"),
            QStringLiteral("确认从BOM中移除“%1”？该节点及其 %2 个下级节点将一并移除，物料档案不会删除。")
                .arg(item->text(0)).arg(qMax(0, subtreeCount - 1))) != QMessageBox::Yes) {
        return;
    }
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM material_bom_items WHERE id=?"));
    remove.addBindValue(itemId);
    if (!remove.exec() || remove.numRowsAffected() != 1) {
        QMessageBox::warning(this, QStringLiteral("移除失败"), remove.lastError().text());
        return;
    }
    QSqlQuery audit(m_database);
    audit.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
        "VALUES(?,'MATERIAL_BOM_REMOVE','material_bom_item',?,?)"));
    audit.addBindValue(m_session.userId);
    audit.addBindValue(itemId);
    audit.addBindValue(item->text(0) + QStringLiteral("，共移除%1个节点").arg(subtreeCount));
    audit.exec();
    refreshBomTree();
    emit dataChanged();
}

void MaterialPage::clearBom()
{
    if (!m_session.canManageMaterials()) return;
    const qlonglong productId = m_bomProductCombo->currentData().toLongLong();
    if (productId <= 0) {
        QMessageBox::information(this, QStringLiteral("请选择成品"),
                                 QStringLiteral("请先选择需要清空BOM的成品。"));
        return;
    }

    QSqlQuery count(m_database);
    count.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM material_bom_items WHERE product_material_id=?"));
    count.addBindValue(productId);
    if (!count.exec() || !count.next()) {
        QMessageBox::warning(this, QStringLiteral("读取BOM失败"), count.lastError().text());
        return;
    }
    const int nodeCount = count.value(0).toInt();
    if (nodeCount <= 0) {
        QMessageBox::information(this, QStringLiteral("BOM为空"),
                                 QStringLiteral("当前成品没有可清空的BOM节点。"));
        refreshBomTree();
        return;
    }

    const QString productText = m_bomProductCombo->currentText();
    if (QMessageBox::warning(
            this, QStringLiteral("确认清空整棵BOM"),
            QStringLiteral("确认清空“%1”的整棵BOM吗？共 %2 个节点。\n\n"
                           "仅删除该成品的全部 BOM 关系，保留成品及所有物料档案、库存和历史记录。\n\n"
                           "此操作不能撤销。")
                .arg(productText).arg(nodeCount),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    if (!m_database.transaction()) {
        QMessageBox::warning(this, QStringLiteral("清空失败"), m_database.lastError().text());
        return;
    }
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral(
        "DELETE FROM material_bom_items WHERE product_material_id=?"));
    remove.addBindValue(productId);
    if (!remove.exec() || remove.numRowsAffected() != nodeCount) {
        const QString error = remove.lastError().text().isEmpty()
            ? QStringLiteral("BOM节点数量已发生变化，请刷新后重试。")
            : remove.lastError().text();
        m_database.rollback();
        QMessageBox::warning(this, QStringLiteral("清空失败"), error);
        return;
    }
    QSqlQuery audit(m_database);
    audit.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
        "VALUES(?,'MATERIAL_BOM_CLEAR','material',?,?)"));
    audit.addBindValue(m_session.userId);
    audit.addBindValue(productId);
    audit.addBindValue(QStringLiteral("%1，清空整棵BOM，共%2个节点")
                           .arg(productText).arg(nodeCount));
    if (!audit.exec() || !m_database.commit()) {
        const QString error = audit.lastError().text().isEmpty()
            ? m_database.lastError().text() : audit.lastError().text();
        m_database.rollback();
        QMessageBox::warning(this, QStringLiteral("清空失败"), error);
        return;
    }

    refreshBomTree();
    emit dataChanged();
    QMessageBox::information(
        this, QStringLiteral("BOM已清空"),
        QStringLiteral("已清空“%1”的 %2 个BOM节点。物料档案、库存和历史记录均已保留。")
            .arg(productText).arg(nodeCount));
}

void MaterialPage::importMaterials()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择物料导入Excel"), {},
                                                       QStringLiteral("Excel工作簿 (*.xlsx)"));
    if (path.isEmpty()) return;
    QList<MaterialImportRow> rows;
    QString error;
    if (!MaterialExcelImporter::parseFile(path, &rows, &error)) {
        QMessageBox::warning(this, QStringLiteral("解析失败"), error);
        return;
    }
    MaterialExcelImporter::validateReferences(m_database, &rows);
    int readyCount = 0;
    int warningCount = 0;
    int errorCount = 0;
    int skippedCount = 0;
    for (const MaterialImportRow &row : std::as_const(rows)) {
        if (row.status == MaterialImportStatus::Ready) ++readyCount;
        else if (row.status == MaterialImportStatus::Warning) ++warningCount;
        else if (row.status == MaterialImportStatus::Error) ++errorCount;
        else ++skippedCount;
    }

    QDialog preview(this);
    preview.setWindowTitle(QStringLiteral("物料Excel导入预览"));
    preview.setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    if (const QScreen *screen = preview.screen()) {
        const QSize available = screen->availableGeometry().size();
        preview.resize(qMax(640, qMin(1500, available.width() - 48)),
                       qMax(480, qMin(820, available.height() - 72)));
    } else {
        preview.resize(1360, 760);
    }
    auto *layout = new QVBoxLayout(&preview);
    auto *summary = new QLabel(
        QStringLiteral("共 %1 行：可导入 %2 行，警告 %3 行，错误 %4 行，自动跳过 %5 行。错误行不会导入。")
            .arg(rows.size()).arg(readyCount).arg(warningCount).arg(errorCount).arg(skippedCount), &preview);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto *table = new QTableWidget(rows.size(), 16, &preview);
    table->setHorizontalHeaderLabels({QStringLiteral("状态"), QStringLiteral("Excel行"),
        QStringLiteral("物料编码"), QStringLiteral("物料名称"), QStringLiteral("规格"),
        QStringLiteral("物料类别"), QStringLiteral("加工方式"), QStringLiteral("品牌"),
        QStringLiteral("单位"), QStringLiteral("单台用量"), QStringLiteral("现有库存"),
        QStringLiteral("最低库存"), QStringLiteral("默认仓库/库位"),
        QStringLiteral("批次/SN"), QStringLiteral("物料状态"), QStringLiteral("说明")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setWordWrap(true);
    table->setTextElideMode(Qt::ElideNone);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    for (int index = 0; index < rows.size(); ++index) {
        const MaterialImportRow &row = rows.at(index);
        QString tracking = QStringLiteral("%1 / %2")
            .arg(row.requireBatch ? QStringLiteral("批次") : QStringLiteral("无批次"),
                 row.requireSerial ? QStringLiteral("SN") : QStringLiteral("无SN"));
        if (!row.inventoryBatch.isEmpty()) tracking += QStringLiteral(" / 库存批次：") + row.inventoryBatch;
        if (!row.inventorySerialNumbers.isEmpty())
            tracking += QStringLiteral(" / 库存SN：%1个").arg(row.inventorySerialNumbers.size());
        const QString location = row.defaultWarehouseCode.isEmpty()
            ? QStringLiteral("未设置")
            : row.defaultWarehouseCode + (row.defaultLocationCode.isEmpty()
                ? QStringLiteral(" / 未设置库位")
                : QStringLiteral(" / ") + row.defaultLocationCode);
        const QString category = row.categoryName.isEmpty() ? row.categoryCode : row.categoryName;
        const QString currentStock = row.currentStockProvided
            ? QString::number(row.currentStock, 'g', 15) : QStringLiteral("未提供");
        const QStringList values = {MaterialExcelImporter::statusText(row.status),
            QString::number(row.sourceRow), row.materialCode, row.materialName, row.specification,
            category, row.processingMethod, row.brand, row.unit,
            QString::number(row.unitUsage, 'g', 15), currentStock,
            QString::number(row.minimumStock, 'g', 15), location, tracking,
            row.isActive ? QStringLiteral("正常") : QStringLiteral("停用"), row.message};
        QColor background;
        QColor foreground(QStringLiteral("#263238"));
        switch (row.status) {
        case MaterialImportStatus::Ready:
            background = QColor(QStringLiteral("#E4F5E8"));
            break;
        case MaterialImportStatus::Warning:
            background = QColor(QStringLiteral("#FFF0B3"));
            foreground = QColor(QStringLiteral("#6B4F00"));
            break;
        case MaterialImportStatus::Error:
            background = QColor(QStringLiteral("#FFD9D9"));
            foreground = QColor(QStringLiteral("#8A1C1C"));
            break;
        case MaterialImportStatus::Skipped:
            background = QColor(QStringLiteral("#E7EAF0"));
            foreground = QColor(QStringLiteral("#4E5969"));
            break;
        }
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setBackground(QBrush(background));
            item->setForeground(QBrush(foreground));
            item->setToolTip(row.message);
            if (column == 0) {
                QFont font = item->font();
                font.setBold(true);
                item->setFont(font);
            }
            table->setItem(index, column, item);
        }
    }
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(15, QHeaderView::Interactive);
    table->setColumnWidth(15, 480);
    layout->addWidget(table, 1);

    auto *detailLabel = new QLabel(&preview);
    detailLabel->setWordWrap(true);
    detailLabel->setMinimumHeight(48);
    detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: #F5F7FA; border: 1px solid #D8DEE8; "
        "border-radius: 4px; padding: 8px; color: #263238; }"));
    auto showFullMessage = [detailLabel, &rows](int row) {
        if (row < 0 || row >= rows.size()) {
            detailLabel->setText(QStringLiteral("选择一行可查看完整说明。"));
            return;
        }
        const MaterialImportRow &record = rows.at(row);
        detailLabel->setText(QStringLiteral("第 %1 行 · %2：%3")
            .arg(record.sourceRow)
            .arg(MaterialExcelImporter::statusText(record.status), record.message));
    };
    connect(table, &QTableWidget::currentCellChanged, &preview,
            [showFullMessage](int currentRow, int, int, int) { showFullMessage(currentRow); });
    if (!rows.isEmpty()) {
        table->setCurrentCell(0, 0);
        showFullMessage(0);
    } else {
        showFullMessage(-1);
    }
    layout->addWidget(detailLabel);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &preview);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("导入可用记录"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(readyCount + warningCount > 0);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &preview, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
    if (preview.exec() != QDialog::Accepted) return;
    if (QMessageBox::question(this, QStringLiteral("确认导入"),
        QStringLiteral("确认导入 %1 条物料？已存在的物料将更新资料。")
            .arg(readyCount + warningCount)) != QMessageBox::Yes) return;
    int created = 0;
    int updated = 0;
    if (!MaterialExcelImporter::importRows(m_database, m_session.userId, rows,
                                           &created, &updated, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }

    QMap<QString, InitialInventoryRequest> stockRequests;
    QStringList stockErrors;
    for (const MaterialImportRow &row : std::as_const(rows)) {
        if (!row.importCurrentStock || row.currentStock <= 0.0000001
            || (row.status != MaterialImportStatus::Ready
                && row.status != MaterialImportStatus::Warning)) continue;
        QSqlQuery location(m_database);
        location.prepare(QStringLiteral(
            "SELECT w.id,l.id FROM warehouses w JOIN locations l ON l.warehouse_id=w.id "
            "WHERE w.code=? AND l.code=? AND w.is_active=1 AND l.is_active=1"));
        location.addBindValue(row.defaultWarehouseCode);
        location.addBindValue(row.defaultLocationCode);
        if (!location.exec() || !location.next()) {
            stockErrors.append(QStringLiteral("第%1行：仓库或库位已变化").arg(row.sourceRow));
            continue;
        }
        const qlonglong warehouseId = location.value(0).toLongLong();
        const qlonglong locationId = location.value(1).toLongLong();
        const QString key = QStringLiteral("%1|%2").arg(warehouseId).arg(locationId);
        InitialInventoryRequest &request = stockRequests[key];
        request.documentDate = QDate::currentDate();
        request.handlerName = m_session.displayName;
        request.sourceFile = QFileInfo(path).fileName();
        request.warehouseId = warehouseId;
        request.locationId = locationId;

        InitialInventoryLine line;
        line.materialCode = row.materialCode;
        line.materialName = row.materialName;
        line.specification = row.specification;
        line.categoryCode = row.categoryCode;
        line.unit = row.unit;
        line.batchNo = row.inventoryBatch;
        line.quantity = row.currentStock;
        line.serialNumbers = row.inventorySerialNumbers;
        line.notes = QStringLiteral("物料档案第%1行现有库存").arg(row.sourceRow);
        request.lines.append(line);
    }

    int initialStock = 0;
    InventoryService inventory(m_database, m_session.userId);
    for (auto iterator = stockRequests.begin(); iterator != stockRequests.end(); ++iterator) {
        InitialInventoryRequest request = iterator.value();
        request.submissionToken = QStringLiteral("material-stock-import:%1")
                                      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QString stockError;
        if (!inventory.importInitialInventory(request, nullptr, &stockError)) {
            stockErrors.append(stockError);
            continue;
        }
        initialStock += request.lines.size();
    }

    refresh();
    emit dataChanged();
    const QString result = QStringLiteral(
        "新增 %1 条，更新 %2 条，现有库存入账 %3 条，跳过重复 %4 条、错误 %5 条。")
        .arg(created).arg(updated).arg(initialStock).arg(skippedCount).arg(errorCount);
    if (stockErrors.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("导入完成"), result);
    } else {
        QMessageBox::warning(this, QStringLiteral("物料已导入，部分库存未入账"),
                             result + QStringLiteral("\n\n库存入账问题：\n")
                                 + stockErrors.join(QLatin1Char('\n')));
    }
}

void MaterialPage::exportMaterials()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出物料档案"),
                                                       QStringLiteral("物料档案.xlsx"),
                                                       QStringLiteral("Excel工作簿 (*.xlsx)"));
    if (path.isEmpty()) return;
    QList<QList<QVariant>> rows;
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT m.code,m.name,m.specification,c.name,m.processing_method,m.unit,m.unit_usage,"
        "COALESCE((SELECT SUM(s.quantity) FROM stock_balances s WHERE s.material_id=m.id),0),"
        "m.minimum_stock,COALESCE(w.code,''),COALESCE(l.code,''),"
        "'' AS inventory_batch,'' AS inventory_serials,"
        "CASE m.require_batch WHEN 1 THEN '是' ELSE '否' END,"
        "CASE m.require_serial WHEN 1 THEN '是' ELSE '否' END,"
        "CASE m.is_active WHEN 1 THEN '正常' ELSE '停用' END,m.brand,m.notes "
        "FROM materials m LEFT JOIN material_categories c ON c.id=m.category_id "
        "LEFT JOIN warehouses w ON w.id=m.default_warehouse_id "
        "LEFT JOIN locations l ON l.id=m.default_location_id ORDER BY m.code"));
    while (query.next()) {
        QList<QVariant> row;
        for (int column = 0; column < 18; ++column) row.append(query.value(column));
        rows.append(row);
    }
    QString error;
    if (!XlsxExporter::writeSingleSheet(path, QStringLiteral("物料导入"),
        {QStringLiteral("物料编码"), QStringLiteral("物料名称"), QStringLiteral("规格"),
         QStringLiteral("物料类别"), QStringLiteral("加工方式"), QStringLiteral("单位"),
         QStringLiteral("单台用量"), QStringLiteral("现有库存"), QStringLiteral("最低库存"),
         QStringLiteral("默认仓库编码"), QStringLiteral("默认库位编码"),
         QStringLiteral("库存批次"), QStringLiteral("库存SN"),
         QStringLiteral("批次管理"), QStringLiteral("SN管理"), QStringLiteral("物料状态"),
         QStringLiteral("品牌"), QStringLiteral("备注")}, rows, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("导出完成"),
                             QStringLiteral("已导出 %1 条物料档案。").arg(rows.size()));
}
