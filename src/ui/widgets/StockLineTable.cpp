#include "ui/widgets/StockLineTable.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace {
constexpr int RequireBatchRole = Qt::UserRole + 1;
constexpr int RequireSerialRole = Qt::UserRole + 2;
constexpr int DefaultWarehouseRole = Qt::UserRole + 3;
constexpr int DefaultLocationRole = Qt::UserRole + 4;
constexpr int MaterialColumn = 0;
constexpr int WarehouseColumn = 1;
constexpr int LocationColumn = 2;
constexpr int BatchColumn = 3;
constexpr int AvailableColumn = 4;
constexpr int QuantityColumn = 5;
constexpr int SerialColumn = 6;
constexpr int ActionColumn = 7;
}

StockLineTable::StockLineTable(QSqlDatabase database, QWidget *parent)
    : QWidget(parent), m_database(std::move(database))
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    auto *toolbar = new QHBoxLayout;
    auto *hint = new QLabel(QStringLiteral("逐行选择实际领用的物料、库存批次和数量"), this);
    hint->setObjectName(QStringLiteral("mutedText"));
    auto *addButton = new QPushButton(QStringLiteral("添加物料"), this);
    addButton->setProperty("primary", true);
    toolbar->addWidget(hint);
    toolbar->addStretch();
    toolbar->addWidget(addButton);
    root->addLayout(toolbar);

    m_table = new QTableWidget(0, 8, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("物料"), QStringLiteral("仓库"),
                                        QStringLiteral("库位"), QStringLiteral("库存批次"),
                                        QStringLiteral("可用库存"), QStringLiteral("领料数量"),
                                        QStringLiteral("SN"), QStringLiteral("操作")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(MaterialColumn, QHeaderView::Stretch);
    m_table->setMinimumHeight(240);
    root->addWidget(m_table);

    connect(addButton, &QPushButton::clicked, this, &StockLineTable::addLine);
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
    query.exec(QStringLiteral(
        "SELECT m.id,m.code,m.name,m.require_batch,m.require_serial,"
        "m.default_warehouse_id,m.default_location_id FROM materials m "
        "WHERE m.is_active=1 AND EXISTS(SELECT 1 FROM stock_balances s "
        "WHERE s.material_id=m.id AND s.quantity>0) ORDER BY m.code"));
    while (query.next()) {
        const int index = combo->count();
        combo->addItem(QStringLiteral("%1 - %2")
                           .arg(query.value(1).toString(), query.value(2).toString()),
                       query.value(0));
        combo->setItemData(index, query.value(3), RequireBatchRole);
        combo->setItemData(index, query.value(4), RequireSerialRole);
        combo->setItemData(index, query.value(5), DefaultWarehouseRole);
        combo->setItemData(index, query.value(6), DefaultLocationRole);
    }
    const int selectedIndex = combo->findData(selected);
    if (selectedIndex >= 0) {
        combo->setCurrentIndex(selectedIndex);
    } else if (combo->count() > 0) {
        combo->setCurrentIndex(0);
    }
    combo->blockSignals(false);
}

void StockLineTable::addLine()
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    auto *material = new QComboBox(m_table);
    material->setEditable(true);
    material->setInsertPolicy(QComboBox::NoInsert);
    auto *warehouse = new QComboBox(m_table);
    auto *location = new QComboBox(m_table);
    auto *batch = new QComboBox(m_table);
    auto *quantity = new QDoubleSpinBox(m_table);
    quantity->setDecimals(6);
    quantity->setRange(0.000001, 999999999999.0);
    quantity->setValue(1.0);
    auto *serialButton = new QPushButton(QStringLiteral("无需选择"), m_table);
    auto *removeButton = new QPushButton(QStringLiteral("删除"), m_table);
    removeButton->setProperty("danger", true);
    m_table->setCellWidget(row, MaterialColumn, material);
    m_table->setCellWidget(row, WarehouseColumn, warehouse);
    m_table->setCellWidget(row, LocationColumn, location);
    m_table->setCellWidget(row, BatchColumn, batch);
    m_table->setItem(row, AvailableColumn, new QTableWidgetItem(QStringLiteral("0")));
    m_table->setCellWidget(row, QuantityColumn, quantity);
    m_table->setCellWidget(row, SerialColumn, serialButton);
    m_table->setCellWidget(row, ActionColumn, removeButton);

    connect(material, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, material] {
                const int currentRow = rowForWidget(material, MaterialColumn);
                if (currentRow >= 0) loadWarehouses(currentRow);
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
    connect(serialButton, &QPushButton::clicked, this,
            [this, serialButton] {
                const int currentRow = rowForWidget(serialButton, SerialColumn);
                if (currentRow >= 0) chooseSerials(currentRow);
            });
    connect(removeButton, &QPushButton::clicked, this,
            [this, removeButton] {
                const int currentRow = rowForWidget(removeButton, ActionColumn);
                if (currentRow >= 0) removeLine(currentRow);
            });

    loadMaterials(material);
    loadWarehouses(row);
}

