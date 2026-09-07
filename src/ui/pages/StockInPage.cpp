#include "ui/pages/StockInPage.h"

#include "services/InventoryService.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTextEdit>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace {
constexpr int RequireBatchRole = Qt::UserRole + 1;
constexpr int RequireSerialRole = Qt::UserRole + 2;
constexpr int DefaultWarehouseRole = Qt::UserRole + 3;
constexpr int DefaultLocationRole = Qt::UserRole + 4;
constexpr int MaterialCodeRole = Qt::UserRole + 5;
}

StockInPage::StockInPage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    panel->setMaximumWidth(900);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(22, 20, 22, 22);
    panelLayout->setSpacing(12);

    auto *heading = new QLabel(QStringLiteral("新建入库记录"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    panelLayout->addWidget(heading);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(11);
    m_typeCombo = new QComboBox(panel);
    m_typeCombo->addItem(QStringLiteral("采购入库"), QStringLiteral("CGRK"));
    m_typeCombo->addItem(QStringLiteral("生产完工入库"), QStringLiteral("SCWG"));
    m_typeCombo->addItem(QStringLiteral("退料入库"), QStringLiteral("TLRK"));
    m_typeCombo->addItem(QStringLiteral("其他入库"), QStringLiteral("QTRK"));
    m_typeCombo->addItem(QStringLiteral("期初入库"), QStringLiteral("QC"));
    m_dateEdit = new QDateEdit(QDate::currentDate(), panel);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_numberLabel = new QLabel(QStringLiteral("提交时按规则自动生成"), panel);
    m_numberLabel->setObjectName(QStringLiteral("mutedText"));
    m_materialCombo = new QComboBox(panel);
    m_materialCombo->setEditable(true);
    m_materialCombo->setInsertPolicy(QComboBox::NoInsert);
    m_materialDetail = new QLabel(panel);
    m_materialDetail->setObjectName(QStringLiteral("mutedText"));
    m_quantitySpin = new QDoubleSpinBox(panel);
    m_quantitySpin->setDecimals(6);
    m_quantitySpin->setRange(0.000001, 999999999999.0);
    m_quantitySpin->setValue(1.0);
    m_batchEdit = new QLineEdit(panel);
    m_batchEdit->setPlaceholderText(QStringLiteral("未启用批次管理时可留空"));
    m_warehouseCombo = new QComboBox(panel);
    m_locationCombo = new QComboBox(panel);
    m_availableLabel = new QLabel(QStringLiteral("当前库位库存：0"), panel);
    m_availableLabel->setObjectName(QStringLiteral("mutedText"));
    m_handlerEdit = new QLineEdit(m_session.displayName, panel);
    m_serialEdit = new QTextEdit(panel);
    m_serialEdit->setPlaceholderText(QStringLiteral("每行一个SN"));
    m_serialEdit->setMaximumHeight(120);
    m_generateSerialButton = new QPushButton(QStringLiteral("按数量批量生成SN"), panel);
    m_notesEdit = new QTextEdit(panel);
    m_notesEdit->setMaximumHeight(85);

    auto *materialBlock = new QWidget(panel);
    auto *materialLayout = new QVBoxLayout(materialBlock);
    materialLayout->setContentsMargins(0, 0, 0, 0);
    materialLayout->setSpacing(4);
    materialLayout->addWidget(m_materialCombo);
    materialLayout->addWidget(m_materialDetail);

    auto *locationBlock = new QWidget(panel);
    auto *locationLayout = new QVBoxLayout(locationBlock);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    locationLayout->setSpacing(4);
    locationLayout->addWidget(m_locationCombo);
    locationLayout->addWidget(m_availableLabel);

    auto *serialBlock = new QWidget(panel);
    auto *serialLayout = new QVBoxLayout(serialBlock);
    serialLayout->setContentsMargins(0, 0, 0, 0);
    serialLayout->setSpacing(6);
    serialLayout->addWidget(m_serialEdit);
    serialLayout->addWidget(m_generateSerialButton, 0, Qt::AlignLeft);

    form->addRow(QStringLiteral("入库类型 *"), m_typeCombo);
    form->addRow(QStringLiteral("入库日期 *"), m_dateEdit);
    form->addRow(QStringLiteral("入库单号"), m_numberLabel);
    form->addRow(QStringLiteral("物料 *"), materialBlock);
    form->addRow(QStringLiteral("数量 *"), m_quantitySpin);
    form->addRow(QStringLiteral("批次号"), m_batchEdit);
    form->addRow(QStringLiteral("仓库 *"), m_warehouseCombo);
    form->addRow(QStringLiteral("库位 *"), locationBlock);
    form->addRow(QStringLiteral("经办人"), m_handlerEdit);
    form->addRow(QStringLiteral("SN列表"), serialBlock);
    form->addRow(QStringLiteral("备注"), m_notesEdit);
    panelLayout->addLayout(form);

    auto *actions = new QHBoxLayout;
    actions->addStretch();
    m_submitButton = new QPushButton(QStringLiteral("确认并入库"), panel);
    m_submitButton->setProperty("primary", true);
    m_submitButton->setEnabled(m_session.canManageWarehouse());
    actions->addWidget(m_submitButton);
    panelLayout->addLayout(actions);
    root->addWidget(panel, 0, Qt::AlignHCenter | Qt::AlignTop);
    root->addStretch();

    connect(m_materialCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &StockInPage::onMaterialChanged);
    connect(m_warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &StockInPage::loadLocations);
    connect(m_locationCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &StockInPage::updateAvailableStock);
    connect(m_batchEdit, &QLineEdit::textChanged, this, &StockInPage::updateAvailableStock);
    connect(m_generateSerialButton, &QPushButton::clicked, this, &StockInPage::generateSerialNumbers);
    connect(m_submitButton, &QPushButton::clicked, this, &StockInPage::submit);
    refreshReferenceData();
}

void StockInPage::refreshReferenceData()
{
    const QVariant currentMaterial = m_materialCombo->currentData();
    const QVariant currentWarehouse = m_warehouseCombo->currentData();
    const QVariant currentLocation = m_locationCombo->currentData();

    m_materialCombo->blockSignals(true);
    m_materialCombo->clear();
    QSqlQuery materials(m_database);
    materials.exec(QStringLiteral(
        "SELECT id, code, name, specification, unit, require_batch, require_serial, "
        "default_warehouse_id, default_location_id FROM materials WHERE is_active=1 ORDER BY code"));
    while (materials.next()) {
        const int index = m_materialCombo->count();
        m_materialCombo->addItem(QStringLiteral("%1 - %2")
                                     .arg(materials.value(1).toString(), materials.value(2).toString()),
                                 materials.value(0));
        m_materialCombo->setItemData(index, materials.value(5), RequireBatchRole);
        m_materialCombo->setItemData(index, materials.value(6), RequireSerialRole);
        m_materialCombo->setItemData(index, materials.value(7), DefaultWarehouseRole);
        m_materialCombo->setItemData(index, materials.value(8), DefaultLocationRole);
        m_materialCombo->setItemData(index, materials.value(1), MaterialCodeRole);
        m_materialCombo->setItemData(index,
            QStringLiteral("规格：%1    单位：%2")
                .arg(materials.value(3).toString().isEmpty() ? QStringLiteral("未填写") : materials.value(3).toString(),
                     materials.value(4).toString()),
            Qt::ToolTipRole);
    }
    selectComboData(m_materialCombo, currentMaterial);
    m_materialCombo->blockSignals(false);

    m_warehouseCombo->blockSignals(true);
    m_warehouseCombo->clear();
    QSqlQuery warehouses(m_database);
    warehouses.exec(QStringLiteral("SELECT id, code, name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (warehouses.next()) {
        m_warehouseCombo->addItem(QStringLiteral("%1 - %2")
                                      .arg(warehouses.value(1).toString(), warehouses.value(2).toString()),
                                  warehouses.value(0));
    }
    selectComboData(m_warehouseCombo, currentWarehouse);
    m_pendingLocationId = currentLocation.toLongLong();
    m_warehouseCombo->blockSignals(false);
    loadLocations();
    onMaterialChanged();
}

void StockInPage::selectComboData(QComboBox *combo, const QVariant &value)
{
    const int index = combo->findData(value);
    if (index >= 0) combo->setCurrentIndex(index);
    else if (combo->count() > 0) combo->setCurrentIndex(0);
}

void StockInPage::onMaterialChanged()
{
    const int index = m_materialCombo->currentIndex();
    if (index < 0) {
        m_materialDetail->setText(QStringLiteral("请先在物料管理中新增物料。"));
        m_submitButton->setEnabled(false);
        return;
    }
    const bool requireBatch = m_materialCombo->itemData(index, RequireBatchRole).toBool();
    const bool requireSerial = m_materialCombo->itemData(index, RequireSerialRole).toBool();
    m_batchEdit->setPlaceholderText(requireBatch ? QStringLiteral("必填") : QStringLiteral("可留空"));
    m_serialEdit->setEnabled(requireSerial);
    m_generateSerialButton->setEnabled(requireSerial);
    if (!requireSerial) m_serialEdit->clear();
    m_materialDetail->setText(m_materialCombo->itemData(index, Qt::ToolTipRole).toString()
                              + (requireBatch ? QStringLiteral("    批次管理：是") : QString())
                              + (requireSerial ? QStringLiteral("    SN管理：是") : QString()));

    const qlonglong defaultWarehouse = m_materialCombo->itemData(index, DefaultWarehouseRole).toLongLong();
    const qlonglong defaultLocation = m_materialCombo->itemData(index, DefaultLocationRole).toLongLong();
    if (defaultWarehouse > 0) {
        m_pendingLocationId = defaultLocation;
        selectComboData(m_warehouseCombo, defaultWarehouse);
        loadLocations();
    }
    m_submitButton->setEnabled(m_session.canManageWarehouse()
                               && m_materialCombo->count() > 0
                               && m_warehouseCombo->count() > 0
                               && m_locationCombo->count() > 0);
    updateAvailableStock();
}

void StockInPage::loadLocations()
{
    const qlonglong warehouseId = m_warehouseCombo->currentData().toLongLong();
    m_locationCombo->blockSignals(true);
    m_locationCombo->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, code, name FROM locations WHERE warehouse_id=? AND is_active=1 ORDER BY code"));
    query.addBindValue(warehouseId);
    query.exec();
    while (query.next()) {
        QString label = query.value(1).toString();
        if (!query.value(2).toString().isEmpty()) label += QStringLiteral(" - ") + query.value(2).toString();
        m_locationCombo->addItem(label, query.value(0));
    }
    if (m_pendingLocationId > 0) {
        selectComboData(m_locationCombo, m_pendingLocationId);
        m_pendingLocationId = 0;
    }
    m_locationCombo->blockSignals(false);
    m_submitButton->setEnabled(m_session.canManageWarehouse()
                               && m_materialCombo->count() > 0
                               && m_locationCombo->count() > 0);
    updateAvailableStock();
}

void StockInPage::updateAvailableStock()
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT COALESCE(quantity,0) FROM stock_balances WHERE material_id=? AND warehouse_id=? "
        "AND location_id=? AND batch_no=?"));
    query.addBindValue(m_materialCombo->currentData());
    query.addBindValue(m_warehouseCombo->currentData());
    query.addBindValue(m_locationCombo->currentData());
    query.addBindValue(m_batchEdit->text().trimmed());
    QString quantity = QStringLiteral("0");
    if (query.exec() && query.next()) {
        quantity = QString::number(query.value(0).toDouble(), 'f', 6)
                       .remove(QRegularExpression(QStringLiteral("\\.?0+$")));
    }
    m_availableLabel->setText(QStringLiteral("当前库位/批次库存：%1").arg(quantity));
}

