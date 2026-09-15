#include "ui/pages/WarehousePage.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTabWidget>
#include <QVariant>
#include <QVBoxLayout>

#include <utility>

WarehousePage::WarehousePage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *toolbar = new QHBoxLayout;
    auto *hint = new QLabel(QStringLiteral("仓库 → 库位，两级结构"), this);
    hint->setObjectName(QStringLiteral("mutedText"));
    m_addWarehouseButton = new QPushButton(QStringLiteral("新增仓库"), this);
    m_addWarehouseButton->setProperty("primary", true);
    m_addLocationButton = new QPushButton(QStringLiteral("为所选仓库新增库位"), this);
    m_editWarehouseButton = new QPushButton(QStringLiteral("编辑仓库"), this);
    m_toggleWarehouseButton = new QPushButton(QStringLiteral("停用仓库"), this);
    m_deleteWarehouseButton = new QPushButton(QStringLiteral("删除仓库"), this);
    m_editLocationButton = new QPushButton(QStringLiteral("编辑库位"), this);
    m_toggleLocationButton = new QPushButton(QStringLiteral("停用库位"), this);
    m_deleteLocationButton = new QPushButton(QStringLiteral("删除库位"), this);
    const bool canEdit = m_session.canManageWarehouseData();
    m_addWarehouseButton->setEnabled(canEdit);
    m_addLocationButton->setEnabled(canEdit);
    toolbar->addWidget(hint);
    toolbar->addStretch();
    toolbar->addWidget(m_addWarehouseButton);
    toolbar->addWidget(m_editWarehouseButton);
    toolbar->addWidget(m_toggleWarehouseButton);
    toolbar->addWidget(m_deleteWarehouseButton);
    toolbar->addWidget(m_addLocationButton);
    toolbar->addWidget(m_editLocationButton);
    toolbar->addWidget(m_toggleLocationButton);
    toolbar->addWidget(m_deleteLocationButton);
    root->addLayout(toolbar);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QHBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    m_warehouseTable = new QTableWidget(panel);
    m_warehouseTable->setColumnCount(5);
    m_warehouseTable->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("仓库编码"),
                                                  QStringLiteral("仓库名称"), QStringLiteral("备注"),
                                                  QStringLiteral("状态")});
    m_locationTable = new QTableWidget(panel);
    m_locationTable->setColumnCount(5);
    m_locationTable->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("库位编码"),
                                                 QStringLiteral("库位名称"), QStringLiteral("备注"),
                                                 QStringLiteral("状态")});
    for (QTableWidget *table : {m_warehouseTable, m_locationTable}) {
        table->hideColumn(0);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->hide();
        table->horizontalHeader()->setStretchLastSection(true);
    }
    layout->addWidget(m_warehouseTable, 1);
    layout->addWidget(m_locationTable, 1);
    root->addWidget(panel, 1);

    connect(m_warehouseTable, &QTableWidget::itemSelectionChanged, this, &WarehousePage::loadLocations);
    connect(m_warehouseTable, &QTableWidget::itemSelectionChanged, this, &WarehousePage::updateActions);
    connect(m_locationTable, &QTableWidget::itemSelectionChanged, this, &WarehousePage::updateActions);
    connect(m_addWarehouseButton, &QPushButton::clicked, this, &WarehousePage::addWarehouse);
    connect(m_addLocationButton, &QPushButton::clicked, this, &WarehousePage::addLocation);
    connect(m_editWarehouseButton, &QPushButton::clicked, this, &WarehousePage::editWarehouse);
    connect(m_editLocationButton, &QPushButton::clicked, this, &WarehousePage::editLocation);
    connect(m_toggleWarehouseButton, &QPushButton::clicked, this, &WarehousePage::toggleWarehouse);
    connect(m_toggleLocationButton, &QPushButton::clicked, this, &WarehousePage::toggleLocation);
    connect(m_deleteWarehouseButton, &QPushButton::clicked, this, &WarehousePage::deleteWarehouse);
    connect(m_deleteLocationButton, &QPushButton::clicked, this, &WarehousePage::deleteLocation);
    refresh();
}

