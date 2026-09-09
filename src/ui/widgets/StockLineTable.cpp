#include "ui/widgets/StockLineTable.h"

#include "ui/widgets/ComboBoxSearch.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace {
constexpr int RequireBatchRole = Qt::UserRole + 1;
constexpr int RequireSerialRole = Qt::UserRole + 2;
constexpr int DefaultWarehouseRole = Qt::UserRole + 3;
constexpr int DefaultLocationRole = Qt::UserRole + 4;
constexpr int UnitUsageRole = Qt::UserRole + 5;
constexpr int MaterialColumn = 0;
constexpr int WarehouseColumn = 1;
constexpr int LocationColumn = 2;
constexpr int BatchColumn = 3;
constexpr int AvailableColumn = 4;
constexpr int UnitUsageColumn = 5;
constexpr int OrderedColumn = 6;
constexpr int QuantityColumn = 7;
constexpr int GiftColumn = 8;
constexpr int SerialColumn = 9;
constexpr int ActionColumn = 10;
constexpr double QuantityTolerance = 0.0000001;
}

StockLineTable::StockLineTable(QSqlDatabase database, Mode mode, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_mode(mode),
      m_purchaseMode(mode == Mode::Inbound)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    auto *toolbar = new QHBoxLayout;
    auto *hint = new QLabel(m_mode == Mode::Inbound
                                ? QStringLiteral("逐行选择入库物料、目标库位、批次和数量")
                                : QStringLiteral("逐行选择出库物料、库存批次和数量"), this);
    hint->setObjectName(QStringLiteral("mutedText"));
    auto *addButton = new QPushButton(QStringLiteral("添加物料"), this);
    addButton->setProperty("primary", true);
    m_importProductionBomButton = new QPushButton(QStringLiteral("一键导入BOM用料"), this);
    m_importProductionBomButton->setVisible(false);
    m_importProductionBomButton->setToolTip(
        QStringLiteral("递归导入当前成品BOM中的全部末级领用物料"));
    toolbar->addWidget(hint);
    toolbar->addStretch();
    toolbar->addWidget(addButton);
    toolbar->addWidget(m_importProductionBomButton);
    root->addLayout(toolbar);

    m_table = new QTableWidget(0, 11, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("物料"), QStringLiteral("仓库"),
                                        QStringLiteral("库位"), QStringLiteral("批次"),
                                        QStringLiteral("当前库存"),
                                        QStringLiteral("单台用量"),
                                        QStringLiteral("采购数量"),
                                        m_mode == Mode::Inbound ? QStringLiteral("入库数量")
                                                                : QStringLiteral("出库数量"),
                                        QStringLiteral("其中赠送"),
                                        QStringLiteral("SN"), QStringLiteral("操作")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(MaterialColumn, QHeaderView::Stretch);
    m_table->setMinimumHeight(240);
    m_table->setColumnHidden(UnitUsageColumn, true);
    if (m_mode == Mode::Outbound) {
        m_table->setColumnHidden(OrderedColumn, true);
        m_table->setColumnHidden(GiftColumn, true);
    }
    root->addWidget(m_table);

    connect(addButton, &QPushButton::clicked, this, &StockLineTable::addLine);
    connect(m_importProductionBomButton, &QPushButton::clicked,
            this, &StockLineTable::productionBomRequested);
    addLine();
}

int StockLineTable::rowForWidget(const QWidget *widget, int column) const
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->cellWidget(row, column) == widget) {
            return row;
        }
    }
    return -1;
}

QComboBox *StockLineTable::comboAt(int row, int column) const
{
    return qobject_cast<QComboBox *>(m_table->cellWidget(row, column));
}

