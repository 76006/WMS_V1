#include "ui/pages/InventoryCountPage.h"

#include "services/InventoryService.h"
#include "ui/widgets/ComboBoxSearch.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
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
constexpr int CodeRole = Qt::UserRole + 6;
constexpr int RequireBatchRole = Qt::UserRole + 7;
constexpr double CountQuantityTolerance = 0.0000001;

QString countIdentity(qlonglong materialId,
                      qlonglong warehouseId,
                      qlonglong locationId,
                      const QString &batchNo)
{
    return QStringLiteral("%1|%2|%3|%4")
        .arg(materialId)
        .arg(warehouseId)
        .arg(locationId)
        .arg(batchNo.trimmed().toUpper());
}
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
        "加载当前库存快照后填写实盘数：快照包含账面为0但仍有结存的库存行。"
        "账面无记录的实物盘盈请点击“添加盘盈物料”。"
        "存在差异必须填写原因；SN物料有差异时请先通过出入库调整SN。"), panel);
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
    m_materialSearchEdit = new QLineEdit(panel);
    m_materialSearchEdit->setPlaceholderText(QStringLiteral("按物料编码或名称检索"));
    m_materialSearchEdit->setClearButtonEnabled(true);
    m_materialSearchEdit->setMinimumWidth(260);
    toolbar->addWidget(m_materialSearchEdit);
    toolbar->addStretch();
    auto *addGain = new QPushButton(QStringLiteral("添加盘盈物料"), panel);
    addGain->setToolTip(QStringLiteral("为账面无库存记录、实物盘点发现的物料补录盘盈明细"));
    toolbar->addWidget(addGain);
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
    connect(addGain, &QPushButton::clicked, this, &InventoryCountPage::addGainMaterial);
    connect(reload, &QPushButton::clicked, this, &InventoryCountPage::loadSnapshot);
    connect(m_materialSearchEdit, &QLineEdit::textChanged,
            this, &InventoryCountPage::filterMaterials);
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
    while (query.next()) {
        m_warehouseCombo->addItem(QStringLiteral("%1 - %2").arg(query.value(1).toString(),
                                                                 query.value(2).toString()),
                                  query.value(0));
        m_warehouseCombo->setItemData(m_warehouseCombo->count() - 1, query.value(1), CodeRole);
    }
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
    while (query.next()) {
        m_locationCombo->addItem(QStringLiteral("%1 - %2").arg(query.value(1).toString(),
                                                                query.value(2).toString()),
                                 query.value(0));
        m_locationCombo->setItemData(m_locationCombo->count() - 1, query.value(1), CodeRole);
    }
    const int selected = m_locationCombo->findData(previous);
    if (selected >= 0) m_locationCombo->setCurrentIndex(selected);
    m_locationCombo->blockSignals(false);
    loadSnapshot();
}

int InventoryCountPage::appendCountRow(qlonglong materialId,
                                       const QString &materialText,
                                       qlonglong warehouseId,
                                       const QString &warehouseCode,
                                       qlonglong locationId,
                                       const QString &locationCode,
                                       const QString &batchNo,
                                       bool requireSerial,
                                       double systemQuantity,
                                       double actualQuantity,
                                       const QString &differenceReason)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    auto *material = new QTableWidgetItem(materialText);
    material->setData(MaterialIdRole, materialId);
    material->setData(WarehouseIdRole, warehouseId);
    material->setData(LocationIdRole, locationId);
    material->setData(RequireSerialRole, requireSerial);
    material->setData(SystemQuantityRole, systemQuantity);
    m_table->setItem(row, 0, material);
    m_table->setItem(row, 1, new QTableWidgetItem(warehouseCode));
    m_table->setItem(row, 2, new QTableWidgetItem(locationCode));
    m_table->setItem(row, 3, new QTableWidgetItem(batchNo));
    m_table->setItem(row, 4, new QTableWidgetItem(requireSerial
                                                      ? QStringLiteral("是") : QStringLiteral("否")));
    m_table->setItem(row, 5, new QTableWidgetItem(QString::number(systemQuantity, 'g', 12)));
    auto *actual = new QDoubleSpinBox(m_table);
    actual->setDecimals(6);
    actual->setRange(0.0, 999999999999.0);
    actual->setValue(actualQuantity);
    m_table->setCellWidget(row, 6, actual);
    auto *difference = new QTableWidgetItem(
        QString::number(actualQuantity - systemQuantity, 'g', 12));
    m_table->setItem(row, 7, difference);
    auto *reason = new QLineEdit(m_table);
    reason->setPlaceholderText(QStringLiteral("有差异时必填"));
    reason->setText(differenceReason);
    m_table->setCellWidget(row, 8, reason);
    connect(actual, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [material, difference](double value) {
                difference->setText(QString::number(
                    value - material->data(SystemQuantityRole).toDouble(), 'g', 12));
            });
    return row;
}