void WarehousePage::refresh()
{
    const qlonglong previous = selectedWarehouseId();
    m_warehouseTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("SELECT id, code, name, notes, CASE is_active WHEN 1 THEN '启用' ELSE '停用' END FROM warehouses ORDER BY is_active DESC, code"));
    int selectRow = -1;
    while (query.next()) {
        const int row = m_warehouseTable->rowCount();
        m_warehouseTable->insertRow(row);
        for (int column = 0; column < 5; ++column) {
            m_warehouseTable->setItem(row, column, new QTableWidgetItem(query.value(column).toString()));
        }
        if (query.value(0).toLongLong() == previous) selectRow = row;
    }
    m_warehouseTable->resizeColumnsToContents();
    if (m_warehouseTable->rowCount() > 0) {
        m_warehouseTable->selectRow(selectRow >= 0 ? selectRow : 0);
    } else {
        m_locationTable->setRowCount(0);
    }
    updateActions();
}

void WarehousePage::loadLocations()
{
    m_locationTable->setRowCount(0);
    const qlonglong warehouseId = selectedWarehouseId();
    if (warehouseId <= 0) return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, code, name, notes, CASE is_active WHEN 1 THEN '启用' ELSE '停用' END "
        "FROM locations WHERE warehouse_id=? ORDER BY is_active DESC, code"));
    query.addBindValue(warehouseId);
    query.exec();
    while (query.next()) {
        const int row = m_locationTable->rowCount();
        m_locationTable->insertRow(row);
        for (int column = 0; column < 5; ++column) {
            m_locationTable->setItem(row, column, new QTableWidgetItem(query.value(column).toString()));
        }
    }
    m_locationTable->resizeColumnsToContents();
    updateActions();
}

qlonglong WarehousePage::selectedWarehouseId() const
{
    const int row = m_warehouseTable->currentRow();
    if (row < 0 || !m_warehouseTable->item(row, 0)) return 0;
    return m_warehouseTable->item(row, 0)->text().toLongLong();
}

qlonglong WarehousePage::selectedLocationId() const
{
    const int row = m_locationTable->currentRow();
    if (row < 0 || !m_locationTable->item(row, 0)) return 0;
    return m_locationTable->item(row, 0)->text().toLongLong();
}

bool WarehousePage::promptCodeAndName(const QString &title, QString *code, QString *name,
                                      const QString &initialCode, const QString &initialName)
{
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.setMinimumWidth(380);
    auto *root = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *codeEdit = new QLineEdit(&dialog);
    auto *nameEdit = new QLineEdit(&dialog);
    codeEdit->setText(initialCode);
    nameEdit->setText(initialName);
    form->addRow(QStringLiteral("编码 *"), codeEdit);
    form->addRow(QStringLiteral("名称"), nameEdit);
    root->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return false;
    *code = codeEdit->text().trimmed().toUpper();
    *name = nameEdit->text().trimmed();
    if (code->isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"), QStringLiteral("编码不能为空。"));
        return false;
    }
    return true;
}

void WarehousePage::addWarehouse()
{
    QString code;
    QString name;
    if (!promptCodeAndName(QStringLiteral("新增仓库"), &code, &name)) return;
    if (name.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"), QStringLiteral("仓库名称不能为空。"));
        return;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO warehouses(code, name) VALUES(?, ?)"));
    query.addBindValue(code);
    query.addBindValue(name);
    if (!query.exec()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("仓库编码或名称可能重复。\n%1").arg(query.lastError().text()));
        return;
    }
    refresh();
    writeAudit(QStringLiteral("WAREHOUSE_CREATE"), QStringLiteral("warehouse"),
               query.lastInsertId().toLongLong(), code + QStringLiteral(" - ") + name);
    emit dataChanged();
}