void StockLineTable::loadMaterials(QComboBox *combo, const QVariant &selected)
{
    combo->blockSignals(true);
    combo->clear();
    QSqlQuery query(m_database);
    QString sql = QStringLiteral(
        "SELECT m.id,m.code,m.name,m.require_batch,m.require_serial,"
        "m.default_warehouse_id,m.default_location_id,m.specification,c.name,m.unit,m.unit_usage "
        "FROM materials m LEFT JOIN material_categories c ON c.id=m.category_id "
        "WHERE m.is_active=1");
    if (m_mode == Mode::Outbound && m_productionUsageMode) {
        sql += QStringLiteral(" AND COALESCE(c.code,'')<>'FINISHED'");
    } else if (m_mode == Mode::Outbound) {
        sql += QStringLiteral(
            " AND EXISTS(SELECT 1 FROM stock_balances s "
            "WHERE s.material_id=m.id AND s.quantity>0)");
    }
    if (!m_materialCategoryFilter.isEmpty()) sql += QStringLiteral(" AND c.code=?");
    sql += QStringLiteral(" ORDER BY m.code");
    query.prepare(sql);
    if (!m_materialCategoryFilter.isEmpty()) query.addBindValue(m_materialCategoryFilter);
    query.exec();
    while (query.next()) {
        const int index = combo->count();
        combo->addItem(QStringLiteral("%1 - %2")
                           .arg(query.value(1).toString(), query.value(2).toString()),
                       query.value(0));
        combo->setItemData(index, query.value(3), RequireBatchRole);
        combo->setItemData(index, query.value(4), RequireSerialRole);
        combo->setItemData(index, query.value(5), DefaultWarehouseRole);
        combo->setItemData(index, query.value(6), DefaultLocationRole);
        combo->setItemData(index, query.value(10), UnitUsageRole);
        combo->setItemData(index,
            QStringLiteral("编码：%1\n名称：%2\n规格：%3\n分类：%4\n单位：%5\n单台用量：%6")
                .arg(query.value(1).toString(), query.value(2).toString(),
                     query.value(7).toString(), query.value(8).toString(),
                     query.value(9).toString(),
                     QString::number(query.value(10).toDouble(), 'g', 12)),
            Qt::ToolTipRole);
    }
    const int selectedIndex = combo->findData(selected);
    if (selectedIndex >= 0) {
        combo->setCurrentIndex(selectedIndex);
    } else if (combo->count() > 0 && !m_requireExplicitMaterialSelection) {
        combo->setCurrentIndex(0);
    } else {
        combo->setCurrentIndex(-1);
    }
    combo->blockSignals(false);
}

