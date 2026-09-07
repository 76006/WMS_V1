#include "ui/pages/InventoryCountPage.h"

#include "services/InventoryService.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace {
constexpr int MaterialIdRole = Qt::UserRole + 1;
constexpr int WarehouseIdRole = Qt::UserRole + 2;
constexpr int LocationIdRole = Qt::UserRole + 3;
constexpr int RequireSerialRole = Qt::UserRole + 4;
constexpr int SystemQuantityRole = Qt::UserRole + 5;
}

InventoryCountPage::InventoryCountPage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(20, 18, 20, 20);
    auto *heading = new QLabel(QStringLiteral("库存盘点"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    layout->addWidget(heading);
    auto *hint = new QLabel(QStringLiteral(
        "加载当前库存快照后填写实盘数。存在差异必须填写原因；SN物料有差异时请先通过出入库调整SN。"), panel);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("mutedText"));
    layout->addWidget(hint);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_warehouseCombo = new QComboBox(panel);
    m_locationCombo = new QComboBox(panel);
    m_dateEdit = new QDateEdit(QDate::currentDate(), panel);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_handlerEdit = new QLineEdit(m_session.displayName, panel);
    m_notesEdit = new QTextEdit(panel);
    m_notesEdit->setMaximumHeight(60);
    form->addRow(QStringLiteral("盘点仓库 *"), m_warehouseCombo);
    form->addRow(QStringLiteral("盘点库位"), m_locationCombo);
    form->addRow(QStringLiteral("盘点日期 *"), m_dateEdit);
    form->addRow(QStringLiteral("盘点人员"), m_handlerEdit);
    form->addRow(QStringLiteral("备注"), m_notesEdit);
    layout->addLayout(form);

    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(new QLabel(QStringLiteral("盘点明细"), panel));
    toolbar->addStretch();
    auto *reload = new QPushButton(QStringLiteral("重新加载库存"), panel);
    toolbar->addWidget(reload);
    layout->addLayout(toolbar);
    m_table = new QTableWidget(0, 9, panel);
    m_table->setHorizontalHeaderLabels({QStringLiteral("物料"), QStringLiteral("仓库"),
                                        QStringLiteral("库位"), QStringLiteral("批次"),
                                        QStringLiteral("SN管理"), QStringLiteral("账面数"),
                                        QStringLiteral("实盘数"), QStringLiteral("差异"),
                                        QStringLiteral("差异原因")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);
    layout->addWidget(m_table, 1);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    m_submitButton = new QPushButton(QStringLiteral("确认盘点并调整库存"), panel);
    m_submitButton->setProperty("primary", true);
    actions->addWidget(m_submitButton);
    layout->addLayout(actions);
    root->addWidget(panel, 1);

    connect(m_warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &InventoryCountPage::loadLocations);
    connect(m_locationCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &InventoryCountPage::loadSnapshot);
    connect(reload, &QPushButton::clicked, this, &InventoryCountPage::loadSnapshot);
    connect(m_submitButton, &QPushButton::clicked, this, &InventoryCountPage::submit);
    resetSubmissionToken();
    refreshReferenceData();
}

void InventoryCountPage::resetSubmissionToken()
{
    m_submissionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void InventoryCountPage::refreshReferenceData()
{
    const QVariant previous = m_warehouseCombo->currentData();
    m_warehouseCombo->blockSignals(true);
    m_warehouseCombo->clear();
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("SELECT id,code,name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (query.next())
        m_warehouseCombo->addItem(QStringLiteral("%1 - %2").arg(query.value(1).toString(),
                                                                 query.value(2).toString()),
                                  query.value(0));
    const int selected = m_warehouseCombo->findData(previous);
    if (selected >= 0) m_warehouseCombo->setCurrentIndex(selected);
    m_warehouseCombo->blockSignals(false);
    loadLocations();
    m_submitButton->setEnabled(m_session.canManageWarehouse());
}

void InventoryCountPage::loadLocations()
{
    const QVariant previous = m_locationCombo->currentData();
    m_locationCombo->blockSignals(true);
    m_locationCombo->clear();
    m_locationCombo->addItem(QStringLiteral("全部库位"), QVariant());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id,code,name FROM locations WHERE warehouse_id=? AND is_active=1 ORDER BY code"));
    query.addBindValue(m_warehouseCombo->currentData());
    query.exec();
    while (query.next())
        m_locationCombo->addItem(QStringLiteral("%1 - %2").arg(query.value(1).toString(),
                                                                query.value(2).toString()),
                                 query.value(0));
    const int selected = m_locationCombo->findData(previous);
    if (selected >= 0) m_locationCombo->setCurrentIndex(selected);
    m_locationCombo->blockSignals(false);
    loadSnapshot();
}

void InventoryCountPage::loadSnapshot()
{
    m_table->setRowCount(0);
    if (m_warehouseCombo->currentIndex() < 0) return;
    QString sql = QStringLiteral(
        "SELECT m.id,m.code,m.name,w.id,w.code,l.id,l.code,s.batch_no,s.quantity,m.require_serial "
        "FROM stock_balances s JOIN materials m ON m.id=s.material_id "
        "JOIN warehouses w ON w.id=s.warehouse_id JOIN locations l ON l.id=s.location_id "
        "WHERE s.warehouse_id=? AND s.quantity>0");
    if (m_locationCombo->currentData().isValid()) sql += QStringLiteral(" AND s.location_id=?");
    sql += QStringLiteral(" ORDER BY m.code,l.code,s.batch_no");
    QSqlQuery query(m_database);
    query.prepare(sql);
    query.addBindValue(m_warehouseCombo->currentData());
    if (m_locationCombo->currentData().isValid()) query.addBindValue(m_locationCombo->currentData());
    query.exec();
    while (query.next()) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *material = new QTableWidgetItem(QStringLiteral("%1 - %2")
                                                  .arg(query.value(1).toString(),
                                                       query.value(2).toString()));
        material->setData(MaterialIdRole, query.value(0));
        material->setData(WarehouseIdRole, query.value(3));
        material->setData(LocationIdRole, query.value(5));
        material->setData(RequireSerialRole, query.value(9));
        material->setData(SystemQuantityRole, query.value(8));
        m_table->setItem(row, 0, material);
        m_table->setItem(row, 1, new QTableWidgetItem(query.value(4).toString()));
        m_table->setItem(row, 2, new QTableWidgetItem(query.value(6).toString()));
        m_table->setItem(row, 3, new QTableWidgetItem(query.value(7).toString()));
        m_table->setItem(row, 4, new QTableWidgetItem(query.value(9).toBool()
                                                          ? QStringLiteral("是") : QStringLiteral("否")));
        m_table->setItem(row, 5, new QTableWidgetItem(query.value(8).toString()));
        auto *actual = new QDoubleSpinBox(m_table);
        actual->setDecimals(6);
        actual->setRange(0.0, 999999999999.0);
        actual->setValue(query.value(8).toDouble());
        m_table->setCellWidget(row, 6, actual);
        auto *difference = new QTableWidgetItem(QStringLiteral("0"));
        m_table->setItem(row, 7, difference);
        auto *reason = new QLineEdit(m_table);
        reason->setPlaceholderText(QStringLiteral("有差异时必填"));
        m_table->setCellWidget(row, 8, reason);
        connect(actual, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [material, difference](double value) {
                    difference->setText(QString::number(
                        value - material->data(SystemQuantityRole).toDouble(), 'g', 12));
                });
    }
    m_submitButton->setEnabled(m_session.canManageWarehouse() && m_table->rowCount() > 0);
}

void InventoryCountPage::submit()
{
    if (m_table->rowCount() == 0) {
        QMessageBox::warning(this, QStringLiteral("没有库存"), QStringLiteral("当前范围没有可盘点库存。"));
        return;
    }
    InventoryCountRequest request;
    request.documentDate = m_dateEdit->date();
    request.handlerName = m_handlerEdit->text().trimmed();
    request.notes = m_notesEdit->toPlainText().trimmed();
    request.submissionToken = m_submissionToken;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *material = m_table->item(row, 0);
        auto *actual = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, 6));
        auto *reason = qobject_cast<QLineEdit *>(m_table->cellWidget(row, 8));
        InventoryCountLine line;
        line.materialId = material->data(MaterialIdRole).toLongLong();
        line.warehouseId = material->data(WarehouseIdRole).toLongLong();
        line.locationId = material->data(LocationIdRole).toLongLong();
        line.batchNo = m_table->item(row, 3)->text();
        line.systemQuantity = material->data(SystemQuantityRole).toDouble();
        line.actualQuantity = actual->value();
        line.differenceReason = reason->text().trimmed();
        request.lines.append(line);
    }
    if (QMessageBox::question(this, QStringLiteral("确认盘点"),
        QStringLiteral("确认提交 %1 条盘点明细？差异库存将立即调整并生成流水。")
            .arg(request.lines.size())) != QMessageBox::Yes) return;
    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    QString error;
    if (!service.postInventoryCount(request, &posted, &error)) {
        QMessageBox::warning(this, QStringLiteral("盘点失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("盘点完成"),
                             QStringLiteral("盘点单 %1 已确认。").arg(posted.documentNumber));
    resetSubmissionToken();
    m_notesEdit->clear();
    emit stockChanged();
    loadSnapshot();
}
