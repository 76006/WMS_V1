#include "ui/pages/StockOutPage.h"

#include "services/InventoryService.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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
}

StockOutPage::StockOutPage(QSqlDatabase database, Session session, QWidget *parent)
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
    auto *heading = new QLabel(QStringLiteral("新建出库记录"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    panelLayout->addWidget(heading);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(11);
    m_typeCombo = new QComboBox(panel);
    m_typeCombo->addItem(QStringLiteral("生产领料"), QStringLiteral("SCLL"));
    m_typeCombo->addItem(QStringLiteral("销售出库"), QStringLiteral("XSCK"));
    m_typeCombo->addItem(QStringLiteral("维修领用"), QStringLiteral("WXLY"));
    m_typeCombo->addItem(QStringLiteral("样品领用"), QStringLiteral("YPLY"));
    m_typeCombo->addItem(QStringLiteral("其他出库"), QStringLiteral("QTCK"));
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
    m_warehouseCombo = new QComboBox(panel);
    m_locationCombo = new QComboBox(panel);
    m_batchCombo = new QComboBox(panel);
    m_availableLabel = new QLabel(QStringLiteral("当前可用库存：0"), panel);
    m_availableLabel->setObjectName(QStringLiteral("mutedText"));
    m_receiverEdit = new QLineEdit(panel);
    m_receiverEdit->setPlaceholderText(QStringLiteral("领用人/收货人"));
    m_purposeEdit = new QLineEdit(panel);
    m_purposeEdit->setPlaceholderText(QStringLiteral("用途、生产批次或关联说明"));
    m_serialList = new QListWidget(panel);
    m_serialList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_serialList->setMaximumHeight(130);
    m_serialHint = new QLabel(QStringLiteral("按 Ctrl 或 Shift 可多选"), panel);
    m_serialHint->setObjectName(QStringLiteral("mutedText"));
    m_notesEdit = new QTextEdit(panel);
    m_notesEdit->setMaximumHeight(75);

    auto *materialBlock = new QWidget(panel);
    auto *materialLayout = new QVBoxLayout(materialBlock);
    materialLayout->setContentsMargins(0, 0, 0, 0);
    materialLayout->setSpacing(4);
    materialLayout->addWidget(m_materialCombo);
    materialLayout->addWidget(m_materialDetail);

    auto *batchBlock = new QWidget(panel);
    auto *batchLayout = new QVBoxLayout(batchBlock);
    batchLayout->setContentsMargins(0, 0, 0, 0);
    batchLayout->setSpacing(4);
    batchLayout->addWidget(m_batchCombo);
    batchLayout->addWidget(m_availableLabel);

    auto *serialBlock = new QWidget(panel);
    auto *serialLayout = new QVBoxLayout(serialBlock);
    serialLayout->setContentsMargins(0, 0, 0, 0);
    serialLayout->setSpacing(4);
    serialLayout->addWidget(m_serialList);
    serialLayout->addWidget(m_serialHint);

    form->addRow(QStringLiteral("出库类型 *"), m_typeCombo);
    form->addRow(QStringLiteral("出库日期 *"), m_dateEdit);
    form->addRow(QStringLiteral("出库单号"), m_numberLabel);
    form->addRow(QStringLiteral("物料 *"), materialBlock);
    form->addRow(QStringLiteral("数量 *"), m_quantitySpin);
    form->addRow(QStringLiteral("仓库 *"), m_warehouseCombo);
    form->addRow(QStringLiteral("库位 *"), m_locationCombo);
    form->addRow(QStringLiteral("批次 *"), batchBlock);
    form->addRow(QStringLiteral("领用人"), m_receiverEdit);
    form->addRow(QStringLiteral("用途"), m_purposeEdit);
    form->addRow(QStringLiteral("SN选择"), serialBlock);
    form->addRow(QStringLiteral("备注"), m_notesEdit);
    panelLayout->addLayout(form);

    auto *actions = new QHBoxLayout;
    actions->addStretch();
    m_submitButton = new QPushButton(QStringLiteral("确认并出库"), panel);
    m_submitButton->setProperty("primary", true);
    actions->addWidget(m_submitButton);
    panelLayout->addLayout(actions);
    root->addWidget(panel, 0, Qt::AlignHCenter | Qt::AlignTop);
    root->addStretch();

    connect(m_materialCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &StockOutPage::onMaterialChanged);
    connect(m_warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &StockOutPage::loadLocations);
    connect(m_locationCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &StockOutPage::loadBatches);
    connect(m_batchCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &StockOutPage::loadSerialNumbers);
    connect(m_serialList, &QListWidget::itemSelectionChanged, this, [this] {
        m_serialHint->setText(QStringLiteral("已选择 %1 个SN；按 Ctrl 或 Shift 可多选")
                                  .arg(m_serialList->selectedItems().size()));
    });
    connect(m_submitButton, &QPushButton::clicked, this, &StockOutPage::submit);
    refreshReferenceData();
}

void StockOutPage::selectComboData(QComboBox *combo, const QVariant &value)
{
    const int index = combo->findData(value);
    if (index >= 0) combo->setCurrentIndex(index);
    else if (combo->count() > 0) combo->setCurrentIndex(0);
}

void StockOutPage::refreshReferenceData()
{
    const QVariant previousMaterial = m_materialCombo->currentData();
    const QVariant previousWarehouse = m_warehouseCombo->currentData();
    const QVariant previousLocation = m_locationCombo->currentData();
    m_materialCombo->blockSignals(true);
    m_materialCombo->clear();
    QSqlQuery materials(m_database);
    materials.exec(QStringLiteral(
        "SELECT m.id,m.code,m.name,m.specification,m.unit,m.require_batch,m.require_serial,"
        "m.default_warehouse_id,m.default_location_id "
        "FROM materials m WHERE m.is_active=1 AND EXISTS("
        " SELECT 1 FROM stock_balances s WHERE s.material_id=m.id AND s.quantity>0) ORDER BY m.code"));
    while (materials.next()) {
        const int index = m_materialCombo->count();
        m_materialCombo->addItem(QStringLiteral("%1 - %2")
                                     .arg(materials.value(1).toString(), materials.value(2).toString()),
                                 materials.value(0));
        m_materialCombo->setItemData(index, materials.value(5), RequireBatchRole);
        m_materialCombo->setItemData(index, materials.value(6), RequireSerialRole);
        m_materialCombo->setItemData(index, materials.value(7), DefaultWarehouseRole);
        m_materialCombo->setItemData(index, materials.value(8), DefaultLocationRole);
        m_materialCombo->setItemData(index,
            QStringLiteral("规格：%1    单位：%2")
                .arg(materials.value(3).toString().isEmpty() ? QStringLiteral("未填写") : materials.value(3).toString(),
                     materials.value(4).toString()), Qt::ToolTipRole);
    }
    selectComboData(m_materialCombo, previousMaterial);
    m_materialCombo->blockSignals(false);

    m_warehouseCombo->blockSignals(true);
    m_warehouseCombo->clear();
    QSqlQuery warehouses(m_database);
    warehouses.exec(QStringLiteral(
        "SELECT DISTINCT w.id,w.code,w.name FROM warehouses w "
        "JOIN stock_balances s ON s.warehouse_id=w.id "
        "WHERE w.is_active=1 AND s.quantity>0 ORDER BY w.code"));
    while (warehouses.next()) {
        m_warehouseCombo->addItem(QStringLiteral("%1 - %2")
                                      .arg(warehouses.value(1).toString(), warehouses.value(2).toString()),
                                  warehouses.value(0));
    }
    selectComboData(m_warehouseCombo, previousWarehouse);
    m_pendingLocationId = previousLocation.toLongLong();
    m_warehouseCombo->blockSignals(false);
    loadLocations();
    onMaterialChanged();
}

void StockOutPage::onMaterialChanged()
{
    const int index = m_materialCombo->currentIndex();
    if (index < 0) {
        m_materialDetail->setText(QStringLiteral("当前没有可出库物料，请先办理入库。"));
        m_submitButton->setEnabled(false);
        m_serialList->clear();
        return;
    }
    const bool requireBatch = m_materialCombo->itemData(index, RequireBatchRole).toBool();
    const bool requireSerial = m_materialCombo->itemData(index, RequireSerialRole).toBool();
    m_materialDetail->setText(m_materialCombo->itemData(index, Qt::ToolTipRole).toString()
                              + (requireBatch ? QStringLiteral("    批次管理：是") : QStringLiteral("    批次管理：否"))
                              + (requireSerial ? QStringLiteral("    SN管理：是") : QString()));
    m_serialList->setVisible(requireSerial);
    m_serialHint->setVisible(requireSerial);
    const qlonglong warehouseId = m_materialCombo->itemData(index, DefaultWarehouseRole).toLongLong();
    const qlonglong locationId = m_materialCombo->itemData(index, DefaultLocationRole).toLongLong();
    if (warehouseId > 0 && m_warehouseCombo->findData(warehouseId) >= 0) {
        m_pendingLocationId = locationId;
        selectComboData(m_warehouseCombo, warehouseId);
    }
    loadLocations();
}

void StockOutPage::loadLocations()
{
    const QVariant previous = m_pendingLocationId > 0 ? QVariant(m_pendingLocationId) : m_locationCombo->currentData();
    m_pendingLocationId = 0;
    m_locationCombo->blockSignals(true);
    m_locationCombo->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT DISTINCT l.id,l.code,l.name FROM locations l "
        "JOIN stock_balances s ON s.location_id=l.id "
        "WHERE l.warehouse_id=? AND s.material_id=? AND s.quantity>0 AND l.is_active=1 ORDER BY l.code"));
    query.addBindValue(m_warehouseCombo->currentData());
    query.addBindValue(m_materialCombo->currentData());
    query.exec();
    while (query.next()) {
        QString label = query.value(1).toString();
        if (!query.value(2).toString().isEmpty()) label += QStringLiteral(" - ") + query.value(2).toString();
        m_locationCombo->addItem(label, query.value(0));
    }
    selectComboData(m_locationCombo, previous);
    m_locationCombo->blockSignals(false);
    loadBatches();
}