void StockLineTable::refreshReferenceData()
{
    if (m_table->rowCount() == 0) {
        addLine();
        return;
    }
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QComboBox *material = comboAt(row, MaterialColumn);
        const QVariant selected = material ? material->currentData() : QVariant();
        if (material) loadMaterials(material, selected);
        loadWarehouses(row);
    }
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
    query.prepare(QStringLiteral(
        "SELECT DISTINCT w.id,w.code,w.name FROM warehouses w "
        "JOIN stock_balances s ON s.warehouse_id=w.id "
        "WHERE w.is_active=1 AND s.material_id=? AND s.quantity>0 ORDER BY w.code"));
    query.addBindValue(material->currentData());
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
    query.prepare(QStringLiteral(
        "SELECT DISTINCT l.id,l.code,l.name FROM locations l "
        "JOIN stock_balances s ON s.location_id=l.id "
        "WHERE l.is_active=1 AND l.warehouse_id=? AND s.material_id=? AND s.quantity>0 "
        "ORDER BY l.code"));
    query.addBindValue(warehouse->currentData());
    query.addBindValue(material->currentData());
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
    const QVariant previous = batch->currentData();
    batch->blockSignals(true);
    batch->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT batch_no,quantity FROM stock_balances WHERE material_id=? AND warehouse_id=? "
        "AND location_id=? AND quantity>0 ORDER BY batch_no"));
    query.addBindValue(material->currentData());
    query.addBindValue(warehouse->currentData());
    query.addBindValue(location->currentData());
    query.exec();
    while (query.next()) {
        const QString value = query.value(0).toString();
        batch->addItem(value.isEmpty() ? QStringLiteral("无批次") : value, value);
    }
    const int selectedIndex = batch->findData(previous);
    if (selectedIndex >= 0) batch->setCurrentIndex(selectedIndex);
    else if (batch->count() > 0) batch->setCurrentIndex(0);
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
    query.addBindValue(batch->currentData().toString());
    double available = 0.0;
    if (query.exec() && query.next()) available = query.value(0).toDouble();
    m_table->item(row, AvailableColumn)->setText(QString::number(available, 'g', 12));

    auto *serialButton = qobject_cast<QPushButton *>(m_table->cellWidget(row, SerialColumn));
    const bool requireSerial = material->currentData(RequireSerialRole).toBool();
    serialButton->setEnabled(requireSerial && available > 0.0);
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
    dialog.setWindowTitle(QStringLiteral("选择本行领用SN"));
    dialog.resize(480, 430);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(QStringLiteral("仅显示所选物料、仓库、库位和批次中的可用SN。"), &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto *list = new QListWidget(&dialog);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    const QStringList previous = button->property("serials").toStringList();
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
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    QStringList serials;
    for (const QListWidgetItem *item : list->selectedItems()) serials.append(item->text());
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
        auto *quantity = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, QuantityColumn));
        auto *serialButton = qobject_cast<QPushButton *>(m_table->cellWidget(row, SerialColumn));
        if (!material || !warehouse || !location || !batch || !quantity
            || material->currentIndex() < 0 || warehouse->currentIndex() < 0
            || location->currentIndex() < 0 || batch->currentIndex() < 0) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 行资料不完整。").arg(row + 1);
            return {};
        }
        const QString identity = QStringLiteral("%1|%2|%3|%4")
                                     .arg(material->currentData().toLongLong())
                                     .arg(warehouse->currentData().toLongLong())
                                     .arg(location->currentData().toLongLong())
                                     .arg(batch->currentData().toString().toUpper());
        if (identities.contains(identity)) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 行与前面明细重复。").arg(row + 1);
            return {};
        }
        identities.insert(identity);
        StockMovementRequest line;
        line.materialId = material->currentData().toLongLong();
        line.quantity = quantity->value();
        line.batchNo = batch->currentData().toString();
        line.warehouseId = warehouse->currentData().toLongLong();
        line.locationId = location->currentData().toLongLong();
        line.serialNumbers = serialButton ? serialButton->property("serials").toStringList() : QStringList();
        if (material->currentData(RequireSerialRole).toBool()) {
            const double roundedQuantity = std::round(line.quantity);
            if (std::abs(line.quantity - roundedQuantity) > 0.0000001
                || line.serialNumbers.size() != static_cast<int>(roundedQuantity)) {
                if (errorMessage) *errorMessage = QStringLiteral("第 %1 行的SN数量必须等于整数领料数量。").arg(row + 1);
                return {};
            }
        }
        result.append(line);
    }
    if (result.isEmpty() && errorMessage) {
        *errorMessage = QStringLiteral("请至少添加一条领料明细。");
    }
    return result;
}