void InventoryCountPage::loadSnapshot()
{
    m_table->setRowCount(0);
    if (m_warehouseCombo->currentIndex() < 0) return;
    // 账面为0但仍有结存记录的物料同样需要盘点，仅排除异常负数结存。
    QString sql = QStringLiteral(
        "SELECT m.id,m.code,m.name,w.id,w.code,l.id,l.code,s.batch_no,s.quantity,m.require_serial "
        "FROM stock_balances s JOIN materials m ON m.id=s.material_id "
        "JOIN warehouses w ON w.id=s.warehouse_id JOIN locations l ON l.id=s.location_id "
        "WHERE s.warehouse_id=? AND s.quantity>=0");
    if (m_locationCombo->currentData().isValid()) sql += QStringLiteral(" AND s.location_id=?");
    sql += QStringLiteral(" ORDER BY m.code,l.code,s.batch_no");
    QSqlQuery query(m_database);
    query.prepare(sql);
    query.addBindValue(m_warehouseCombo->currentData());
    if (m_locationCombo->currentData().isValid()) query.addBindValue(m_locationCombo->currentData());
    query.exec();
    while (query.next()) {
        const double systemQuantity = query.value(8).toDouble();
        appendCountRow(query.value(0).toLongLong(),
                       QStringLiteral("%1 - %2").arg(query.value(1).toString(),
                                                     query.value(2).toString()),
                       query.value(3).toLongLong(), query.value(4).toString(),
                       query.value(5).toLongLong(), query.value(6).toString(),
                       query.value(7).toString(), query.value(9).toBool(),
                       systemQuantity, systemQuantity, QString());
    }
    m_submitButton->setEnabled(m_session.canManageWarehouse() && m_table->rowCount() > 0);
    filterMaterials();
}

void InventoryCountPage::filterMaterials()
{
    const QString keyword = m_materialSearchEdit->text().trimmed();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *material = m_table->item(row, 0);
        m_table->setRowHidden(row, material && !keyword.isEmpty()
            && !material->text().contains(keyword, Qt::CaseInsensitive));
    }
}

