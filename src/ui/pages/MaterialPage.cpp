#include "ui/pages/MaterialPage.h"

#include "import/LegacyInventoryImporter.h"
#include "import/XlsxExporter.h"
#include "services/InventoryService.h"
#include "services/MaterialCodeService.h"
#include "ui/dialogs/MaterialDialog.h"

#include <QComboBox>
#include <QCheckBox>
#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QMap>
#include <QPushButton>
#include <QScreen>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QTableView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUuid>
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
    m_statusCombo = new QComboBox(this);
    m_statusCombo->setMinimumWidth(110);
    m_statusCombo->addItem(QStringLiteral("全部状态"), QVariant());
    m_statusCombo->addItem(QStringLiteral("正常"), true);
    m_statusCombo->addItem(QStringLiteral("停用"), false);
    auto *searchButton = new QPushButton(QStringLiteral("查询"), this);
    m_addButton = new QPushButton(QStringLiteral("新增物料"), this);
    m_addButton->setProperty("primary", true);
    m_editButton = new QPushButton(QStringLiteral("编辑"), this);
    m_batchEditButton = new QPushButton(QStringLiteral("批量编辑"), this);
    m_importButton = new QPushButton(QStringLiteral("导入Excel"), this);
    m_exportButton = new QPushButton(QStringLiteral("导出Excel"), this);
    m_projectButton = new QPushButton(QStringLiteral("项目代码"), this);
    const bool canEdit = m_session.canManageMaterials();
    m_addButton->setEnabled(canEdit);
    m_editButton->setEnabled(canEdit);
    m_batchEditButton->setEnabled(canEdit);
    m_importButton->setEnabled(canEdit);
    m_projectButton->setEnabled(canEdit);
    if (!canEdit) {
        m_addButton->setToolTip(QStringLiteral("当前角色没有物料维护权限"));
        m_editButton->setToolTip(m_addButton->toolTip());
        m_batchEditButton->setToolTip(m_addButton->toolTip());
        m_importButton->setToolTip(m_addButton->toolTip());
        m_projectButton->setToolTip(m_addButton->toolTip());
    }
    toolbar->addWidget(m_searchEdit, 1);
    toolbar->addWidget(m_categoryCombo);
    toolbar->addWidget(m_statusCombo);
    toolbar->addWidget(searchButton);
    toolbar->addSpacing(14);
    toolbar->addWidget(m_addButton);
    toolbar->addWidget(m_editButton);
    toolbar->addWidget(m_batchEditButton);
    toolbar->addWidget(m_importButton);
    toolbar->addWidget(m_exportButton);
    toolbar->addWidget(m_projectButton);
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
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    panelLayout->addWidget(m_table);
    root->addWidget(panel, 1);

    connect(searchButton, &QPushButton::clicked, this, &MaterialPage::refresh);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MaterialPage::refresh);
    connect(m_categoryCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MaterialPage::refresh);
    connect(m_statusCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MaterialPage::refresh);
    connect(m_addButton, &QPushButton::clicked, this, &MaterialPage::addMaterial);
    connect(m_editButton, &QPushButton::clicked, this, &MaterialPage::editMaterial);
    connect(m_batchEditButton, &QPushButton::clicked, this, &MaterialPage::batchEditMaterials);
    connect(m_importButton, &QPushButton::clicked, this, &MaterialPage::importMaterials);
    connect(m_exportButton, &QPushButton::clicked, this, &MaterialPage::exportMaterials);
    connect(m_projectButton, &QPushButton::clicked, this, &MaterialPage::manageProjects);
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

void MaterialPage::batchEditMaterials()
{
    const QModelIndexList selectedRows = m_table->selectionModel()->selectedRows(0);
    if (selectedRows.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("请选择物料"),
                                 QStringLiteral("请按住 Ctrl 或 Shift 选择需要批量修改的物料。"));
        return;
    }

    QList<qlonglong> materialIds;
    for (const QModelIndex &index : selectedRows) {
        const qlonglong id = index.data().toLongLong();
        if (id > 0) materialIds.append(id);
    }
    if (materialIds.isEmpty()) return;

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
    processingCombo->addItem(QStringLiteral("未设置"), QString());
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
        values.append(processingCombo->currentText() == QStringLiteral("未设置")
                          ? QString() : processingCombo->currentText().trimmed());
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
    dialog.exec();
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