void StockLineTable::addLine()
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    auto *material = new QComboBox(m_table);
    ComboBoxSearch::enableContainsSearch(
        material, QStringLiteral("输入物料编码或名称检索"));
    auto *warehouse = new QComboBox(m_table);
    auto *location = new QComboBox(m_table);
    auto *batch = new QComboBox(m_table);
    batch->setEditable(m_mode == Mode::Inbound);
    if (m_mode == Mode::Inbound) batch->setInsertPolicy(QComboBox::NoInsert);
    auto *quantity = new QDoubleSpinBox(m_table);
    quantity->setDecimals(6);
    quantity->setRange(0.000001, 999999999999.0);
    quantity->setValue(1.0);
    auto *unitUsage = new QDoubleSpinBox(m_table);
    unitUsage->setDecimals(6);
    unitUsage->setRange(0.0, 999999999999.0);
    unitUsage->setValue(0.0);
    auto *ordered = new QDoubleSpinBox(m_table);
    ordered->setDecimals(6);
    ordered->setRange(0.0, 999999999999.0);
    ordered->setValue(m_purchaseMode ? 1.0 : 0.0);
    auto *gift = new QDoubleSpinBox(m_table);
    gift->setDecimals(6);
    gift->setRange(0.0, qMax(0.0, quantity->value() - ordered->value()));
    gift->setValue(0.0);
    auto *serialButton = new QPushButton(QStringLiteral("无需选择"), m_table);
    auto *removeButton = new QPushButton(QStringLiteral("删除"), m_table);
    removeButton->setProperty("danger", true);
    m_table->setCellWidget(row, MaterialColumn, material);
    m_table->setCellWidget(row, WarehouseColumn, warehouse);
    m_table->setCellWidget(row, LocationColumn, location);
    m_table->setCellWidget(row, BatchColumn, batch);
    m_table->setItem(row, AvailableColumn, new QTableWidgetItem(QStringLiteral("0")));
    m_table->setCellWidget(row, UnitUsageColumn, unitUsage);
    m_table->setCellWidget(row, OrderedColumn, ordered);
    m_table->setCellWidget(row, QuantityColumn, quantity);
    m_table->setCellWidget(row, GiftColumn, gift);
    m_table->setCellWidget(row, SerialColumn, serialButton);
    m_table->setCellWidget(row, ActionColumn, removeButton);

    connect(material, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, material] {
                const int currentRow = rowForWidget(material, MaterialColumn);
                if (currentRow >= 0) {
                    loadUnitUsage(currentRow);
                    loadWarehouses(currentRow);
                }
            });
    connect(warehouse, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, warehouse] {
                const int currentRow = rowForWidget(warehouse, WarehouseColumn);
                if (currentRow >= 0) loadLocations(currentRow);
            });
    connect(location, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, location] {
                const int currentRow = rowForWidget(location, LocationColumn);
                if (currentRow >= 0) loadBatches(currentRow);
            });
    connect(batch, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, batch] {
                const int currentRow = rowForWidget(batch, BatchColumn);
                if (currentRow >= 0) {
                    auto *serialButton = qobject_cast<QPushButton *>(
                        m_table->cellWidget(currentRow, SerialColumn));
                    if (serialButton) serialButton->setProperty("serials", QStringList());
                    updateAvailable(currentRow);
                }
            });
    if (m_mode == Mode::Inbound) {
        connect(batch, &QComboBox::editTextChanged, this,
                [this, batch] {
                    const int currentRow = rowForWidget(batch, BatchColumn);
                    if (currentRow >= 0) updateAvailable(currentRow);
                });
    }
    connect(serialButton, &QPushButton::clicked, this,
            [this, serialButton] {
                const int currentRow = rowForWidget(serialButton, SerialColumn);
                if (currentRow >= 0) chooseSerials(currentRow);
            });
    auto updateGiftMaximum = [ordered, quantity, gift] {
        gift->setMaximum(qMax(0.0, quantity->value() - ordered->value()));
    };
    connect(quantity, qOverload<double>(&QDoubleSpinBox::valueChanged), gift,
            [updateGiftMaximum](double) { updateGiftMaximum(); });
    connect(ordered, qOverload<double>(&QDoubleSpinBox::valueChanged), gift,
            [updateGiftMaximum](double) { updateGiftMaximum(); });
    connect(unitUsage, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this, unitUsage](double) {
                const int currentRow = rowForWidget(unitUsage, UnitUsageColumn);
                if (currentRow >= 0) updateProductionQuantity(currentRow);
            });
    connect(removeButton, &QPushButton::clicked, this,
            [this, removeButton] {
                const int currentRow = rowForWidget(removeButton, ActionColumn);
                if (currentRow >= 0) removeLine(currentRow);
            });

    loadMaterials(material);
    loadUnitUsage(row);
    quantity->setReadOnly(m_productionUsageMode);
    quantity->setButtonSymbols(m_productionUsageMode ? QAbstractSpinBox::NoButtons
                                                      : QAbstractSpinBox::UpDownArrows);
    loadWarehouses(row);
}