QStringList StockInPage::enteredSerialNumbers() const
{
    QString text = m_serialEdit->toPlainText();
    QStringList values = text.split(QRegularExpression(QStringLiteral("[,;\\r\\n]+")), Qt::SkipEmptyParts);
    for (QString &value : values) value = value.trimmed().toUpper();
    return values;
}

void StockInPage::generateSerialNumbers()
{
    if (m_materialCombo->currentIndex() < 0) return;
    const double quantity = m_quantitySpin->value();
    if (std::abs(quantity - std::round(quantity)) > 0.0000001) {
        QMessageBox::warning(this, QStringLiteral("数量不正确"), QStringLiteral("SN管理物料的入库数量必须是整数。"));
        return;
    }
    QString defaultPrefix = m_materialCombo->currentData(MaterialCodeRole).toString();
    bool ok = false;
    const QString prefix = QInputDialog::getText(this, QStringLiteral("批量生成SN"),
                                                 QStringLiteral("SN前缀"), QLineEdit::Normal,
                                                 defaultPrefix, &ok);
    if (!ok) return;
    InventoryService service(m_database, m_session.userId);
    QString error;
    const QStringList serials = service.previewSerialNumbers(
        m_materialCombo->currentData().toLongLong(), prefix,
        static_cast<int>(std::round(quantity)), &error);
    if (serials.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法生成SN"), error);
        return;
    }
    m_serialEdit->setPlainText(serials.join(QLatin1Char('\n')));
}