void StockOutPage::loadBatches()
{
    const QVariant previous = m_batchCombo->currentData();
    m_batchCombo->blockSignals(true);
    m_batchCombo->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT batch_no,quantity FROM stock_balances WHERE material_id=? AND warehouse_id=? "
        "AND location_id=? AND quantity>0 ORDER BY batch_no"));
    query.addBindValue(m_materialCombo->currentData());
    query.addBindValue(m_warehouseCombo->currentData());
    query.addBindValue(m_locationCombo->currentData());
    query.exec();
    while (query.next()) {
        const QString batch = query.value(0).toString();
        const QString label = batch.isEmpty()
            ? QStringLiteral("无批次（可用 %1）").arg(query.value(1).toString())
            : QStringLiteral("%1（可用 %2）").arg(batch, query.value(1).toString());
        m_batchCombo->addItem(label, batch);
    }
    selectComboData(m_batchCombo, previous);
    m_batchCombo->blockSignals(false);
    loadSerialNumbers();
}

void StockOutPage::loadSerialNumbers()
{
    m_serialList->clear();
    const int materialIndex = m_materialCombo->currentIndex();
    const bool requireSerial = materialIndex >= 0
                               && m_materialCombo->itemData(materialIndex, RequireSerialRole).toBool();
    if (requireSerial) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "SELECT serial_no FROM serial_numbers WHERE material_id=? AND warehouse_id=? AND location_id=? "
            "AND batch_no=? AND status='IN_STOCK' ORDER BY serial_no"));
        query.addBindValue(m_materialCombo->currentData());
        query.addBindValue(m_warehouseCombo->currentData());
        query.addBindValue(m_locationCombo->currentData());
        query.addBindValue(m_batchCombo->currentData().toString());
        query.exec();
        while (query.next()) m_serialList->addItem(query.value(0).toString());
    }
    m_serialHint->setText(QStringLiteral("可用 %1 个，已选择 0 个；按 Ctrl 或 Shift 可多选")
                              .arg(m_serialList->count()));
    updateAvailableStock();
}