bool StockLineTable::setProductionMaterials(
    const QList<QPair<qlonglong, double>> &materials,
    QString *errorMessage)
{
    if (!m_productionUsageMode) {
        if (errorMessage) *errorMessage = QStringLiteral("当前明细表不是生产领料模式。");
        return false;
    }

    m_table->setUpdatesEnabled(false);
    m_table->setRowCount(0);
    QString missingMaterial;
    for (const auto &productionMaterial : materials) {
        const qlonglong materialId = productionMaterial.first;
        addLine();
        const int row = m_table->rowCount() - 1;
        QComboBox *material = comboAt(row, MaterialColumn);
        const int materialIndex = material ? material->findData(materialId) : -1;
        if (materialIndex < 0) {
            missingMaterial = QString::number(materialId);
            break;
        }
        material->setCurrentIndex(materialIndex);
        auto *unitUsage = qobject_cast<QDoubleSpinBox *>(
            m_table->cellWidget(row, UnitUsageColumn));
        if (unitUsage) {
            const QSignalBlocker blocker(unitUsage);
            unitUsage->setValue(productionMaterial.second);
            updateProductionQuantity(row);
        }
    }
    m_table->setUpdatesEnabled(true);

    if (!missingMaterial.isEmpty()) {
        m_table->setRowCount(0);
        if (!m_keepEmptyWhenNoRows) addLine();
        if (errorMessage) {
            *errorMessage = QStringLiteral("物料 ID %1 已停用或不存在，无法自动生成领料明细。")
                                .arg(missingMaterial);
        }
        return false;
    }
    if (materials.isEmpty() && !m_keepEmptyWhenNoRows) addLine();
    return true;
}

void StockLineTable::setProductionUsageMode(bool enabled)
{
    m_productionUsageMode = m_mode == Mode::Outbound && enabled;
    m_importProductionBomButton->setVisible(m_productionUsageMode);
    m_keepEmptyWhenNoRows = m_productionUsageMode;
    m_requireExplicitMaterialSelection = m_productionUsageMode;
    m_table->setColumnHidden(UnitUsageColumn, !m_productionUsageMode);
    m_table->setHorizontalHeaderItem(
        QuantityColumn,
        new QTableWidgetItem(m_productionUsageMode ? QStringLiteral("总用量")
                                                   : (m_mode == Mode::Inbound
                                                          ? QStringLiteral("入库数量")
                                                          : QStringLiteral("出库数量"))));
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *unitUsage = qobject_cast<QDoubleSpinBox *>(
            m_table->cellWidget(row, UnitUsageColumn));
        auto *quantity = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, QuantityColumn));
        if (unitUsage) unitUsage->setEnabled(m_productionUsageMode);
        if (quantity) {
            quantity->setReadOnly(m_productionUsageMode);
            quantity->setButtonSymbols(m_productionUsageMode ? QAbstractSpinBox::NoButtons
                                                              : QAbstractSpinBox::UpDownArrows);
        }
        loadUnitUsage(row);
    }
    if (m_productionUsageMode) m_table->setRowCount(0);
    else if (m_table->rowCount() == 0) addLine();
}

void StockLineTable::setMaterialCategoryFilter(const QString &categoryCode)
{
    const QString normalized = categoryCode.trimmed().toUpper();
    if (m_materialCategoryFilter == normalized) return;
    m_materialCategoryFilter = normalized;
    refreshReferenceData();
}

void StockLineTable::setProductionQuantity(double quantity)
{
    m_productionQuantity = qMax(0.0, quantity);
    if (!m_productionUsageMode) return;
    for (int row = 0; row < m_table->rowCount(); ++row) updateProductionQuantity(row);
}

void StockLineTable::updateProductionQuantity(int row)
{
    auto *unitUsage = qobject_cast<QDoubleSpinBox *>(
        m_table->cellWidget(row, UnitUsageColumn));
    auto *quantity = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, QuantityColumn));
    QComboBox *material = comboAt(row, MaterialColumn);
    if (!material || !unitUsage || !quantity) return;
    if (!m_productionUsageMode) return;
    quantity->setMinimum(0.0);
    quantity->setValue(unitUsage->value() * m_productionQuantity);
    auto *serialButton = qobject_cast<QPushButton *>(m_table->cellWidget(row, SerialColumn));
    if (serialButton) {
        serialButton->setProperty("serials", QStringList());
        serialButton->setText(material->currentData(RequireSerialRole).toBool()
                                  ? QStringLiteral("已选 0 个")
                                  : QStringLiteral("无需选择"));
    }
}

