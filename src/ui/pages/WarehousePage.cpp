#include "ui/pages/WarehousePage.h"

#include <QDialog>
#include <QDialogButtonBox>
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
    const bool canEdit = m_session.isAdministrator();
    m_addWarehouseButton->setEnabled(canEdit);
    m_addLocationButton->setEnabled(canEdit);
    toolbar->addWidget(hint);
    toolbar->addStretch();
    toolbar->addWidget(m_addWarehouseButton);
    toolbar->addWidget(m_addLocationButton);
    root->addLayout(toolbar);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QHBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    m_warehouseTable = new QTableWidget(panel);
    m_warehouseTable->setColumnCount(4);
    m_warehouseTable->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("仓库编码"),
                                                  QStringLiteral("仓库名称"), QStringLiteral("备注")});
    m_locationTable = new QTableWidget(panel);
    m_locationTable->setColumnCount(4);
    m_locationTable->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("库位编码"),
                                                 QStringLiteral("库位名称"), QStringLiteral("备注")});
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
    connect(m_addWarehouseButton, &QPushButton::clicked, this, &WarehousePage::addWarehouse);
    connect(m_addLocationButton, &QPushButton::clicked, this, &WarehousePage::addLocation);
    refresh();
}

void WarehousePage::refresh()
{
    const qlonglong previous = selectedWarehouseId();
    m_warehouseTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("SELECT id, code, name, notes FROM warehouses WHERE is_active=1 ORDER BY code"));
    int selectRow = -1;
    while (query.next()) {
        const int row = m_warehouseTable->rowCount();
        m_warehouseTable->insertRow(row);
        for (int column = 0; column < 4; ++column) {
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
}

void WarehousePage::loadLocations()
{
    m_locationTable->setRowCount(0);
    const qlonglong warehouseId = selectedWarehouseId();
    if (warehouseId <= 0) return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, code, name, notes FROM locations WHERE warehouse_id=? AND is_active=1 ORDER BY code"));
    query.addBindValue(warehouseId);
    query.exec();
    while (query.next()) {
        const int row = m_locationTable->rowCount();
        m_locationTable->insertRow(row);
        for (int column = 0; column < 4; ++column) {
            m_locationTable->setItem(row, column, new QTableWidgetItem(query.value(column).toString()));
        }
    }
    m_locationTable->resizeColumnsToContents();
}

qlonglong WarehousePage::selectedWarehouseId() const
{
    const int row = m_warehouseTable->currentRow();
    if (row < 0 || !m_warehouseTable->item(row, 0)) return 0;
    return m_warehouseTable->item(row, 0)->text().toLongLong();
}

bool WarehousePage::promptCodeAndName(const QString &title, QString *code, QString *name)
{
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.setMinimumWidth(380);
    auto *root = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *codeEdit = new QLineEdit(&dialog);
    auto *nameEdit = new QLineEdit(&dialog);
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
    emit dataChanged();
}