void WarehousePage::addLocation()
{
    const qlonglong warehouseId = selectedWarehouseId();
    if (warehouseId <= 0) {
        QMessageBox::information(this, QStringLiteral("请选择仓库"), QStringLiteral("请先选择一个仓库。"));
        return;
    }
    QString code;
    QString name;
    if (!promptCodeAndName(QStringLiteral("新增库位"), &code, &name)) return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO locations(warehouse_id, code, name) VALUES(?, ?, ?)"));
    query.addBindValue(warehouseId);
    query.addBindValue(code);
    query.addBindValue(name);
    if (!query.exec()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("同一仓库内的库位编码不能重复。\n%1").arg(query.lastError().text()));
        return;
    }
    loadLocations();
    writeAudit(QStringLiteral("LOCATION_CREATE"), QStringLiteral("location"),
               query.lastInsertId().toLongLong(), code + QStringLiteral(" - ") + name);
    emit dataChanged();
}

void WarehousePage::editWarehouse()
{
    const int row = m_warehouseTable->currentRow();
    if (row < 0) return;
    QString code;
    QString name;
    if (!promptCodeAndName(QStringLiteral("编辑仓库"), &code, &name,
                           m_warehouseTable->item(row, 1)->text(),
                           m_warehouseTable->item(row, 2)->text())) return;
    if (name.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"), QStringLiteral("仓库名称不能为空。"));
        return;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE warehouses SET code=?,name=?,updated_at=? WHERE id=?"));
    query.addBindValue(code);
    query.addBindValue(name);
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    query.addBindValue(selectedWarehouseId());
    if (!query.exec()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), query.lastError().text());
        return;
    }
    writeAudit(QStringLiteral("WAREHOUSE_UPDATE"), QStringLiteral("warehouse"),
               selectedWarehouseId(), code + QStringLiteral(" - ") + name);
    refresh();
    emit dataChanged();
}

void WarehousePage::editLocation()
{
    const int row = m_locationTable->currentRow();
    if (row < 0) return;
    const qlonglong locationId = selectedLocationId();
    QString code;
    QString name;
    if (!promptCodeAndName(QStringLiteral("编辑库位"), &code, &name,
                           m_locationTable->item(row, 1)->text(),
                           m_locationTable->item(row, 2)->text())) return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE locations SET code=?,name=?,updated_at=? WHERE id=?"));
    query.addBindValue(code);
    query.addBindValue(name);
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    query.addBindValue(locationId);
    if (!query.exec()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), query.lastError().text());
        return;
    }
    writeAudit(QStringLiteral("LOCATION_UPDATE"), QStringLiteral("location"), locationId,
               code + QStringLiteral(" - ") + name);
    loadLocations();
    emit dataChanged();
}

void WarehousePage::toggleWarehouse()
{
    const int row = m_warehouseTable->currentRow();
    if (row < 0) return;
    const qlonglong id = selectedWarehouseId();
    const bool active = m_warehouseTable->item(row, 4)->text() == QStringLiteral("启用");
    if (active) {
        QSqlQuery stock(m_database);
        stock.prepare(QStringLiteral("SELECT COALESCE(SUM(quantity),0) FROM stock_balances WHERE warehouse_id=?"));
        stock.addBindValue(id);
        if (stock.exec() && stock.next() && stock.value(0).toDouble() > 0.0000001) {
            QMessageBox::warning(this, QStringLiteral("无法停用"), QStringLiteral("该仓库仍有库存，请先调拨或出库。"));
            return;
        }
    }
    if (QMessageBox::question(this, active ? QStringLiteral("确认停用") : QStringLiteral("确认启用"),
        active ? QStringLiteral("停用仓库时将同时停用其全部库位，是否继续？")
               : QStringLiteral("是否启用该仓库？库位需按需单独启用。")) != QMessageBox::Yes) return;
    if (!m_database.transaction()) return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE warehouses SET is_active=?,updated_at=? WHERE id=?"));
    query.addBindValue(!active);
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    query.addBindValue(id);
    bool ok = query.exec();
    if (ok && active) {
        query.prepare(QStringLiteral("UPDATE locations SET is_active=0,updated_at=? WHERE warehouse_id=?"));
        query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        query.addBindValue(id);
        ok = query.exec();
    }
    if (!ok || !m_database.commit()) {
        m_database.rollback();
        QMessageBox::warning(this, QStringLiteral("操作失败"), query.lastError().text());
        return;
    }
    writeAudit(active ? QStringLiteral("WAREHOUSE_DISABLE") : QStringLiteral("WAREHOUSE_ENABLE"),
               QStringLiteral("warehouse"), id, m_warehouseTable->item(row, 1)->text());
    refresh();
    emit dataChanged();
}