void StockLineTable::loadUnitUsage(int row)
{
    QComboBox *material = comboAt(row, MaterialColumn);
    auto *unitUsage = qobject_cast<QDoubleSpinBox *>(
        m_table->cellWidget(row, UnitUsageColumn));
    if (!material || !unitUsage) return;
    const QSignalBlocker blocker(unitUsage);
    unitUsage->setValue(material->currentData(UnitUsageRole).toDouble());
    updateProductionQuantity(row);
}

void StockLineTable::setPurchaseMode(bool enabled)
{
    m_purchaseMode = m_mode == Mode::Inbound && enabled;
    m_table->setColumnHidden(OrderedColumn, !m_purchaseMode);
    m_table->setColumnHidden(GiftColumn, !m_purchaseMode);
    m_table->setHorizontalHeaderItem(
        QuantityColumn,
        new QTableWidgetItem(m_purchaseMode ? QStringLiteral("实际入库")
                                            : (m_mode == Mode::Inbound
                                                   ? QStringLiteral("入库数量")
                                                   : QStringLiteral("出库数量"))));
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *ordered = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, OrderedColumn));
        auto *quantity = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, QuantityColumn));
        auto *gift = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, GiftColumn));
        if (!ordered || !quantity || !gift) continue;
        if (m_purchaseMode) {
            if (ordered->value() <= QuantityTolerance) ordered->setValue(quantity->value());
        } else {
            ordered->setValue(0.0);
            gift->setValue(0.0);
        }
    }
}

QStringList StockLineTable::purchaseWarnings() const
{
    QStringList warnings;
    if (!m_purchaseMode) return warnings;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *material = comboAt(row, MaterialColumn);
        auto *ordered = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, OrderedColumn));
        auto *quantity = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, QuantityColumn));
        auto *gift = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, GiftColumn));
        if (!material || !ordered || !quantity || !gift) continue;
        const double difference = quantity->value() - ordered->value();
        const QString label = material->currentText();
        if (difference < -QuantityTolerance) {
            warnings.append(QStringLiteral("第 %1 行 %2：少到货 %3")
                                .arg(row + 1).arg(label)
                                .arg(-difference, 0, 'g', 12));
        } else if (difference > QuantityTolerance
                   && gift->value() + QuantityTolerance < difference) {
            warnings.append(QStringLiteral("第 %1 行 %2：多到货 %3，其中仍有 %4 未标记为赠送，将计入对账数量")
                                .arg(row + 1).arg(label)
                                .arg(difference, 0, 'g', 12)
                                .arg(difference - gift->value(), 0, 'g', 12));
        }
    }
    return warnings;
}

void StockLineTable::refreshReferenceData()
{
    if (m_table->rowCount() == 0) {
        if (!m_keepEmptyWhenNoRows) addLine();
        return;
    }
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QComboBox *material = comboAt(row, MaterialColumn);
        const QVariant selected = material ? material->currentData() : QVariant();
        if (material) loadMaterials(material, selected);
        loadUnitUsage(row);
        loadWarehouses(row);
    }
}

void StockLineTable::clearLines()
{
    m_table->setRowCount(0);
    if (!m_keepEmptyWhenNoRows) addLine();
}

void StockLineTable::loadWarehouses(int row)
{
    QComboBox *material = comboAt(row, MaterialColumn);
    QComboBox *warehouse = comboAt(row, WarehouseColumn);
    if (!material || !warehouse) return;
    const QVariant previous = warehouse->currentData();
    warehouse->blockSignals(true);
    warehouse->clear();
    QSqlQuery query(m_database);
    if (m_mode == Mode::Inbound) {
        query.prepare(QStringLiteral(
            "SELECT id,code,name FROM warehouses WHERE is_active=1 ORDER BY code"));
    } else {
        query.prepare(QStringLiteral(
            "SELECT DISTINCT w.id,w.code,w.name FROM warehouses w "
            "JOIN stock_balances s ON s.warehouse_id=w.id "
            "WHERE w.is_active=1 AND s.material_id=? AND s.quantity>0 ORDER BY w.code"));
        query.addBindValue(material->currentData());
    }
    query.exec();
    while (query.next()) {
        warehouse->addItem(QStringLiteral("%1 - %2")
                               .arg(query.value(1).toString(), query.value(2).toString()),
                           query.value(0));
    }
    QVariant selected = previous;
    if (!selected.isValid()) selected = material->currentData(DefaultWarehouseRole);
    const int selectedIndex = warehouse->findData(selected);
    if (selectedIndex >= 0) warehouse->setCurrentIndex(selectedIndex);
    else if (warehouse->count() > 0) warehouse->setCurrentIndex(0);
    warehouse->blockSignals(false);
    loadLocations(row);
}

