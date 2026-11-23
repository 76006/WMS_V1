#include "ui/pages/FinishedGoodsInPage.h"
#include "services/ProductionTask.h"
#include "services/InspectionService.h"

#include "services/InventoryService.h"
#include "services/OfficeTemplateService.h"
#include "ui/dialogs/DocumentTemplateDialog.h"
#include "ui/widgets/ComboBoxSearch.h"
#include "ui/widgets/TableExcelExport.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace {
constexpr int ProductMaterialRole = Qt::UserRole + 1;
constexpr int ProductCodeRole = Qt::UserRole + 2;
constexpr int ProductNameRole = Qt::UserRole + 3;
constexpr int ProductModelRole = Qt::UserRole + 4;
constexpr int PlannedRole = Qt::UserRole + 5;
constexpr int DefaultWarehouseRole = Qt::UserRole + 6;
constexpr int DefaultLocationRole = Qt::UserRole + 7;
constexpr int RequireSerialRole = Qt::UserRole + 8;
constexpr int RequireBatchRole = Qt::UserRole + 9;
constexpr int ProductionBatchRole = Qt::UserRole + 10;
constexpr int ExternalProductionRole = Qt::UserRole + 11;
}

FinishedGoodsInPage::FinishedGoodsInPage(QSqlDatabase database,
                                         Session session,
                                         QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *pageScroll = new QScrollArea(this);
    pageScroll->setWidgetResizable(true);
    pageScroll->setFrameShape(QFrame::NoFrame);
    pageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *pageBody = new QWidget(pageScroll);
    pageBody->setObjectName(QStringLiteral("pageRoot"));
    auto *pageLayout = new QVBoxLayout(pageBody);
    pageLayout->setContentsMargins(20, 20, 20, 20);
    pageLayout->setSpacing(12);
    pageLayout->setSizeConstraint(QLayout::SetMinimumSize);

    auto *panel = new QFrame(pageBody);
    panel->setObjectName(QStringLiteral("panel"));
    panel->setMaximumWidth(980);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(22, 20, 22, 22);
    auto *heading = new QLabel(QStringLiteral("完工入库（成品 / 半成品）"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    panelLayout->addWidget(heading);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_runCombo = new QComboBox(panel);
    ComboBoxSearch::enableContainsSearch(
        m_runCombo, QStringLiteral("输入生产批次、成品编码或名称检索"));
    m_batchEdit = new QLineEdit(panel);
    m_batchEdit->setPlaceholderText(
        QStringLiteral("外部生产成品启用批次管理时填写；未启用可留空"));
    m_productLabel = new QLabel(panel);
    m_productLabel->setObjectName(QStringLiteral("mutedText"));
    m_progressLabel = new QLabel(panel);
    m_progressLabel->setObjectName(QStringLiteral("mutedText"));
    m_dateEdit = new QDateEdit(QDate::currentDate(), panel);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_quantitySpin = new QDoubleSpinBox(panel);
    m_quantitySpin->setDecimals(6);
    m_quantitySpin->setRange(0.000001, 999999999999.0);
    m_quantitySpin->setValue(1.0);
    m_warehouseCombo = new QComboBox(panel);
    m_locationCombo = new QComboBox(panel);
    m_handlerEdit = new QLineEdit(m_session.displayName, panel);
    m_handlerEdit->setProperty("currentUserDefault", true);
    m_handlerEdit->setProperty("lastCurrentUserDefault", m_session.displayName);
    m_serialEdit = new QTextEdit(panel);
    m_serialEdit->setMaximumHeight(115);
    m_serialEdit->setPlaceholderText(QStringLiteral("每行一个成品SN"));
    m_generateButton = new QPushButton(QStringLiteral("按本次数量批量生成SN"), panel);
    auto *serialBlock = new QWidget(panel);
    auto *serialLayout = new QVBoxLayout(serialBlock);
    serialLayout->setContentsMargins(0, 0, 0, 0);
    serialLayout->addWidget(m_serialEdit);
    serialLayout->addWidget(m_generateButton, 0, Qt::AlignLeft);
    m_notesEdit = new QTextEdit(panel);
    m_notesEdit->setMaximumHeight(70);
    m_stageCombo = new QComboBox(panel);
    m_inspectionCombo = new QComboBox(panel);
    form->addRow(QStringLiteral("入库成品/生产批次 *"), m_runCombo);
    form->addRow(QStringLiteral("完工阶段"), m_stageCombo);
    form->addRow(QStringLiteral("成品资料"), m_productLabel);
    form->addRow(QStringLiteral("生产进度"), m_progressLabel);
    form->addRow(QStringLiteral("库存批号"), m_batchEdit);
    form->addRow(QStringLiteral("是否送检"), m_inspectionCombo);
    form->addRow(QStringLiteral("生产日期 *"), m_dateEdit);
    form->addRow(QStringLiteral("本次入库数量 *"), m_quantitySpin);
    form->addRow(QStringLiteral("仓库 *"), m_warehouseCombo);
    form->addRow(QStringLiteral("库位 *"), m_locationCombo);
    form->addRow(QStringLiteral("操作人员"), m_handlerEdit);
    form->addRow(QStringLiteral("产品SN"), serialBlock);
    form->addRow(QStringLiteral("备注"), m_notesEdit);
    panelLayout->addLayout(form);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    m_submitButton = new QPushButton(QStringLiteral("确认完工入库"), panel);
    m_submitButton->setProperty("primary", true);
    actions->addWidget(m_submitButton);
    panelLayout->addLayout(actions);
    pageLayout->addWidget(panel, 0, Qt::AlignHCenter | Qt::AlignTop);

    auto *recentPanel = new QFrame(pageBody);
    recentPanel->setObjectName(QStringLiteral("panel"));
    auto *recentLayout = new QVBoxLayout(recentPanel);
    auto *recentToolbar = new QHBoxLayout;
    recentToolbar->addWidget(new QLabel(QStringLiteral("近期成品入库单"), recentPanel));
    recentToolbar->addStretch();
    auto *editRecentButton = new QPushButton(QStringLiteral("修改单据"), recentPanel);
    editRecentButton->setProperty("primary", true);
    auto *fullScreenRecentButton = new QPushButton(QStringLiteral("全屏显示"), recentPanel);
    recentToolbar->addWidget(editRecentButton);
    recentToolbar->addWidget(fullScreenRecentButton);
    recentLayout->addLayout(recentToolbar);
    m_recentTable = new QTableWidget(0, 5, recentPanel);
    m_recentTable->setProperty("businessDocumentTable", true);
    m_recentTable->setHorizontalHeaderLabels({QStringLiteral("单据号"), QStringLiteral("日期"),
                                              QStringLiteral("生产/库存批次"), QStringLiteral("成品"),
                                              QStringLiteral("数量")});
    m_recentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_recentTable->horizontalHeader()->setStretchLastSection(true);
    m_recentTable->setMinimumHeight(180);
    recentLayout->addWidget(m_recentTable);
    pageLayout->addWidget(recentPanel, 1);
    pageScroll->setWidget(pageBody);
    root->addWidget(pageScroll);

    connect(editRecentButton, &QPushButton::clicked, this, [this] {
        TableExcelExport::editSelectedBusinessDocument(
            m_recentTable, this, [this] { refreshRecentDocuments(); });
    });
    connect(fullScreenRecentButton, &QPushButton::clicked, this, [this] {
        TableExcelExport::fullScreenTable(
            m_recentTable, QStringLiteral("全部成品入库单"), this, [this] { refreshRecentDocuments(); });
    });
    connect(m_runCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &FinishedGoodsInPage::loadRunDetails);
    connect(m_warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &FinishedGoodsInPage::loadLocations);
    connect(m_generateButton, &QPushButton::clicked,
            this, &FinishedGoodsInPage::generateSerialNumbers);
    connect(m_stageCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { loadRunDetails(); });
    connect(m_inspectionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { selectInspection(); });
    connect(m_submitButton, &QPushButton::clicked, this, &FinishedGoodsInPage::submit);
    resetSubmissionToken();
    refreshReferenceData();
}

void FinishedGoodsInPage::resetSubmissionToken()
{
    m_submissionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void FinishedGoodsInPage::refreshReferenceData()
{
    const QVariant selectedRun = m_runCombo->currentData();
    m_runCombo->blockSignals(true);
    m_runCombo->clear();
    QSqlQuery runs(m_database);
    runs.exec(QStringLiteral(
        "SELECT p.id,p.batch_no,m.id,m.code,p.product_name,p.product_model,p.planned_quantity,"
        "m.default_warehouse_id,m.default_location_id,m.require_serial,m.require_batch,p.status "
        "FROM production_runs p JOIN materials m ON m.id=p.product_material_id ORDER BY p.id DESC"));
    while (runs.next()) {
        const int index = m_runCombo->count();
        m_runCombo->addItem(QStringLiteral("%1 - %2 - %3（%4）")
                                .arg(runs.value(1).toString(), runs.value(3).toString(),
                                     runs.value(4).toString(),
                                     runs.value(11).toString() == QStringLiteral("COMPLETED")
                                         ? QStringLiteral("已完成") : QStringLiteral("进行中")),
                            runs.value(0));
        m_runCombo->setItemData(index, runs.value(2), ProductMaterialRole);
        m_runCombo->setItemData(index, runs.value(3), ProductCodeRole);
        m_runCombo->setItemData(index, runs.value(4), ProductNameRole);
        m_runCombo->setItemData(index, runs.value(5), ProductModelRole);
        m_runCombo->setItemData(index, runs.value(6), PlannedRole);
        m_runCombo->setItemData(index, runs.value(7), DefaultWarehouseRole);
        m_runCombo->setItemData(index, runs.value(8), DefaultLocationRole);
        m_runCombo->setItemData(index, runs.value(9), RequireSerialRole);
        m_runCombo->setItemData(index, runs.value(10), RequireBatchRole);
        m_runCombo->setItemData(index, runs.value(1), ProductionBatchRole);
        m_runCombo->setItemData(index, false, ExternalProductionRole);
    }
    QSqlQuery externalProducts(m_database);
    externalProducts.exec(QStringLiteral(
        "SELECT m.id,m.code,m.name,m.specification,m.default_warehouse_id,"
        "m.default_location_id,m.require_serial,m.require_batch "
        "FROM materials m JOIN material_categories c ON c.id=m.category_id "
        "WHERE m.is_active=1 AND c.code IN ('FINISHED','SEMI') ORDER BY m.code"));
    while (externalProducts.next()) {
        const qlonglong materialId = externalProducts.value(0).toLongLong();
        const int index = m_runCombo->count();
        m_runCombo->addItem(
            QStringLiteral("外部生产（无内部生产批次） - %1 - %2")
                .arg(externalProducts.value(1).toString(), externalProducts.value(2).toString()),
            -materialId);
        m_runCombo->setItemData(index, materialId, ProductMaterialRole);
        m_runCombo->setItemData(index, externalProducts.value(1), ProductCodeRole);
        m_runCombo->setItemData(index, externalProducts.value(2), ProductNameRole);
        m_runCombo->setItemData(index, externalProducts.value(3), ProductModelRole);
        m_runCombo->setItemData(index, 0.0, PlannedRole);
        m_runCombo->setItemData(index, externalProducts.value(4), DefaultWarehouseRole);
        m_runCombo->setItemData(index, externalProducts.value(5), DefaultLocationRole);
        m_runCombo->setItemData(index, externalProducts.value(6), RequireSerialRole);
        m_runCombo->setItemData(index, externalProducts.value(7), RequireBatchRole);
        m_runCombo->setItemData(index, QString(), ProductionBatchRole);
        m_runCombo->setItemData(index, true, ExternalProductionRole);
    }
    const int selectedIndex = m_runCombo->findData(selectedRun);
    if (selectedIndex >= 0) m_runCombo->setCurrentIndex(selectedIndex);
    m_runCombo->blockSignals(false);

    const QVariant selectedWarehouse = m_warehouseCombo->currentData();
    m_warehouseCombo->blockSignals(true);
    m_warehouseCombo->clear();
    QSqlQuery warehouses(m_database);
    warehouses.exec(QStringLiteral(
        "SELECT id,code,name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (warehouses.next()) {
        m_warehouseCombo->addItem(QStringLiteral("%1 - %2")
                                      .arg(warehouses.value(1).toString(),
                                           warehouses.value(2).toString()),
                                  warehouses.value(0));
    }
    const int warehouseIndex = m_warehouseCombo->findData(selectedWarehouse);
    if (warehouseIndex >= 0) m_warehouseCombo->setCurrentIndex(warehouseIndex);
    m_warehouseCombo->blockSignals(false);
    loadRunDetails();
    refreshRecentDocuments();
}

void FinishedGoodsInPage::loadRunDetails()
{
    const int index = m_runCombo->currentIndex();
    if (index < 0) {
        m_productMaterialId = 0;
        m_currentSourceKey = 0;
        m_serialEdit->clear();
        m_batchEdit->clear();
        m_productLabel->setText(QStringLiteral("请选择内部生产批次或外部生产成品。"));
        m_progressLabel->clear();
        m_submitButton->setEnabled(false);
        return;
    }
    const qlonglong sourceKey = m_runCombo->currentData().toLongLong();
    qlonglong productMaterialId = m_runCombo->itemData(index, ProductMaterialRole).toLongLong();
    const bool runChanged = m_currentSourceKey != sourceKey;
    const QString selectedStage = runChanged ? QStringLiteral("0") : m_stageCombo->currentData().toString();
    QString stageName, stageModel;
    double stagePlanned = 0;
    qlonglong stageWarehouse = 0, stageLocation = 0;
    bool stageSerial = false, stageBatch = false;
    {
        const QSignalBlocker block(m_stageCombo);
        m_stageCombo->clear(); m_bomFingerprint.clear();
        QSqlQuery snapshot(m_database);
        snapshot.prepare(QStringLiteral("SELECT 1 FROM production_task_boms WHERE production_run_id=?"));
        snapshot.addBindValue(sourceKey);
        if (!snapshot.exec()) { m_progressLabel->setText(snapshot.lastError().text()); m_submitButton->setEnabled(false); return; }
        if (sourceKey > 0 && snapshot.next()) {
            ProductionTask task; QString error;
            if (!ProductionTask::load(m_database, sourceKey, productMaterialId, task, &error)) {
                m_progressLabel->setText(error); m_submitButton->setEnabled(false); return;
            }
            m_bomFingerprint = task.fingerprint();
            for (const auto &s : task.receiptStages()) m_stageCombo->addItem(s.code + QStringLiteral(" - ") + task.stagePath(s.key), s.key);
            const int found = m_stageCombo->findData(selectedStage);
            m_stageCombo->setCurrentIndex(found >= 0 ? found : 0);
            const auto *stage = task.stage(m_stageCombo->currentData().toString());
            if (stage) {
                productMaterialId = stage->materialId; stageName = stage->name; stageModel = stage->model;
                stagePlanned = stage->perMachine * m_runCombo->itemData(index, PlannedRole).toDouble();
                QSqlQuery material(m_database);
                material.prepare(QStringLiteral("SELECT code,default_warehouse_id,default_location_id,require_serial,require_batch FROM materials WHERE id=? AND is_active=1"));
                material.addBindValue(productMaterialId);
                if (!material.exec() || !material.next()) { m_progressLabel->setText(QStringLiteral("完工物料已停用或不存在。")); m_submitButton->setEnabled(false); return; }
                m_productCode = material.value(0).toString(); stageWarehouse = material.value(1).toLongLong(); stageLocation = material.value(2).toLongLong();
                stageSerial = material.value(3).toBool(); stageBatch = material.value(4).toBool();
            }
        } else m_stageCombo->addItem(QStringLiteral("整机 / 外部生产物料"), QString());
    }
    const QString stageKey = m_stageCombo->currentData().toString();
    const bool sourceChanged = runChanged || m_currentStageKey != stageKey;
    m_currentStageKey = stageKey;
    if (sourceChanged) {
        m_serialEdit->clear();
        m_batchEdit->clear();
    }
    m_currentSourceKey = sourceKey;
    m_productMaterialId = productMaterialId;
    if (stageKey.isEmpty()) m_productCode = m_runCombo->itemData(index, ProductCodeRole).toString();
    m_plannedQuantity = stageKey.isEmpty() ? m_runCombo->itemData(index, PlannedRole).toDouble() : stagePlanned;
    m_requireSerial = stageKey.isEmpty() ? m_runCombo->itemData(index, RequireSerialRole).toBool() : stageSerial;
    m_requireBatch = stageKey.isEmpty() ? m_runCombo->itemData(index, RequireBatchRole).toBool() : stageBatch;
    const bool externalProduction = m_runCombo->itemData(index, ExternalProductionRole).toBool();
    if (externalProduction) {
        m_receivedQuantity = 0.0;
    } else {
        InventoryService service(m_database, m_session.userId);
        QString error;
        m_receivedQuantity = stageKey.isEmpty() ? service.productionRunReceivedQuantity(sourceKey, &error)
            : ProductionTask::received(m_database, sourceKey, stageKey, &error);
        if (m_receivedQuantity < 0.0) { m_progressLabel->setText(error); m_submitButton->setEnabled(false); return; }
    }
    m_productLabel->setText(QStringLiteral("%1    型号：%2")
                                .arg(stageKey.isEmpty() ? m_runCombo->itemData(index, ProductNameRole).toString() : stageName,
                                     stageKey.isEmpty() ? m_runCombo->itemData(index, ProductModelRole).toString() : stageModel));
    if (externalProduction) {
        m_progressLabel->setText(QStringLiteral("外部生产成品：不关联内部生产计划和生产批次"));
        m_batchEdit->setReadOnly(false);
        m_batchEdit->setEnabled(m_requireBatch);
        m_batchEdit->setPlaceholderText(
            m_requireBatch ? QStringLiteral("该成品启用了批次管理，请填写库存批号")
                           : QStringLiteral("该成品未启用批次管理，无需填写"));
    } else {
        m_progressLabel->setText(QStringLiteral("计划 %1 / 已入库 %2 / 剩余 %3")
                                     .arg(m_plannedQuantity, 0, 'g', 12)
                                     .arg(m_receivedQuantity, 0, 'g', 12)
                                     .arg(qMax(0.0, m_plannedQuantity - m_receivedQuantity), 0, 'g', 12));
        m_batchEdit->setEnabled(true);
        m_batchEdit->setReadOnly(false);
        if (sourceChanged || m_batchEdit->text().isEmpty()) m_batchEdit->setText(m_requireBatch ? m_runCombo->itemData(index, ProductionBatchRole).toString() : QString());
        m_batchEdit->setPlaceholderText(QString());
    }
    const qlonglong defaultWarehouse = stageKey.isEmpty() ? m_runCombo->itemData(index, DefaultWarehouseRole).toLongLong() : stageWarehouse;
    const int warehouseIndex = m_warehouseCombo->findData(defaultWarehouse);
    if (warehouseIndex >= 0) m_warehouseCombo->setCurrentIndex(warehouseIndex);
    m_locationCombo->setProperty("preferredLocation",
                                 stageKey.isEmpty() ? m_runCombo->itemData(index, DefaultLocationRole) : QVariant(stageLocation));
    loadLocations();
    m_serialEdit->setEnabled(m_requireSerial);
    m_generateButton->setEnabled(m_requireSerial);
    if (!m_requireSerial) m_serialEdit->clear();
    m_submitButton->setEnabled(m_session.canPostProduction()
                               && m_warehouseCombo->count() > 0
                               && m_locationCombo->count() > 0);
    if (sourceChanged) { const QSignalBlocker block(m_inspectionCombo); m_inspectionCombo->setCurrentIndex(0); }
    refreshInspectionChoices();
}

void FinishedGoodsInPage::refreshInspectionChoices()
{
    const auto previous = m_inspectionCombo->currentData().toLongLong();
    const QSignalBlocker blocker(m_inspectionCombo);
    m_inspectionCombo->clear();
    m_inspectionCombo->addItem(QStringLiteral("否（无需送检，直接入库）"), qlonglong(0));
    InspectionService service(m_database, m_session.userId);
    QList<InspectionNotice> notices; QString error;
    if (!service.listQualifiedUnused(&notices, &error)) {
        m_progressLabel->setText(error); m_submitButton->setEnabled(false); return;
    }
    for (const auto &summary : notices) {
        InspectionNotice notice;
        if (!service.readNotice(summary.id, &notice, &error)) {
            m_progressLabel->setText(error); m_submitButton->setEnabled(false); return;
        }
        bool compatible = true; double count = 0;
        for (const auto &line : notice.lines) {
            const double available = line.qualifiedQuantity - line.inboundQuantity;
            if (available <= 0.0000001) continue;
            if (line.materialId != m_productMaterialId) compatible = false;
            count += available;
        }
        if (compatible && count > 0) m_inspectionCombo->addItem(
            QStringLiteral("是 | %1 | 可入库 %2").arg(notice.inspectionNumber).arg(count,0,'g',12), notice.id);
    }
    const int selected = m_inspectionCombo->findData(previous);
    m_inspectionCombo->setCurrentIndex(selected >= 0 ? selected : 0);
    selectInspection();
}

void FinishedGoodsInPage::selectInspection()
{
    const auto noticeId = m_inspectionCombo->currentData().toLongLong();
    m_quantitySpin->setReadOnly(noticeId > 0);
    m_batchEdit->setReadOnly(noticeId > 0);
    if (noticeId <= 0) {
        if (m_batchEdit->text().contains(QStringLiteral("、"))) m_batchEdit->clear();
        return;
    }
    InspectionService service(m_database, m_session.userId); InspectionNotice notice; QString error;
    if (!service.readNotice(noticeId, &notice, &error)) {
        QMessageBox::warning(this, QStringLiteral("读取送检通知失败"), error); return;
    }
    double count = 0; QStringList batches;
    for (const auto &line : notice.lines) if (line.qualifiedQuantity > line.inboundQuantity + 0.0000001) {
        count += line.qualifiedQuantity - line.inboundQuantity;
        if (!batches.contains(line.batchNumber)) batches.append(line.batchNumber);
    }
    m_quantitySpin->setValue(count);
    m_batchEdit->setText(batches.join(QStringLiteral("、")));
}

void FinishedGoodsInPage::loadLocations()
{
    const QVariant preferred = m_locationCombo->property("preferredLocation");
    const QVariant selected = preferred.isValid() ? preferred : m_locationCombo->currentData();
    m_locationCombo->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id,code,name FROM locations WHERE warehouse_id=? AND is_active=1 ORDER BY code"));
    query.addBindValue(m_warehouseCombo->currentData());
    query.exec();
    while (query.next()) {
        QString text = query.value(1).toString();
        if (!query.value(2).toString().isEmpty()) text += QStringLiteral(" - ") + query.value(2).toString();
        m_locationCombo->addItem(text, query.value(0));
    }
    const int index = m_locationCombo->findData(selected);
    if (index >= 0) m_locationCombo->setCurrentIndex(index);
    m_locationCombo->setProperty("preferredLocation", QVariant());
}

QStringList FinishedGoodsInPage::enteredSerialNumbers() const
{
    QStringList values = m_serialEdit->toPlainText().split(
        QRegularExpression(QStringLiteral("[,;\\r\\n]+")), Qt::SkipEmptyParts);
    for (QString &value : values) value = value.trimmed();
    return values;
}

void FinishedGoodsInPage::generateSerialNumbers()
{
    if (!m_requireSerial || m_productMaterialId <= 0) return;
    const double quantity = m_quantitySpin->value();
    if (std::abs(quantity - std::round(quantity)) > 0.0000001) {
        QMessageBox::warning(this, QStringLiteral("数量不正确"),
                             QStringLiteral("SN管理成品的入库数量必须是整数。"));
        return;
    }
    bool ok = false;
    const QString prefix = QInputDialog::getText(this, QStringLiteral("批量生成成品SN"),
                                                 QStringLiteral("SN前缀"), QLineEdit::Normal,
                                                 m_productCode, &ok);
    if (!ok) return;
    InventoryService service(m_database, m_session.userId);
    QString error;
    const QStringList serials = service.previewSerialNumbers(
        m_productMaterialId, prefix, static_cast<int>(std::round(quantity)), &error);
    if (serials.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法生成SN"), error);
        return;
    }
    m_serialEdit->setPlainText(serials.join(QLatin1Char('\n')));
}

void FinishedGoodsInPage::refreshRecentDocuments()
{
    m_recentTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT d.id,d.document_no,d.document_date,"
        "COALESCE(NULLIF(p.batch_no,''),NULLIF(i.batch_no,''),'无生产批次'),"
        "COALESCE(NULLIF(p.product_name,''),m.name),i.quantity "
        "FROM business_documents d "
        "JOIN business_document_items i ON i.document_id=d.id "
        "JOIN materials m ON m.id=i.material_id "
        "LEFT JOIN production_runs p ON p.id=d.production_run_id "
        "WHERE d.document_type='CPRK' AND d.status<>'REVERSED' ORDER BY d.id DESC")
        + (m_recentTable->property("tableFullScreenActive").toBool() ? QString() : QStringLiteral(" LIMIT 20")));
    while (query.next()) {
        const int row = m_recentTable->rowCount();
        m_recentTable->insertRow(row);
        for (int column = 0; column < 5; ++column) {
            auto *item = new QTableWidgetItem(query.value(column + 1).toString());
            item->setData(Qt::UserRole, query.value(0));
            m_recentTable->setItem(row, column, item);
        }
    }
}

void FinishedGoodsInPage::submit()
{
    if (m_runCombo->currentIndex() < 0 || m_productMaterialId <= 0
        || m_locationCombo->currentIndex() < 0) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"),
                             QStringLiteral("请选择入库成品、仓库和库位。"));
        return;
    }
    if (m_requireBatch && m_batchEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"),
                             QStringLiteral("该成品启用了批次管理，请填写库存批号。"));
        m_batchEdit->setFocus();
        return;
    }
    const bool externalProduction =
        m_runCombo->itemData(m_runCombo->currentIndex(), ExternalProductionRole).toBool();
    const QStringList serials = enteredSerialNumbers();
    if (m_requireSerial
        && (std::abs(m_quantitySpin->value() - std::round(m_quantitySpin->value())) > 0.0000001
            || serials.size() != static_cast<int>(std::round(m_quantitySpin->value())))) {
        QMessageBox::warning(this, QStringLiteral("SN数量不一致"),
                             QStringLiteral("SN管理成品的数量必须是整数，且SN数量必须一致。"));
        return;
    }
    StockMovementRequest line;
    line.materialId = m_productMaterialId;
    line.quantity = m_quantitySpin->value();
    line.batchNo = m_batchEdit->text().trimmed();
    line.warehouseId = m_warehouseCombo->currentData().toLongLong();
    line.locationId = m_locationCombo->currentData().toLongLong();
    line.serialNumbers = serials;
    line.notes = m_notesEdit->toPlainText().trimmed();

    QList<StockMovementRequest> receiptLines{line};
    const auto noticeId = m_inspectionCombo->currentData().toLongLong();
    if (noticeId > 0) {
        InspectionService inspections(m_database, m_session.userId); InspectionNotice notice; QString error;
        if (!inspections.readNotice(noticeId, &notice, &error)) {
            QMessageBox::warning(this, QStringLiteral("读取送检通知失败"), error); return;
        }
        QMap<QString, double> batches;
        for (const auto &source : notice.lines) {
            const double available = source.qualifiedQuantity - source.inboundQuantity;
            if (available <= 0.0000001) continue;
            if (source.materialId != m_productMaterialId) {
                QMessageBox::warning(this, QStringLiteral("送检物料不一致"), QStringLiteral("通知单合格物料发生变化，请刷新后重选。")); return;
            }
            batches[source.batchNumber] += available;
        }
        receiptLines.clear(); int serialOffset = 0; double received = 0;
        for (auto it = batches.cbegin(); it != batches.cend(); ++it) {
            auto part = line; part.batchNo = it.key(); part.quantity = it.value();
            if (m_requireSerial) {
                if (std::abs(part.quantity-std::round(part.quantity)) > 0.0000001) {
                    QMessageBox::warning(this, QStringLiteral("送检数量有误"), QStringLiteral("SN物料每个批号的合格数量必须为整数。")); return;
                }
                part.serialNumbers = serials.mid(serialOffset, static_cast<int>(std::round(part.quantity)));
                serialOffset += part.serialNumbers.size();
            }
            receiptLines.append(part); received += part.quantity;
        }
        if (std::abs(received-line.quantity) > 0.0000001 || receiptLines.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("合格数量已变化"), QStringLiteral("请重新选择送检单后再入库。")); return;
        }
    }

    OfficeTemplateDocument inboundForm;
    inboundForm.kind = OfficeFormKind::FinishedGoodsInbound;
    inboundForm.documentNumber = QStringLiteral("提交后自动生成");
    inboundForm.documentDate = m_dateEdit->date();
    inboundForm.fields.insert(QStringLiteral("handler"), m_handlerEdit->text().trimmed());
    inboundForm.fields.insert(QStringLiteral("notes"), m_notesEdit->toPlainText().trimmed());
    QString formError;
    inboundForm.lines = OfficeTemplateService::materialLines(m_database, receiptLines, &formError);
    if (inboundForm.lines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法填写成品入库单模板"), formError);
        return;
    }
    DocumentTemplateDialog inboundDialog(inboundForm, this);
    if (inboundDialog.exec() != QDialog::Accepted) return;
    inboundForm = inboundDialog.document();

    const double afterReceipt = m_receivedQuantity + m_quantitySpin->value();
    QString confirmation = QStringLiteral("确认本次成品入库 %1？库存将立即增加并生成流水。")
                               .arg(m_quantitySpin->value(), 0, 'g', 12);
    if (!externalProduction && afterReceipt > m_plannedQuantity + 0.0000001) {
        confirmation += QStringLiteral("\n本次提交后将超过计划数量 %1。")
                            .arg(afterReceipt - m_plannedQuantity, 0, 'g', 12);
    }
    if (QMessageBox::question(this, QStringLiteral("确认成品入库"), confirmation)
        != QMessageBox::Yes) {
        return;
    }

    StockDocumentRequest document;
    document.documentType = QStringLiteral("CPRK");
    document.documentDate = m_dateEdit->date();
    document.handlerName = m_handlerEdit->text().trimmed();
    document.notes = m_notesEdit->toPlainText().trimmed();
    document.submissionToken = m_submissionToken;
    document.productionRunId = qMax<qlonglong>(0, m_runCombo->currentData().toLongLong());
    document.productionStageKey = m_currentStageKey;
    document.productionBomFingerprint = m_bomFingerprint;
    document.inspectionNoticeId = noticeId;
    document.lines = receiptLines;

    m_submitButton->setEnabled(false);
    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    QString error;
    const bool ok = service.postFinishedGoodsInbound(document, &posted, &error);
    m_submitButton->setEnabled(m_session.canPostProduction());
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("成品入库失败"), error);
        return;
    }
    inboundForm.documentNumber = posted.documentNumber;
    if (OfficeTemplateService::attachToDocument(inboundForm, m_database, m_session.userId,
                                                posted.documentId, &formError)) {
        QMessageBox::information(
            this, QStringLiteral("成品入库完成"),
            QStringLiteral("成品入库单 %1 已生效，模板表单已保存。\n\n文件：%2")
                .arg(posted.documentNumber,
                     QDir::toNativeSeparators(OfficeTemplateService::archiveFilePath(inboundForm))));
    } else {
        QMessageBox::warning(
            this, QStringLiteral("成品入库已完成，但模板处理未全部完成"),
            QStringLiteral("成品入库单 %1 已生效，但以下保存步骤未完成：\n\n%2")
                .arg(posted.documentNumber, formError));
    }
    resetSubmissionToken();
    m_currentSourceKey = 0;
    m_serialEdit->clear();
    m_notesEdit->clear();
    emit stockChanged();
    refreshReferenceData();
}
