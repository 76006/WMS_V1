#include "ui/pages/MaterialPage.h"

#include "import/LegacyInventoryImporter.h"
#include "import/XlsxExporter.h"
#include "ui/dialogs/MaterialDialog.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QTableView>
#include <QTableWidget>
#include <QTableWidgetItem>
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
    m_importButton = new QPushButton(QStringLiteral("导入Excel"), this);
    m_exportButton = new QPushButton(QStringLiteral("导出Excel"), this);
    const bool canEdit = m_session.canManageMaterials();
    m_addButton->setEnabled(canEdit);
    m_editButton->setEnabled(canEdit);
    m_importButton->setEnabled(canEdit);
    if (!canEdit) {
        m_addButton->setToolTip(QStringLiteral("当前角色没有物料维护权限"));
        m_editButton->setToolTip(m_addButton->toolTip());
        m_importButton->setToolTip(m_addButton->toolTip());
    }
    toolbar->addWidget(m_searchEdit, 1);
    toolbar->addWidget(m_categoryCombo);
    toolbar->addWidget(searchButton);
    toolbar->addSpacing(14);
    toolbar->addWidget(m_addButton);
    toolbar->addWidget(m_editButton);
    toolbar->addWidget(m_importButton);
    toolbar->addWidget(m_exportButton);
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
    connect(m_importButton, &QPushButton::clicked, this, &MaterialPage::importMaterials);
    connect(m_exportButton, &QPushButton::clicked, this, &MaterialPage::exportMaterials);
    connect(m_table, &QTableView::doubleClicked, this, [this] {
        if (m_session.canManageMaterials()) editMaterial();
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
    for (const MaterialImportRow &row : std::as_const(rows)) {
        if (row.status == MaterialImportStatus::Ready) ++readyCount;
        else if (row.status == MaterialImportStatus::Warning) ++warningCount;
        else ++errorCount;
    }

    QDialog preview(this);
    preview.setWindowTitle(QStringLiteral("物料Excel导入预览"));
    preview.resize(1180, 700);
    auto *layout = new QVBoxLayout(&preview);
    auto *summary = new QLabel(
        QStringLiteral("共 %1 行：可导入 %2 行，警告 %3 行，错误 %4 行。错误行不会导入。")
            .arg(rows.size()).arg(readyCount).arg(warningCount).arg(errorCount), &preview);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto *table = new QTableWidget(rows.size(), 12, &preview);
    table->setHorizontalHeaderLabels({QStringLiteral("状态"), QStringLiteral("Excel行"),
        QStringLiteral("物料编码"), QStringLiteral("物料名称"), QStringLiteral("规格"),
        QStringLiteral("分类"), QStringLiteral("品牌"), QStringLiteral("单位"),
        QStringLiteral("最低库存"), QStringLiteral("默认仓库/库位"),
        QStringLiteral("批次/SN"), QStringLiteral("说明")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->verticalHeader()->setVisible(false);
    for (int index = 0; index < rows.size(); ++index) {
        const MaterialImportRow &row = rows.at(index);
        const QString tracking = QStringLiteral("%1 / %2")
            .arg(row.requireBatch ? QStringLiteral("批次") : QStringLiteral("无批次"),
                 row.requireSerial ? QStringLiteral("SN") : QStringLiteral("无SN"));
        const QString location = row.defaultWarehouseCode.isEmpty()
            ? QStringLiteral("未设置")
            : row.defaultWarehouseCode + QStringLiteral(" / ") + row.defaultLocationCode;
        const QStringList values = {MaterialExcelImporter::statusText(row.status),
            QString::number(row.sourceRow), row.materialCode, row.materialName, row.specification,
            row.categoryCode, row.brand, row.unit, QString::number(row.minimumStock, 'g', 15),
            location, tracking, row.message};
        for (int column = 0; column < values.size(); ++column)
            table->setItem(index, column, new QTableWidgetItem(values.at(column)));
    }
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(11, QHeaderView::Stretch);
    layout->addWidget(table, 1);
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
    QMessageBox::information(this, QStringLiteral("导入完成"),
                             QStringLiteral("新增 %1 条，更新 %2 条，跳过错误 %3 条。")
                                 .arg(created).arg(updated).arg(errorCount));
    refresh();
    emit dataChanged();
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
        "SELECT m.code,m.name,m.specification,c.code,m.unit,m.minimum_stock,"
        "COALESCE(w.code,''),COALESCE(l.code,''),CASE m.require_batch WHEN 1 THEN '是' ELSE '否' END,"
        "CASE m.require_serial WHEN 1 THEN '是' ELSE '否' END,m.brand,m.notes "
        "FROM materials m LEFT JOIN material_categories c ON c.id=m.category_id "
        "LEFT JOIN warehouses w ON w.id=m.default_warehouse_id "
        "LEFT JOIN locations l ON l.id=m.default_location_id WHERE m.is_active=1 ORDER BY m.code"));
    while (query.next()) {
        QList<QVariant> row;
        for (int column = 0; column < 12; ++column) row.append(query.value(column));
        rows.append(row);
    }
    QString error;
    if (!XlsxExporter::writeSingleSheet(path, QStringLiteral("物料导入"),
        {QStringLiteral("物料编码"), QStringLiteral("物料名称"), QStringLiteral("规格"),
         QStringLiteral("分类编码"), QStringLiteral("单位"), QStringLiteral("最低库存"),
         QStringLiteral("默认仓库编码"), QStringLiteral("默认库位编码"),
         QStringLiteral("批次管理"), QStringLiteral("SN管理"), QStringLiteral("品牌"),
         QStringLiteral("备注")}, rows, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("导出完成"),
                             QStringLiteral("已导出 %1 条物料档案。").arg(rows.size()));
}