void StockOutPage::updateAvailableStock()
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT quantity FROM stock_balances WHERE material_id=? AND warehouse_id=? AND location_id=? AND batch_no=?"));
    query.addBindValue(m_materialCombo->currentData());
    query.addBindValue(m_warehouseCombo->currentData());
    query.addBindValue(m_locationCombo->currentData());
    query.addBindValue(m_batchCombo->currentData().toString());
    double quantity = 0.0;
    if (query.exec() && query.next()) quantity = query.value(0).toDouble();
    QString display = QString::number(quantity, 'f', 6);
    display.remove(QRegularExpression(QStringLiteral("\\.?0+$")));
    m_availableLabel->setText(QStringLiteral("当前可用库存：%1").arg(display));
    m_submitButton->setEnabled(m_session.canManageWarehouse() && quantity > 0.0000001);
}

QStringList StockOutPage::selectedSerialNumbers() const
{
    QStringList serials;
    for (const QListWidgetItem *item : m_serialList->selectedItems()) serials.append(item->text());
    return serials;
}

void StockOutPage::submit()
{
    if (m_materialCombo->currentIndex() < 0 || m_locationCombo->currentIndex() < 0
        || m_batchCombo->currentIndex() < 0) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"),
                             QStringLiteral("请选择有可用库存的物料、仓库、库位和批次。"));
        return;
    }
    const int materialIndex = m_materialCombo->currentIndex();
    const bool requireSerial = m_materialCombo->itemData(materialIndex, RequireSerialRole).toBool();
    if (requireSerial) {
        const double quantity = m_quantitySpin->value();
        if (std::abs(quantity - std::round(quantity)) > 0.0000001
            || selectedSerialNumbers().size() != static_cast<int>(std::round(quantity))) {
            QMessageBox::warning(this, QStringLiteral("SN数量不一致"),
                                 QStringLiteral("SN物料的出库数量必须是整数，并且要选择相同数量的SN。"));
            return;
        }
    }
    if (QMessageBox::question(this, QStringLiteral("确认出库"),
        QStringLiteral("确认提交本次出库？库存将立即扣减，并自动生成库存流水。")) != QMessageBox::Yes) {
        return;
    }

    StockMovementRequest request;
    request.documentType = m_typeCombo->currentData().toString();
    request.documentDate = m_dateEdit->date();
    request.handlerName = m_receiverEdit->text().trimmed();
    request.purpose = m_purposeEdit->text().trimmed();
    request.notes = m_notesEdit->toPlainText().trimmed();
    request.materialId = m_materialCombo->currentData().toLongLong();
    request.quantity = m_quantitySpin->value();
    request.batchNo = m_batchCombo->currentData().toString();
    request.warehouseId = m_warehouseCombo->currentData().toLongLong();
    request.locationId = m_locationCombo->currentData().toLongLong();
    request.serialNumbers = selectedSerialNumbers();

    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    QString error;
    if (!service.postOutbound(request, &posted, &error)) {
        QMessageBox::warning(this, QStringLiteral("出库失败"), error);
        return;
    }
    m_numberLabel->setText(posted.documentNumber);
    QMessageBox::information(this, QStringLiteral("出库完成"),
                             QStringLiteral("出库成功，单据号：%1").arg(posted.documentNumber));
    m_quantitySpin->setValue(1.0);
    m_receiverEdit->clear();
    m_purposeEdit->clear();
    m_notesEdit->clear();
    emit stockChanged();
    refreshReferenceData();
}