void StockInPage::submit()
{
    if (m_materialCombo->currentIndex() < 0 || m_locationCombo->currentIndex() < 0) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"), QStringLiteral("请选择物料、仓库和库位。"));
        return;
    }
    const auto confirm = QMessageBox::question(
        this, QStringLiteral("确认入库"),
        QStringLiteral("确认提交本次入库？提交后库存将立即增加，并自动生成库存流水。"));
    if (confirm != QMessageBox::Yes) return;

    StockMovementRequest request;
    request.documentType = m_typeCombo->currentData().toString();
    request.documentDate = m_dateEdit->date();
    request.handlerName = m_handlerEdit->text().trimmed();
    request.notes = m_notesEdit->toPlainText().trimmed();
    request.materialId = m_materialCombo->currentData().toLongLong();
    request.quantity = m_quantitySpin->value();
    request.batchNo = m_batchEdit->text().trimmed();
    request.warehouseId = m_warehouseCombo->currentData().toLongLong();
    request.locationId = m_locationCombo->currentData().toLongLong();
    request.serialNumbers = enteredSerialNumbers();

    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    QString error;
    if (!service.postInbound(request, &posted, &error)) {
        QMessageBox::warning(this, QStringLiteral("入库失败"), error);
        return;
    }
    m_numberLabel->setText(posted.documentNumber);
    QMessageBox::information(this, QStringLiteral("入库完成"),
                             QStringLiteral("入库成功，单据号：%1").arg(posted.documentNumber));
    m_quantitySpin->setValue(1.0);
    m_batchEdit->clear();
    m_serialEdit->clear();
    m_notesEdit->clear();
    updateAvailableStock();
    emit stockChanged();
}