void InventoryCountPage::addGainMaterial()
{
    if (m_warehouseCombo->currentIndex() < 0) {
        QMessageBox::warning(this, QStringLiteral("缺少仓库"), QStringLiteral("请先选择盘点仓库。"));
        return;
    }
    const qlonglong warehouseId = m_warehouseCombo->currentData().toLongLong();
    const QString warehouseCode = m_warehouseCombo->currentData(CodeRole).toString();
    const QVariant locationFilter = m_locationCombo->currentData();

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("添加盘盈物料"));
    dialog.setMinimumWidth(560);
    auto *root = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(QStringLiteral(
        "用于账面无库存记录、但实物盘点发现的物料。添加后账面数为0，实盘数量作为盘盈差异处理。"), &dialog);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("mutedText"));
    root->addWidget(hint);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto *materialCombo = new QComboBox(&dialog);
    ComboBoxSearch::enableContainsSearch(
        materialCombo, QStringLiteral("输入物料编码或名称检索"));
    // 盘盈属于高风险库存调整，必须显式选择物料，绝不回退到第一个物料。
    materialCombo->addItem(QStringLiteral("请选择物料"), QVariant(qlonglong(0)));
    QSqlQuery materialQuery(m_database);
    if (!materialQuery.exec(QStringLiteral(
            "SELECT id,code,name,require_batch,require_serial FROM materials "
            "WHERE is_active=1 ORDER BY code"))) {
        QMessageBox::warning(this, QStringLiteral("读取物料失败"),
                             materialQuery.lastError().text());
        return;
    }
    while (materialQuery.next()) {
        materialCombo->addItem(QStringLiteral("%1 - %2").arg(materialQuery.value(1).toString(),
                                                             materialQuery.value(2).toString()),
                               materialQuery.value(0));
        const int index = materialCombo->count() - 1;
        materialCombo->setItemData(index, materialQuery.value(3), RequireBatchRole);
        materialCombo->setItemData(index, materialQuery.value(4), RequireSerialRole);
    }
    // 第0项是占位项，因此必须至少有两条才存在真实物料。
    if (materialCombo->count() <= 1) {
        QMessageBox::warning(this, QStringLiteral("没有可用物料"),
                             QStringLiteral("当前没有启用的物料，无法添加盘盈。"));
        return;
    }
    auto *locationCombo = new QComboBox(&dialog);
    QSqlQuery locationQuery(m_database);
    locationQuery.prepare(QStringLiteral(
        "SELECT id,code,name FROM locations WHERE warehouse_id=? AND is_active=1 "
        "AND (? IS NULL OR id=?) ORDER BY code"));
    locationQuery.addBindValue(warehouseId);
    locationQuery.addBindValue(locationFilter);
    locationQuery.addBindValue(locationFilter);
    if (!locationQuery.exec()) {
        QMessageBox::warning(this, QStringLiteral("读取库位失败"), locationQuery.lastError().text());
        return;
    }
    while (locationQuery.next()) {
        locationCombo->addItem(QStringLiteral("%1 - %2").arg(locationQuery.value(1).toString(),
                                                             locationQuery.value(2).toString()),
                               locationQuery.value(0));
        locationCombo->setItemData(locationCombo->count() - 1, locationQuery.value(1), CodeRole);
    }
    if (locationCombo->count() == 0) {
        QMessageBox::warning(this, QStringLiteral("没有可用库位"),
                             QStringLiteral("所选仓库没有启用的库位，无法添加盘盈。"));
        return;
    }
    if (locationFilter.isValid()) {
        locationCombo->setEnabled(false);
        locationCombo->setToolTip(QStringLiteral("当前按库位筛选，只能添加该库位的盘盈物料"));
    }
    auto *batchEdit = new QLineEdit(&dialog);
    batchEdit->setPlaceholderText(QStringLiteral("批次管理物料必填"));
    auto *quantitySpin = new QDoubleSpinBox(&dialog);
    quantitySpin->setDecimals(6);
    quantitySpin->setRange(0.0, 999999999999.0);
    // 不预填1，避免误操作直接产生盘盈差异；必须由操作员显式填写。
    quantitySpin->setValue(0.0);
    form->addRow(QStringLiteral("物料 *"), materialCombo);
    form->addRow(QStringLiteral("库位 *"), locationCombo);
    form->addRow(QStringLiteral("批次"), batchEdit);
    form->addRow(QStringLiteral("盘盈数量 *"), quantitySpin);
    root->addLayout(form);
    auto *serialHint = new QLabel(QStringLiteral(
        "SN管理物料不能通过盘点盘盈，请使用入库单正常入库并登记SN。"), &dialog);
    serialHint->setWordWrap(true);
    serialHint->setObjectName(QStringLiteral("mutedText"));
    root->addWidget(serialHint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("添加到盘点明细"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        // 可编辑下拉框允许输入检索词，这里要求最终文本能对应到一条实际物料。
        int materialIndex = materialCombo->currentIndex();
        if (materialIndex < 0
            || materialCombo->currentText().trimmed()
                   != materialCombo->itemText(materialIndex).trimmed()) {
            materialIndex = materialCombo->findText(materialCombo->currentText().trimmed(),
                                                    Qt::MatchFixedString);
        }
        // 占位项（id=0）以及只输入文字但没有对应真实物料的情况都必须拒绝。
        const qlonglong materialId = materialIndex < 0
            ? 0 : materialCombo->itemData(materialIndex).toLongLong();
        if (materialId <= 0) {
            QMessageBox::warning(&dialog, QStringLiteral("缺少物料"),
                                 QStringLiteral("请从下拉列表中检索并选择一条启用状态的物料。"));
            return;
        }
        const bool requireBatch = materialCombo->itemData(materialIndex, RequireBatchRole).toBool();
        const bool requireSerial = materialCombo->itemData(materialIndex, RequireSerialRole).toBool();
        if (requireSerial) {
            QMessageBox::warning(
                &dialog, QStringLiteral("SN管理物料"),
                QStringLiteral("SN管理物料必须通过入库单登记SN，不能使用盘点盘盈；"
                               "请改用正常入库业务并填写SN。"));
            return;
        }
        const qlonglong locationId = locationCombo->currentData().toLongLong();
        if (locationId <= 0) {
            QMessageBox::warning(&dialog, QStringLiteral("缺少库位"),
                                 QStringLiteral("请选择启用状态的库位。"));
            return;
        }
        const QString batchNo = batchEdit->text().trimmed();
        if (requireBatch && batchNo.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("缺少批次"),
                                 QStringLiteral("该物料启用了批次管理，必须填写批次号。"));
            return;
        }
        if (quantitySpin->value() <= CountQuantityTolerance) {
            QMessageBox::warning(&dialog, QStringLiteral("盘盈数量无效"),
                                 QStringLiteral("盘盈数量必须大于0。"));
            return;
        }
        // 已有账面记录（含0结存）的物料必须走快照行，不能重复生成盘盈明细。
        QSqlQuery balance(m_database);
        balance.prepare(QStringLiteral(
            "SELECT quantity FROM stock_balances WHERE material_id=? AND warehouse_id=? "
            "AND location_id=? AND batch_no=?"));
        balance.addBindValue(materialId);
        balance.addBindValue(warehouseId);
        balance.addBindValue(locationId);
        balance.addBindValue(batchNo);
        if (!balance.exec()) {
            QMessageBox::warning(&dialog, QStringLiteral("盘盈失败"), balance.lastError().text());
            return;
        }
        if (balance.next()) {
            QMessageBox::information(
                &dialog, QStringLiteral("已有库存记录"),
                QStringLiteral("物料 %1 在仓库 %2、库位 %3、批次 %4 已有库存记录"
                               "（当前数量 %5），不属于账外盘盈。\n"
                               "请关闭本窗口后点击“重新加载库存”，在现有盘点行中填写实盘数。")
                    .arg(materialCombo->currentText(), warehouseCode,
                         locationCombo->currentText(),
                         batchNo.isEmpty() ? QStringLiteral("无批次") : batchNo,
                         QString::number(balance.value(0).toDouble(), 'g', 12)));
            return;
        }
        const QString identity = countIdentity(materialId, warehouseId, locationId, batchNo);
        for (int row = 0; row < m_table->rowCount(); ++row) {
            const QTableWidgetItem *material = m_table->item(row, 0);
            const QTableWidgetItem *batch = m_table->item(row, 3);
            if (!material || !batch) continue;
            if (countIdentity(material->data(MaterialIdRole).toLongLong(),
                              material->data(WarehouseIdRole).toLongLong(),
                              material->data(LocationIdRole).toLongLong(),
                              batch->text()) != identity) {
                continue;
            }
            m_materialSearchEdit->clear();
            m_table->setRowHidden(row, false);
            m_table->clearSelection();
            m_table->selectRow(row);
            m_table->scrollToItem(material);
            QMessageBox::information(
                &dialog, QStringLiteral("盘点明细已存在"),
                QStringLiteral("该物料、库位和批次已在盘点明细第 %1 行，请直接在该行填写实盘数。")
                    .arg(row + 1));
            return;
        }

        const int row = appendCountRow(materialId, materialCombo->currentText(), warehouseId,
                                       warehouseCode, locationId,
                                       locationCombo->currentData(CodeRole).toString(), batchNo,
                                       false, 0.0, quantitySpin->value(),
                                       QStringLiteral("盘盈：账面无记录，实物盘点发现"));
        m_materialSearchEdit->clear();
        m_table->setRowHidden(row, false);
        m_table->clearSelection();
        m_table->selectRow(row);
        m_table->scrollToItem(m_table->item(row, 0));
        m_submitButton->setEnabled(m_session.canManageWarehouse());
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void InventoryCountPage::submit()
{
    if (m_table->rowCount() == 0) {
        QMessageBox::warning(this, QStringLiteral("没有库存"),
                             QStringLiteral("当前范围没有可盘点库存；"
                                            "如有账外实物盘盈，请点击“添加盘盈物料”。"));
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