void WarehousePage::toggleLocation()
{
    const int row = m_locationTable->currentRow();
    if (row < 0) return;
    const qlonglong id = selectedLocationId();
    const bool active = m_locationTable->item(row, 4)->text() == QStringLiteral("启用");
    if (active) {
        QSqlQuery stock(m_database);
        stock.prepare(QStringLiteral("SELECT COALESCE(SUM(quantity),0) FROM stock_balances WHERE location_id=?"));
        stock.addBindValue(id);
        if (stock.exec() && stock.next() && stock.value(0).toDouble() > 0.0000001) {
            QMessageBox::warning(this, QStringLiteral("无法停用"), QStringLiteral("该库位仍有库存，请先调拨或出库。"));
            return;
        }
    } else if (m_warehouseTable->item(m_warehouseTable->currentRow(), 4)->text() != QStringLiteral("启用")) {
        QMessageBox::warning(this, QStringLiteral("无法启用"), QStringLiteral("请先启用所属仓库。"));
        return;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE locations SET is_active=?,updated_at=? WHERE id=?"));
    query.addBindValue(!active);
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    query.addBindValue(id);
    if (!query.exec()) {
        QMessageBox::warning(this, QStringLiteral("操作失败"), query.lastError().text());
        return;
    }
    writeAudit(active ? QStringLiteral("LOCATION_DISABLE") : QStringLiteral("LOCATION_ENABLE"),
               QStringLiteral("location"), id, m_locationTable->item(row, 1)->text());
    loadLocations();
    emit dataChanged();
}

void WarehousePage::deleteWarehouse()
{
    const int row = m_warehouseTable->currentRow();
    const qlonglong warehouseId = selectedWarehouseId();
    if (row < 0 || warehouseId <= 0) return;
    const QString code = m_warehouseTable->item(row, 1)->text();
    const QString name = m_warehouseTable->item(row, 2)->text();
    if (QMessageBox::warning(
            this, QStringLiteral("确认删除仓库"),
            QStringLiteral("确定永久删除仓库“%1 - %2”及其全部库位吗？\n\n"
                           "该仓库现有库存将被清除，在库 SN 将作废；历史单据会保留，"
                           "但其中的仓库、库位引用以及相关库存流水和盘点明细将被移除。\n\n"
                           "此操作不可撤销。")
                .arg(code, name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    if (!m_database.transaction()) {
        QMessageBox::warning(this, QStringLiteral("删除失败"), m_database.lastError().text());
        return;
    }
    QSqlQuery query(m_database);
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    auto execute = [&query](const QString &sql, const QVariantList &values = {}) {
        query.prepare(sql);
        for (const QVariant &value : values) query.addBindValue(value);
        return query.exec();
    };

    bool ok = execute(QStringLiteral(
        "UPDATE inventory_ledger SET reversal_of_ledger_id=NULL "
        "WHERE reversal_of_ledger_id IN ("
        "SELECT id FROM inventory_ledger WHERE warehouse_id=? "
        "OR location_id IN (SELECT id FROM locations WHERE warehouse_id=?))"),
        {warehouseId, warehouseId});
    if (ok) {
        ok = execute(QStringLiteral(
            "DELETE FROM inventory_ledger_serials WHERE ledger_id IN ("
            "SELECT id FROM inventory_ledger WHERE warehouse_id=? "
            "OR location_id IN (SELECT id FROM locations WHERE warehouse_id=?))"),
            {warehouseId, warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "DELETE FROM inventory_ledger WHERE warehouse_id=? "
            "OR location_id IN (SELECT id FROM locations WHERE warehouse_id=?)"),
            {warehouseId, warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "DELETE FROM inventory_count_items WHERE warehouse_id=? "
            "OR location_id IN (SELECT id FROM locations WHERE warehouse_id=?)"),
            {warehouseId, warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "UPDATE serial_numbers SET "
            "status=CASE WHEN status='IN_STOCK' THEN 'VOIDED' ELSE status END,"
            "warehouse_id=NULL,location_id=NULL "
            "WHERE warehouse_id=? OR location_id IN ("
            "SELECT id FROM locations WHERE warehouse_id=?)"),
            {warehouseId, warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "UPDATE business_document_items SET location_id=NULL "
            "WHERE location_id IN (SELECT id FROM locations WHERE warehouse_id=?)"),
            {warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "UPDATE business_document_items SET target_location_id=NULL "
            "WHERE target_location_id IN (SELECT id FROM locations WHERE warehouse_id=?)"),
            {warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "UPDATE business_document_items SET warehouse_id=NULL WHERE warehouse_id=?"),
            {warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "UPDATE business_document_items SET target_warehouse_id=NULL WHERE target_warehouse_id=?"),
            {warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "DELETE FROM stock_balances WHERE warehouse_id=? "
            "OR location_id IN (SELECT id FROM locations WHERE warehouse_id=?)"),
            {warehouseId, warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "UPDATE materials SET default_location_id=NULL,updated_at=? "
            "WHERE default_location_id IN (SELECT id FROM locations WHERE warehouse_id=?)"),
            {now, warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "UPDATE materials SET default_warehouse_id=NULL,updated_at=? WHERE default_warehouse_id=?"),
            {now, warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral("DELETE FROM locations WHERE warehouse_id=?"), {warehouseId});
    }
    if (ok) {
        ok = execute(QStringLiteral("DELETE FROM warehouses WHERE id=?"), {warehouseId})
            && query.numRowsAffected() == 1;
    }
    const QString databaseError = query.lastError().text();
    if (ok) {
        ok = writeAudit(QStringLiteral("WAREHOUSE_DELETE"), QStringLiteral("warehouse"),
                        warehouseId, code + QStringLiteral(" - ") + name);
    }
    if (!ok || !m_database.commit()) {
        m_database.rollback();
        QMessageBox::warning(
            this, QStringLiteral("无法删除仓库"),
            QStringLiteral("删除仓库时处理关联库存或历史资料失败，所有修改已回滚。\n\n%1")
                .arg(databaseError));
        return;
    }
    refresh();
    emit dataChanged();
}

void WarehousePage::deleteLocation()
{
    const int row = m_locationTable->currentRow();
    const qlonglong locationId = selectedLocationId();
    if (row < 0 || locationId <= 0) return;
    const QString code = m_locationTable->item(row, 1)->text();
    const QString name = m_locationTable->item(row, 2)->text();
    if (QMessageBox::warning(
            this, QStringLiteral("确认删除库位"),
            QStringLiteral("确定永久删除库位“%1 - %2”吗？\n\n"
                           "该库位现有库存将被清除，在库 SN 将作废；历史单据会保留，"
                           "但其中的库位引用以及相关库存流水和盘点明细将被移除。\n\n"
                           "此操作不可撤销。")
                .arg(code, name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    if (!m_database.transaction()) {
        QMessageBox::warning(this, QStringLiteral("删除失败"), m_database.lastError().text());
        return;
    }
    QSqlQuery query(m_database);
    auto execute = [&query](const QString &sql, const QVariantList &values = {}) {
        query.prepare(sql);
        for (const QVariant &value : values) query.addBindValue(value);
        return query.exec();
    };

    bool ok = execute(QStringLiteral(
        "UPDATE inventory_ledger SET reversal_of_ledger_id=NULL "
        "WHERE reversal_of_ledger_id IN (SELECT id FROM inventory_ledger WHERE location_id=?)"),
        {locationId});
    if (ok) {
        ok = execute(QStringLiteral(
            "DELETE FROM inventory_ledger_serials WHERE ledger_id IN ("
            "SELECT id FROM inventory_ledger WHERE location_id=?)"), {locationId});
    }
    if (ok) {
        ok = execute(QStringLiteral("DELETE FROM inventory_ledger WHERE location_id=?"), {locationId});
    }
    if (ok) {
        ok = execute(QStringLiteral("DELETE FROM inventory_count_items WHERE location_id=?"), {locationId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "UPDATE serial_numbers SET "
            "status=CASE WHEN status='IN_STOCK' THEN 'VOIDED' ELSE status END,"
            "warehouse_id=NULL,location_id=NULL WHERE location_id=?"), {locationId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "UPDATE business_document_items SET location_id=NULL WHERE location_id=?"), {locationId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "UPDATE business_document_items SET target_location_id=NULL WHERE target_location_id=?"),
            {locationId});
    }
    if (ok) {
        ok = execute(QStringLiteral("DELETE FROM stock_balances WHERE location_id=?"), {locationId});
    }
    if (ok) {
        ok = execute(QStringLiteral(
            "UPDATE materials SET default_location_id=NULL,updated_at=? WHERE default_location_id=?"),
            {QDateTime::currentDateTime().toString(Qt::ISODateWithMs), locationId});
    }
    if (ok) {
        ok = execute(QStringLiteral("DELETE FROM locations WHERE id=?"), {locationId})
            && query.numRowsAffected() == 1;
    }
    const QString databaseError = query.lastError().text();
    if (ok) {
        ok = writeAudit(QStringLiteral("LOCATION_DELETE"), QStringLiteral("location"),
                        locationId, code + QStringLiteral(" - ") + name);
    }
    if (!ok || !m_database.commit()) {
        m_database.rollback();
        QMessageBox::warning(
            this, QStringLiteral("无法删除库位"),
            QStringLiteral("删除库位时处理关联库存或历史资料失败，所有修改已回滚。\n\n%1")
                .arg(databaseError));
        return;
    }
    loadLocations();
    emit dataChanged();
}

void WarehousePage::updateActions()
{
    const bool allowed = m_session.canManageWarehouseData();
    const int warehouseRow = m_warehouseTable->currentRow();
    const int locationRow = m_locationTable->currentRow();
    m_editWarehouseButton->setEnabled(allowed && warehouseRow >= 0);
    m_toggleWarehouseButton->setEnabled(allowed && warehouseRow >= 0);
    m_deleteWarehouseButton->setEnabled(allowed && warehouseRow >= 0);
    m_addLocationButton->setEnabled(allowed && warehouseRow >= 0
        && m_warehouseTable->item(warehouseRow, 4)->text() == QStringLiteral("启用"));
    m_editLocationButton->setEnabled(allowed && locationRow >= 0);
    m_toggleLocationButton->setEnabled(allowed && locationRow >= 0);
    m_deleteLocationButton->setEnabled(allowed && locationRow >= 0);
    if (warehouseRow >= 0) m_toggleWarehouseButton->setText(
        m_warehouseTable->item(warehouseRow, 4)->text() == QStringLiteral("启用")
            ? QStringLiteral("停用仓库") : QStringLiteral("启用仓库"));
    if (locationRow >= 0) m_toggleLocationButton->setText(
        m_locationTable->item(locationRow, 4)->text() == QStringLiteral("启用")
            ? QStringLiteral("停用库位") : QStringLiteral("启用库位"));
}

bool WarehousePage::writeAudit(const QString &action, const QString &entityType,
                               qlonglong entityId, const QString &detail)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) VALUES(?,?,?,?,?)"));
    query.addBindValue(m_session.userId);
    query.addBindValue(action);
    query.addBindValue(entityType);
    query.addBindValue(entityId);
    query.addBindValue(detail);
    return query.exec();
}
