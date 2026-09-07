#include "ui/dialogs/MaterialDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextEdit>
#include <QVBoxLayout>

#include <utility>

MaterialDialog::MaterialDialog(QSqlDatabase database,
                               qlonglong materialId,
                               QWidget *parent)
    : QDialog(parent), m_database(std::move(database)), m_materialId(materialId)
{
    setWindowTitle(materialId > 0 ? QStringLiteral("编辑物料") : QStringLiteral("新增物料"));
    setMinimumWidth(560);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setSpacing(10);

    m_codeEdit = new QLineEdit(this);
    m_codeEdit->setMaxLength(64);
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setMaxLength(128);
    m_specificationEdit = new QLineEdit(this);
    m_categoryCombo = new QComboBox(this);
    m_brandEdit = new QLineEdit(this);
    m_unitEdit = new QLineEdit(this);
    m_unitEdit->setMaxLength(20);
    m_minimumStockSpin = new QDoubleSpinBox(this);
    m_minimumStockSpin->setDecimals(6);
    m_minimumStockSpin->setRange(0, 999999999999.0);
    m_warehouseCombo = new QComboBox(this);
    m_locationCombo = new QComboBox(this);
    m_batchCheck = new QCheckBox(QStringLiteral("启用批次管理"), this);
    m_serialCheck = new QCheckBox(QStringLiteral("启用SN序列号管理"), this);
    m_notesEdit = new QTextEdit(this);
    m_notesEdit->setMaximumHeight(85);

    auto *trackingLayout = new QHBoxLayout;
    trackingLayout->addWidget(m_batchCheck);
    trackingLayout->addWidget(m_serialCheck);
    trackingLayout->addStretch();

    form->addRow(QStringLiteral("物料编码 *"), m_codeEdit);
    form->addRow(QStringLiteral("物料名称 *"), m_nameEdit);
    form->addRow(QStringLiteral("规格型号"), m_specificationEdit);
    form->addRow(QStringLiteral("物料分类 *"), m_categoryCombo);
    form->addRow(QStringLiteral("品牌"), m_brandEdit);
    form->addRow(QStringLiteral("单位 *"), m_unitEdit);
    form->addRow(QStringLiteral("最低库存"), m_minimumStockSpin);
    form->addRow(QStringLiteral("默认仓库"), m_warehouseCombo);
    form->addRow(QStringLiteral("默认库位"), m_locationCombo);
    form->addRow(QStringLiteral("追溯方式"), trackingLayout);
    form->addRow(QStringLiteral("备注"), m_notesEdit);
    root->addLayout(form);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setStyleSheet(QStringLiteral("color:#b91c1c;"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->hide();
    root->addWidget(m_errorLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    root->addWidget(buttons);

    connect(m_warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MaterialDialog::loadLocations);
    connect(buttons, &QDialogButtonBox::accepted, this, &MaterialDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    loadReferenceData();
    if (m_materialId > 0) {
        loadMaterial();
    }
}

void MaterialDialog::loadReferenceData()
{
    m_categoryCombo->clear();
    m_categoryCombo->addItem(QStringLiteral("请选择"), QVariant());
    QSqlQuery categories(m_database);
    categories.exec(QStringLiteral(
        "SELECT id, name FROM material_categories WHERE is_active=1 ORDER BY sort_order, name"));
    while (categories.next()) {
        m_categoryCombo->addItem(categories.value(1).toString(), categories.value(0));
    }

    m_warehouseCombo->clear();
    m_warehouseCombo->addItem(QStringLiteral("未设置"), QVariant());
    QSqlQuery warehouses(m_database);
    warehouses.exec(QStringLiteral("SELECT id, code, name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (warehouses.next()) {
        m_warehouseCombo->addItem(
            QStringLiteral("%1 - %2").arg(warehouses.value(1).toString(), warehouses.value(2).toString()),
            warehouses.value(0));
    }
    loadLocations();
}

void MaterialDialog::loadLocations()
{
    const qlonglong warehouseId = m_warehouseCombo->currentData().toLongLong();
    m_locationCombo->clear();
    m_locationCombo->addItem(QStringLiteral("未设置"), QVariant());
    if (warehouseId <= 0) {
        return;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, code, name FROM locations WHERE warehouse_id=? AND is_active=1 ORDER BY code"));
    query.addBindValue(warehouseId);
    query.exec();
    while (query.next()) {
        QString label = query.value(1).toString();
        if (!query.value(2).toString().isEmpty()) {
            label += QStringLiteral(" - ") + query.value(2).toString();
        }
        m_locationCombo->addItem(label, query.value(0));
    }
    if (m_pendingLocationId > 0) {
        const int index = m_locationCombo->findData(m_pendingLocationId);
        if (index >= 0) {
            m_locationCombo->setCurrentIndex(index);
        }
        m_pendingLocationId = 0;
    }
}

void MaterialDialog::loadMaterial()
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT code, name, specification, category_id, brand, unit, minimum_stock, "
        "default_warehouse_id, default_location_id, require_batch, require_serial, notes "
        "FROM materials WHERE id=?"));
    query.addBindValue(m_materialId);
    if (!query.exec() || !query.next()) {
        showError(QStringLiteral("无法读取物料资料：%1").arg(query.lastError().text()));
        return;
    }
    m_codeEdit->setText(query.value(0).toString());
    m_nameEdit->setText(query.value(1).toString());
    m_specificationEdit->setText(query.value(2).toString());
    m_categoryCombo->setCurrentIndex(m_categoryCombo->findData(query.value(3)));
    m_brandEdit->setText(query.value(4).toString());
    m_unitEdit->setText(query.value(5).toString());
    m_minimumStockSpin->setValue(query.value(6).toDouble());
    m_pendingLocationId = query.value(8).toLongLong();
    const int warehouseIndex = m_warehouseCombo->findData(query.value(7));
    m_warehouseCombo->setCurrentIndex(warehouseIndex >= 0 ? warehouseIndex : 0);
    loadLocations();
    m_batchCheck->setChecked(query.value(9).toBool());
    m_serialCheck->setChecked(query.value(10).toBool());
    m_notesEdit->setPlainText(query.value(11).toString());
}

void MaterialDialog::save()
{
    const QString code = m_codeEdit->text().trimmed().toUpper();
    const QString name = m_nameEdit->text().trimmed();
    const QString unit = m_unitEdit->text().trimmed();
    if (code.isEmpty() || name.isEmpty() || unit.isEmpty() || m_categoryCombo->currentData().isNull()) {
        showError(QStringLiteral("请完整填写物料编码、名称、分类和单位。"));
        return;
    }
    const qlonglong warehouseId = m_warehouseCombo->currentData().toLongLong();
    const qlonglong locationId = m_locationCombo->currentData().toLongLong();
    if ((warehouseId <= 0) != (locationId <= 0)) {
        showError(QStringLiteral("默认仓库和默认库位必须同时设置，或同时留空。"));
        return;
    }

    QSqlQuery query(m_database);
    if (m_materialId > 0) {
        query.prepare(QStringLiteral(
            "UPDATE materials SET code=?, name=?, specification=?, category_id=?, brand=?, unit=?, "
            "minimum_stock=?, default_warehouse_id=?, default_location_id=?, require_batch=?, "
            "require_serial=?, notes=?, updated_at=? WHERE id=?"));
    } else {
        query.prepare(QStringLiteral(
            "INSERT INTO materials(code, name, specification, category_id, brand, unit, minimum_stock, "
            "default_warehouse_id, default_location_id, require_batch, require_serial, notes) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    }
    query.addBindValue(code);
    query.addBindValue(name);
    query.addBindValue(m_specificationEdit->text().trimmed());
    query.addBindValue(m_categoryCombo->currentData());
    query.addBindValue(m_brandEdit->text().trimmed());
    query.addBindValue(unit);
    query.addBindValue(m_minimumStockSpin->value());
    query.addBindValue(warehouseId > 0 ? QVariant(warehouseId) : QVariant());
    query.addBindValue(locationId > 0 ? QVariant(locationId) : QVariant());
    query.addBindValue(m_batchCheck->isChecked());
    query.addBindValue(m_serialCheck->isChecked());
    query.addBindValue(m_notesEdit->toPlainText().trimmed());
    if (m_materialId > 0) {
        query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        query.addBindValue(m_materialId);
    }
    if (!query.exec()) {
        showError(QStringLiteral("保存失败。请检查物料编码是否重复。\n%1").arg(query.lastError().text()));
        return;
    }
    accept();
}

void MaterialDialog::showError(const QString &message)
{
    m_errorLabel->setText(message);
    m_errorLabel->show();
}