void StockLineTable::loadLocations(int row)
{
    QComboBox *material = comboAt(row, MaterialColumn);
    QComboBox *warehouse = comboAt(row, WarehouseColumn);
    QComboBox *location = comboAt(row, LocationColumn);
    if (!material || !warehouse || !location) return;
    const QVariant previous = location->currentData();
    location->blockSignals(true);
    location->clear();
    QSqlQuery query(m_database);
    if (m_mode == Mode::Inbound) {
        query.prepare(QStringLiteral(
            "SELECT id,code,name FROM locations WHERE is_active=1 AND warehouse_id=? ORDER BY code"));
        query.addBindValue(warehouse->currentData());
    } else {
        query.prepare(QStringLiteral(
            "SELECT DISTINCT l.id,l.code,l.name FROM locations l "
            "JOIN stock_balances s ON s.location_id=l.id "
            "WHERE l.is_active=1 AND l.warehouse_id=? AND s.material_id=? AND s.quantity>0 "
            "ORDER BY l.code"));
        query.addBindValue(warehouse->currentData());
        query.addBindValue(material->currentData());
    }
    query.exec();
    while (query.next()) {
        QString label = query.value(1).toString();
        if (!query.value(2).toString().isEmpty()) label += QStringLiteral(" - ") + query.value(2).toString();
        location->addItem(label, query.value(0));
    }
    QVariant selected = previous;
    if (!selected.isValid()) selected = material->currentData(DefaultLocationRole);
    const int selectedIndex = location->findData(selected);
    if (selectedIndex >= 0) location->setCurrentIndex(selectedIndex);
    else if (location->count() > 0) location->setCurrentIndex(0);
    location->blockSignals(false);
    loadBatches(row);
}

void StockLineTable::loadBatches(int row)
{
    QComboBox *material = comboAt(row, MaterialColumn);
    QComboBox *warehouse = comboAt(row, WarehouseColumn);
    QComboBox *location = comboAt(row, LocationColumn);
    QComboBox *batch = comboAt(row, BatchColumn);
    if (!material || !warehouse || !location || !batch) return;
    const QString previous = m_mode == Mode::Inbound
        ? batch->currentText().trimmed() : batch->currentData().toString();
    batch->blockSignals(true);
    batch->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT batch_no,quantity FROM stock_balances WHERE material_id=? AND warehouse_id=? "
        "AND location_id=? %1 ORDER BY batch_no")
                      .arg(m_mode == Mode::Inbound ? QString() : QStringLiteral("AND quantity>0")));
    query.addBindValue(material->currentData());
    query.addBindValue(warehouse->currentData());
    query.addBindValue(location->currentData());
    query.exec();
    while (query.next()) {
        const QString value = query.value(0).toString();
        batch->addItem(value.isEmpty() ? QStringLiteral("无批次") : value, value);
    }
    if (m_mode == Mode::Inbound) {
        if (batch->findData(QString()) < 0) batch->insertItem(0, QStringLiteral("无批次"), QString());
        batch->setEditText(previous);
    } else {
        const int selectedIndex = batch->findData(previous);
        if (selectedIndex >= 0) batch->setCurrentIndex(selectedIndex);
        else if (batch->count() > 0) batch->setCurrentIndex(0);
    }
    batch->blockSignals(false);
    auto *serialButton = qobject_cast<QPushButton *>(m_table->cellWidget(row, SerialColumn));
    if (serialButton) serialButton->setProperty("serials", QStringList());
    updateAvailable(row);
}

void StockLineTable::updateAvailable(int row)
{
    QComboBox *material = comboAt(row, MaterialColumn);
    QComboBox *warehouse = comboAt(row, WarehouseColumn);
    QComboBox *location = comboAt(row, LocationColumn);
    QComboBox *batch = comboAt(row, BatchColumn);
    if (!material || !warehouse || !location || !batch) return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT COALESCE(quantity,0) FROM stock_balances WHERE material_id=? AND warehouse_id=? "
        "AND location_id=? AND batch_no=?"));
    query.addBindValue(material->currentData());
    query.addBindValue(warehouse->currentData());
    query.addBindValue(location->currentData());
    query.addBindValue(m_mode == Mode::Inbound ? batch->currentText().trimmed()
                                               : batch->currentData().toString());
    double available = 0.0;
    if (query.exec() && query.next()) available = query.value(0).toDouble();
    m_table->item(row, AvailableColumn)->setText(QString::number(available, 'g', 12));

    auto *serialButton = qobject_cast<QPushButton *>(m_table->cellWidget(row, SerialColumn));
    const bool requireSerial = material->currentData(RequireSerialRole).toBool();
    serialButton->setEnabled(requireSerial && (m_mode == Mode::Inbound || available > 0.0));
    if (!requireSerial) {
        serialButton->setProperty("serials", QStringList());
        serialButton->setText(QStringLiteral("无需选择"));
    } else {
        const QStringList selected = serialButton->property("serials").toStringList();
        serialButton->setText(QStringLiteral("已选 %1 个").arg(selected.size()));
    }
}

void StockLineTable::chooseSerials(int row)
{
    QComboBox *material = comboAt(row, MaterialColumn);
    QComboBox *warehouse = comboAt(row, WarehouseColumn);
    QComboBox *location = comboAt(row, LocationColumn);
    QComboBox *batch = comboAt(row, BatchColumn);
    auto *button = qobject_cast<QPushButton *>(m_table->cellWidget(row, SerialColumn));
    if (!material || !warehouse || !location || !batch || !button) return;

    QDialog dialog(this);
    dialog.setWindowTitle(m_mode == Mode::Inbound
                              ? QStringLiteral("填写本行入库SN")
                              : QStringLiteral("选择本行出库SN"));
    dialog.resize(480, 430);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(m_mode == Mode::Inbound
                                 ? QStringLiteral("每行填写一个SN，也可以用逗号分隔；重复SN会被自动去除。")
                                 : QStringLiteral("仅显示所选物料、仓库、库位和批次中的可用SN。"),
                             &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    const QStringList previous = button->property("serials").toStringList();
    QListWidget *list = nullptr;
    QTextEdit *editor = nullptr;
    if (m_mode == Mode::Inbound) {
        editor = new QTextEdit(&dialog);
        editor->setPlainText(previous.join(QLatin1Char('\n')));
        layout->addWidget(editor);
    } else {
        list = new QListWidget(&dialog);
        list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        const QSet<QString> selected(previous.cbegin(), previous.cend());
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "SELECT serial_no FROM serial_numbers WHERE material_id=? AND warehouse_id=? "
            "AND location_id=? AND batch_no=? AND status='IN_STOCK' ORDER BY serial_no"));
        query.addBindValue(material->currentData());
        query.addBindValue(warehouse->currentData());
        query.addBindValue(location->currentData());
        query.addBindValue(batch->currentData().toString());
        query.exec();
        while (query.next()) {
            auto *item = new QListWidgetItem(query.value(0).toString(), list);
            item->setSelected(selected.contains(item->text()));
        }
        layout->addWidget(list);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    QStringList serials;
    if (editor) {
        QSet<QString> unique;
        const QStringList values = editor->toPlainText().split(
            QRegularExpression(QStringLiteral("[,，;；\\s]+")), Qt::SkipEmptyParts);
        for (const QString &value : values) unique.insert(value.trimmed().toUpper());
        serials = unique.values();
    } else {
        for (const QListWidgetItem *item : list->selectedItems()) serials.append(item->text());
    }
    serials.sort();
    button->setProperty("serials", serials);
    button->setText(QStringLiteral("已选 %1 个").arg(serials.size()));
}

void StockLineTable::removeLine(int row)
{
    if (row >= 0) m_table->removeRow(row);
}

QList<StockMovementRequest> StockLineTable::lines(QString *errorMessage) const
{
    QList<StockMovementRequest> result;
    QSet<QString> identities;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QComboBox *material = comboAt(row, MaterialColumn);
        QComboBox *warehouse = comboAt(row, WarehouseColumn);
        QComboBox *location = comboAt(row, LocationColumn);
        QComboBox *batch = comboAt(row, BatchColumn);
        auto *ordered = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, OrderedColumn));
        auto *quantity = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, QuantityColumn));
        auto *unitUsage = qobject_cast<QDoubleSpinBox *>(
            m_table->cellWidget(row, UnitUsageColumn));
        auto *gift = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, GiftColumn));
        auto *serialButton = qobject_cast<QPushButton *>(m_table->cellWidget(row, SerialColumn));
        if (!material || !warehouse || !location || !batch || !ordered || !quantity || !gift
            || material->currentIndex() < 0 || warehouse->currentIndex() < 0
            || location->currentIndex() < 0
            || (m_mode == Mode::Outbound && batch->currentIndex() < 0)) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 行资料不完整。").arg(row + 1);
            return {};
        }
        if (m_productionUsageMode
            && (!unitUsage || unitUsage->value() <= QuantityTolerance)) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 行物料未设置单台用量。")
                                                  .arg(row + 1);
            return {};
        }
        if (quantity->value() <= QuantityTolerance) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 行数量必须大于0。")
                                                  .arg(row + 1);
            return {};
        }
        const QString batchNumber = m_mode == Mode::Inbound ? batch->currentText().trimmed()
                                                            : batch->currentData().toString();
        const QString identity = QStringLiteral("%1|%2|%3|%4")
                                     .arg(material->currentData().toLongLong())
                                     .arg(warehouse->currentData().toLongLong())
                                     .arg(location->currentData().toLongLong())
                                     .arg(batchNumber.toUpper());
        if (identities.contains(identity)) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 行与前面明细重复。").arg(row + 1);
            return {};
        }
        identities.insert(identity);
        StockMovementRequest line;
        line.materialId = material->currentData().toLongLong();
        line.orderedQuantity = m_purchaseMode ? ordered->value() : 0.0;
        line.quantity = quantity->value();
        line.giftQuantity = m_purchaseMode ? gift->value() : 0.0;
        line.batchNo = batchNumber;
        line.warehouseId = warehouse->currentData().toLongLong();
        line.locationId = location->currentData().toLongLong();
        line.serialNumbers = serialButton ? serialButton->property("serials").toStringList() : QStringList();
        if (line.giftQuantity < -QuantityTolerance
            || line.giftQuantity > qMax(0.0, line.quantity - line.orderedQuantity)
                                        + QuantityTolerance) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 行赠送数量不能超过多到货数量。")
                                                  .arg(row + 1);
            return {};
        }
        if (material->currentData(RequireSerialRole).toBool()) {
            const double roundedQuantity = std::round(line.quantity);
            if (std::abs(line.quantity - roundedQuantity) > 0.0000001
                || line.serialNumbers.size() != static_cast<int>(roundedQuantity)) {
                if (errorMessage) *errorMessage = QStringLiteral("第 %1 行的SN数量必须等于整数业务数量。").arg(row + 1);
                return {};
            }
        }
        result.append(line);
    }
    if (result.isEmpty() && errorMessage) {
        *errorMessage = QStringLiteral("请至少添加一条物料明细。");
    }
    return result;
}
