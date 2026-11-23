#include "ui/pages/ProductionIssuePage.h"

#include "services/InventoryService.h"
#include "services/ProcessBom.h"
#include "services/OfficeTemplateService.h"
#include "ui/dialogs/DocumentTemplateDialog.h"
#include "ui/widgets/ComboBoxSearch.h"
#include "ui/widgets/StockLineTable.h"
#include "ui/widgets/TableExcelExport.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

namespace {
constexpr int ProductModelRole = Qt::UserRole + 1;
constexpr int ProductNameRole = Qt::UserRole + 2;
}

ProductionIssuePage::ProductionIssuePage(QSqlDatabase database,
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
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(20, 18, 20, 20);
    m_heading = new QLabel(QStringLiteral("新建领料单"), panel);
    m_heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    panelLayout->addWidget(m_heading);

    auto *headerForm = new QFormLayout;
    headerForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_issueTypeCombo = new QComboBox(panel);
    m_issueTypeCombo->addItem(QStringLiteral("生产领料（可导入BOM用料）"), QStringLiteral("SCLL"));
    m_issueTypeCombo->addItem(QStringLiteral("售后领料"), QStringLiteral("SHLL"));
    m_issueTypeCombo->addItem(QStringLiteral("研发领料"), QStringLiteral("YFLL"));
    m_issueTypeCombo->addItem(QStringLiteral("其他领料"), QStringLiteral("QTLL"));
    m_productCombo = new QComboBox(panel);
    m_productCombo->setMinimumWidth(400); // 最小宽度
    ComboBoxSearch::enableContainsSearch(
        m_productCombo, QStringLiteral("输入产品名称检索"));
    m_productModelEdit = new QLineEdit(panel);
    m_productModelEdit->setReadOnly(true);
    m_productModelEdit->setPlaceholderText(QStringLiteral("选择产品名称后自动显示"));
    m_batchEdit = new QLineEdit(panel);
    m_batchEdit->setPlaceholderText(QStringLiteral("例如 PROD-202609-001"));
    m_plannedQuantity = new QDoubleSpinBox(panel);
    m_plannedQuantity->setDecimals(0);
    m_plannedQuantity->setRange(1, 999999999999.0);
    m_plannedQuantity->setValue(1.0);
    m_dateEdit = new QDateEdit(QDate::currentDate(), panel);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_handlerEdit = new QLineEdit(m_session.displayName, panel);
    m_handlerEdit->setProperty("currentUserDefault", true);
    m_handlerEdit->setProperty("lastCurrentUserDefault", m_session.displayName);
    m_numberLabel = new QLabel(QStringLiteral("提交时自动生成"), panel);
    m_numberLabel->setObjectName(QStringLiteral("mutedText"));
    m_notesEdit = new QTextEdit(panel);
    m_notesEdit->setMaximumHeight(65);
    headerForm->addRow(QStringLiteral("领料类型 *"), m_issueTypeCombo);
    headerForm->addRow(QStringLiteral("领料日期 *"), m_dateEdit);
    headerForm->addRow(QStringLiteral("领料人员"), m_handlerEdit);
    headerForm->addRow(QStringLiteral("领料单号"), m_numberLabel);
    headerForm->addRow(QStringLiteral("产品名称"), m_productCombo);
    headerForm->addRow(QStringLiteral("产品型号"), m_productModelEdit);
    headerForm->addRow(QStringLiteral("备注"), m_notesEdit);
    panelLayout->addLayout(headerForm);

    m_productionFieldsGroup = new QGroupBox(QStringLiteral("整机生产任务 / 半成品分次领料（可选）"), panel);
    auto *productionForm = new QFormLayout(m_productionFieldsGroup);
    productionForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    productionForm->addRow(QStringLiteral("生产批次"), m_batchEdit);
    productionForm->addRow(QStringLiteral("生产台数"), m_plannedQuantity);
    m_taskCombo = new QComboBox(m_productionFieldsGroup);
    ComboBoxSearch::enableContainsSearch(m_taskCombo, QStringLiteral("按整机生产批次选择任务"));
    productionForm->insertRow(0, QStringLiteral("继续已有任务"), m_taskCombo);
    m_stageCombo = new QComboBox(m_productionFieldsGroup);
    productionForm->addRow(QStringLiteral("本次领料阶段"), m_stageCombo);
    m_stageSummary = new QLabel(m_productionFieldsGroup);
    m_stageSummary->setWordWrap(true);
    productionForm->addRow(m_stageSummary);
    m_demandTable = new QTableWidget(0, 5, m_productionFieldsGroup);
    m_demandTable->setHorizontalHeaderLabels({QStringLiteral("物料编码"),QStringLiteral("物料名称"),
        QStringLiteral("计划用量"),QStringLiteral("已领（扣除退料/撤销）"),QStringLiteral("未领数量")});
    m_demandTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_demandTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_demandTable->horizontalHeader()->setStretchLastSection(true);
    m_demandTable->setMinimumHeight(160);
    productionForm->addRow(m_demandTable);
    m_remainingButton = new QPushButton(QStringLiteral("带入本阶段未领物料"), m_productionFieldsGroup);
    auto *progressFullscreen = new QPushButton(QStringLiteral("全屏查看用料进度"), m_productionFieldsGroup);
    productionForm->addRow(m_remainingButton, progressFullscreen);
    connect(m_remainingButton, &QPushButton::clicked, this, &ProductionIssuePage::importProductBom);
    connect(progressFullscreen, &QPushButton::clicked, this, [this] {
        TableExcelExport::fullScreenTable(m_demandTable, QStringLiteral("生产阶段用料进度"), this,
                                        [this] { refreshStageProgress(); });
    });
    m_processBomButton = new QPushButton(QStringLiteral("按工艺组领料"), m_productionFieldsGroup);
    productionForm->addRow(QStringLiteral("工艺 BOM"), m_processBomButton);
    connect(m_processBomButton, &QPushButton::clicked, this, &ProductionIssuePage::importProcessBom);
    panelLayout->addWidget(m_productionFieldsGroup);

    m_lines = new StockLineTable(m_database, StockLineTable::Mode::Outbound, panel);
    m_lines->setKeepEmptyWhenNoRows(true);
    m_lines->setIssueOptionsMode(true);
    m_lines->setProductionUsageMode(true);
    m_lines->setProductionQuantity(m_plannedQuantity->value());
    m_submitButton = new QPushButton(QStringLiteral("确认并领料"), m_lines);
    m_submitButton->setProperty("primary", true);
    m_submitButton->setFixedSize(110, 34);
    m_submitButton->setEnabled(m_session.canPostProduction());
    m_lines->addToolbarAction(m_submitButton);
    m_usageHint = new QLabel(
        QStringLiteral("按生产批次选择工艺 BOM 中的工艺组，带入剩余未领物料。"),
        panel);
    m_usageHint->setObjectName(QStringLiteral("mutedText"));
    m_usageHint->setWordWrap(true);
    panelLayout->addWidget(m_usageHint);
    panelLayout->addWidget(m_lines);
    pageLayout->addWidget(panel);

    auto *recentPanel = new QFrame(pageBody);
    recentPanel->setObjectName(QStringLiteral("panel"));
    auto *recentLayout = new QVBoxLayout(recentPanel);
    auto *recentToolbar = new QHBoxLayout;
    recentToolbar->addWidget(new QLabel(QStringLiteral("近期领料单"), recentPanel));
    recentToolbar->addStretch();
    auto *editRecentButton = new QPushButton(QStringLiteral("修改单据"), recentPanel);
    editRecentButton->setProperty("primary", true);
    auto *fullScreenRecentButton = new QPushButton(QStringLiteral("全屏显示"), recentPanel);
    recentToolbar->addWidget(editRecentButton);
    recentToolbar->addWidget(fullScreenRecentButton);
    recentLayout->addLayout(recentToolbar);
    m_recentTable = new QTableWidget(0, 6, recentPanel);
    m_recentTable->setProperty("businessDocumentTable", true);
    m_recentTable->setHorizontalHeaderLabels({QStringLiteral("单据号"), QStringLiteral("领料类型"),
                                              QStringLiteral("日期"), QStringLiteral("生产批次"),
                                              QStringLiteral("成品"), QStringLiteral("明细数")});
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
    connect(m_recentTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) {
        m_recentTable->setCurrentCell(row, 0);
        m_recentTable->selectRow(row);
        TableExcelExport::editSelectedBusinessDocument(
            m_recentTable, m_recentTable->window(),
            [this] { refreshRecentDocuments(); }, this->window());
    });
    connect(fullScreenRecentButton, &QPushButton::clicked, this, [this] {
        TableExcelExport::fullScreenTable(
            m_recentTable, QStringLiteral("全部领料单"), this, [this] { refreshRecentDocuments(); });
    });
    connect(m_submitButton, &QPushButton::clicked, this, &ProductionIssuePage::submit);
    connect(m_plannedQuantity, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] {
        m_lines->clearLines(); refreshStageProgress();
    });
    connect(m_taskCombo, qOverload<int>(&QComboBox::activated), this, [this] { selectProductionTask(); });
    connect(m_stageCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        m_lines->clearLines(); refreshStageProgress();
    });
    connect(m_batchEdit, &QLineEdit::editingFinished, this, [this] {
        if (m_batchEdit->property("lastStageBatch").toString() == m_batchEdit->text().trimmed()) return;
        m_lines->clearLines(); refreshProductionStages();
    });
    connect(m_lines, &StockLineTable::productionBomRequested,
            this, &ProductionIssuePage::importProductBom);
    connect(m_issueTypeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        m_lines->clearLines();
        updateIssueType();
    });
    connect(m_productCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        m_lines->clearLines();
        m_batchEdit->clear();
        m_productModelEdit->setText(
            m_productCombo->currentData(ProductModelRole).toString());
        updateIssueType();
        refreshProductionStages();
    });
    updateIssueType();
    resetSubmissionToken();
    refreshReferenceData();
}

void ProductionIssuePage::resetSubmissionToken()
{
    m_submissionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void ProductionIssuePage::updateIssueType()
{
    const bool production = m_issueTypeCombo->currentData().toString() == QStringLiteral("SCLL");
    const bool hasProductionProduct = production
        && m_productCombo->currentData().toLongLong() > 0;
    m_productModelEdit->setText(
        m_productCombo->currentData(ProductModelRole).toString());
    m_productionFieldsGroup->setVisible(production);
    m_batchEdit->setEnabled(hasProductionProduct);
    m_plannedQuantity->setEnabled(hasProductionProduct && m_taskRunId <= 0);
    m_processBomButton->setEnabled(hasProductionProduct && m_session.canPostProduction());
    if (m_stageCombo && !m_stageCombo->currentData().toString().isEmpty()) m_processBomButton->setEnabled(false);
    m_lines->setProductionUsageMode(hasProductionProduct);
    m_lines->setMaterialCategoryFilter(QString());
    m_heading->setText(QStringLiteral("新建%1单")
                           .arg(m_issueTypeCombo->currentText().section(QStringLiteral("（"), 0, 0)));
    m_usageHint->setText(
        production
            ? (hasProductionProduct
                ? QStringLiteral("选择本次领料阶段，点击“带入本阶段未领物料”。可删除暂不领的行或调低数量，分次领取。")
                : QStringLiteral("生产信息可以不填。当前为直接领料模式，可逐项添加物料并手工填写领用数量；选择成品后可导入BOM并按台数计算。"))
            : QStringLiteral("点击“添加物料”逐项选择本次领用的物料、仓库、库位、批次和数量。"));
    m_submitButton->setEnabled(m_session.canPostProduction());
}

void ProductionIssuePage::refreshReferenceData()
{
    const QVariant selected = m_productCombo->currentData();
    const QSignalBlocker productBlocker(m_productCombo);
    m_productCombo->clear();
    m_productCombo->addItem(QStringLiteral("不选择产品"), qlonglong(0));
    m_productCombo->setItemData(0, QString(), ProductModelRole);
    m_productCombo->setItemData(0, QString(), ProductNameRole);
    QSqlQuery products(m_database);
    products.exec(QStringLiteral(
        "SELECT m.id,m.code,m.name,m.specification FROM materials m "
        "JOIN material_categories c ON c.id=m.category_id "
        "WHERE m.is_active=1 AND c.code='FINISHED' ORDER BY m.name,m.code"));
    while (products.next()) {
        const int index = m_productCombo->count();
        m_productCombo->addItem(products.value(2).toString(), products.value(0));
        m_productCombo->setItemData(index, products.value(3), ProductModelRole);
        m_productCombo->setItemData(index, products.value(2), ProductNameRole);
        m_productCombo->setItemData(
            index,
            QStringLiteral("%1  |  %2  |  %3")
                .arg(products.value(2).toString(), products.value(3).toString(),
                     products.value(1).toString()),
            Qt::ToolTipRole);
    }
    const int selectedIndex = m_productCombo->findData(selected);
    m_productCombo->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    m_lines->refreshReferenceData();
    {
        const QSignalBlocker taskBlocker(m_taskCombo);
        m_taskCombo->clear();
        m_taskCombo->addItem(QStringLiteral("新生产批次 / 手工填写"), qlonglong(0));
        QSqlQuery runs(m_database);
        if (runs.exec(QStringLiteral("SELECT id,batch_no,product_name FROM production_runs ORDER BY id DESC"))) {
            while (runs.next()) m_taskCombo->addItem(runs.value(1).toString() + QStringLiteral(" - ") + runs.value(2).toString(), runs.value(0));
        }
    }
    refreshProductionStages();
    updateIssueType();
    refreshRecentDocuments();
}

void ProductionIssuePage::importProcessBom()
{
    const auto productId = m_productCombo->currentData().toLongLong();
    ProcessBom bom;
    QString error;
    if (!ProcessBom::load(m_database, productId, bom, error)) {
        QMessageBox::warning(this, QStringLiteral("读取工艺 BOM 失败"), error);
        return;
    }
    if (bom.groups.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("尚无工艺 BOM"),
                                 QStringLiteral("请先在物料维护的“工艺BOM”页签导入该成品的配料表 JSON。"));
        return;
    }
    QStringList names;
    for (const auto &group : bom.groups) names.append(group.label());
    QInputDialog choice(this);
    choice.setWindowTitle(QStringLiteral("按工艺组领料"));
    choice.setLabelText(QStringLiteral("选择本次生产的工艺组"));
    choice.setComboBoxItems(names);
    if (choice.exec() != QDialog::Accepted) return;
    const auto index = choice.findChild<QComboBox *>()->currentIndex();
    const auto &group = bom.groups.at(index);
    const auto name = group.label();
    QList<ProcessBomUsage> usages;
    if (!group.issueMaterials(m_plannedQuantity->value(), usages, error)) {
        QMessageBox::warning(this, QStringLiteral("该工艺组暂不能领料"), error);
        return;
    }
    QList<QPair<qlonglong, double>> lines;
    for (const auto &usage : usages) lines.append({usage.materialId, usage.quantity});
    const auto summary = QStringLiteral("按“%1”生成 %2 种物料的领料明细，替换当前明细。\n"
                                        "领用量 = 每组用量 × %3（生产台数）。")
                             .arg(name).arg(lines.size()).arg(m_plannedQuantity->value());
    if (QMessageBox::question(this, QStringLiteral("确认工艺用料"), summary) != QMessageBox::Yes) return;
    if (!m_lines->setProductionMaterials(lines, &error)) {
        QMessageBox::warning(this, QStringLiteral("带入工艺用料失败"), error);
        return;
    }
    m_usageHint->setText(QStringLiteral("已带入“%1”的 %2 种物料，请核对领用数量并指定库存批次。")
                             .arg(name).arg(lines.size()));
}

void ProductionIssuePage::selectProductionTask()
{
    const auto id = m_taskCombo->currentData().toLongLong();
    m_lines->clearLines();
    if (id <= 0) {
        m_batchEdit->clear(); m_plannedQuantity->setEnabled(true);
        refreshProductionStages(); return;
    }
    QSqlQuery q(m_database);
    q.prepare(QStringLiteral("SELECT batch_no,product_material_id,planned_quantity FROM production_runs WHERE id=?"));
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return;
    const int index = m_productCombo->findData(q.value(1));
    if (index < 0) { QMessageBox::warning(this, QStringLiteral("任务不可用"), QStringLiteral("该任务成品已停用或不存在。")); return; }
    const QSignalBlocker productBlock(m_productCombo), countBlock(m_plannedQuantity);
    m_productCombo->setCurrentIndex(index);
    m_batchEdit->setText(q.value(0).toString());
    m_plannedQuantity->setValue(q.value(2).toDouble());
    updateIssueType();
    refreshProductionStages();
}

void ProductionIssuePage::refreshProductionStages()
{
    m_batchEdit->setProperty("lastStageBatch", m_batchEdit->text().trimmed());
    const QString previous = m_stageCombo->currentData().toString();
    const bool hadStages = m_stageCombo->count() > 1;
    const QSignalBlocker blocker(m_stageCombo);
    m_stageCombo->clear();
    m_stageCombo->addItem(QStringLiteral("手工领料（不统计阶段进度）"), QString());
    m_remainingButton->setEnabled(false);
    m_demandTable->setRowCount(0);
    const auto previousFingerprint = m_bomFingerprint;
    m_task = ProductionTask{};
    m_taskRunId = 0; m_bomFingerprint.clear();
    const auto productId = m_productCombo->currentData().toLongLong();
    m_plannedQuantity->setEnabled(productId > 0);
    if (productId <= 0) { refreshStageProgress(); return; }
    QSqlQuery run(m_database);
    run.prepare(QStringLiteral("SELECT id,product_material_id,planned_quantity FROM production_runs WHERE batch_no=?"));
    run.addBindValue(m_batchEdit->text().trimmed().toUpper());
    if (!run.exec()) { m_stageSummary->setText(run.lastError().text()); return; }
    if (run.next()) {
        if (run.value(1).toLongLong() != productId) {
            m_stageSummary->setText(QStringLiteral("此批次属于其他成品，请从已有任务选择。")); return;
        }
        m_taskRunId = run.value(0).toLongLong();
        const QSignalBlocker countBlock(m_plannedQuantity);
        m_plannedQuantity->setValue(run.value(2).toDouble());
        m_plannedQuantity->setEnabled(false);
    }
    QString error;
    if (!ProductionTask::load(m_database, m_taskRunId, productId, m_task, &error)) {
        m_stageSummary->setText(error); return;
    }
    m_bomFingerprint = m_task.fingerprint();
    if (!previousFingerprint.isEmpty() && previousFingerprint != m_bomFingerprint)
        m_lines->clearLines();
    for (const auto &s : m_task.issueStages()) {
        QList<ProductionDemand> demands;
        if (!m_task.demand(m_database, m_taskRunId, s.key, m_plannedQuantity->value(), demands, &error)) {
            m_stageCombo->addItem(QStringLiteral("%1（无法带入用料）").arg(m_task.stagePath(s.key)), s.key);
            continue;
        }
        if (demands.isEmpty()) continue;
        int pending = 0;
        for (const auto &d : demands) if (d.remaining() > 0.0000001) ++pending;
        const QString label = s.key == QStringLiteral("0") ? QStringLiteral("整机装配")
            : m_task.usesProcessBom() ? QStringLiteral("工艺组") : QStringLiteral("历史半成品阶段");
        m_stageCombo->addItem(QStringLiteral("%1：%2（未领%3种）")
            .arg(label, m_task.stagePath(s.key)).arg(pending), s.key);
    }
    const int index = m_stageCombo->findData(previous);
    m_stageCombo->setCurrentIndex(index > 0 || (hadStages && index == 0) ? index : (m_stageCombo->count() > 1 ? 1 : 0));
    const QSignalBlocker taskBlocker(m_taskCombo);
    const int taskIndex = m_taskCombo->findData(m_taskRunId);
    m_taskCombo->setCurrentIndex(taskIndex >= 0 ? taskIndex : 0);
    refreshStageProgress();
}

void ProductionIssuePage::refreshStageProgress()
{
    const auto key = m_stageCombo->currentData().toString();
    const auto *currentStage = m_task.stage(key);
    const double stageQuantity = m_plannedQuantity->value() * (currentStage ? currentStage->perMachine : 1);
    if (m_lines->property("stageQuantity").toDouble() != stageQuantity) {
        m_lines->setProperty("stageQuantity", stageQuantity);
        m_lines->setProductionQuantity(stageQuantity);
    }
    m_demandTable->setRowCount(0);
    m_processBomButton->setEnabled(key.isEmpty() && m_productCombo->currentData().toLongLong() > 0 && m_session.canPostProduction());
    m_remainingButton->setEnabled(false);
    if (key.isEmpty()) {
        const bool missingRecipe = m_task.usesProcessBom() && m_task.issueStages().size() == 1;
        m_stageSummary->setText(missingRecipe
            ? QStringLiteral("该成品尚无工艺 BOM。要分阶段领料，请先在“物料维护 → 工艺BOM”中维护工艺组。")
            : QStringLiteral("手工模式不统计阶段未领数量。"));
        return;
    }
    QList<ProductionDemand> demands;
    QString error;
    if (!m_task.demand(m_database, m_taskRunId, key, m_plannedQuantity->value(), demands, &error)) {
        m_stageSummary->setText(error); return;
    }
    for (const auto &d : demands) {
        const int row = m_demandTable->rowCount(); m_demandTable->insertRow(row);
        const QStringList values{d.code, d.name, QString::number(d.required,'g',12),
            QString::number(d.issued,'g',12), QString::number(d.remaining(),'g',12)};
        for (int col = 0; col < values.size(); ++col) m_demandTable->setItem(row,col,new QTableWidgetItem(values.at(col)));
    }
    const auto *stage = m_task.stage(key);
    m_remainingButton->setEnabled(!demands.isEmpty() && m_session.canPostProduction());
    if (m_task.usesProcessBom() && key != QStringLiteral("0")) {
        m_stageSummary->setText(QStringLiteral("%1：计划 %2 组。可按表中的未领数量分次领取。")
            .arg(stage->name).arg(stageQuantity, 0, 'g', 12));
        return;
    }
    const double received = ProductionTask::received(m_database, m_taskRunId, key, &error);
    if (received < 0) { m_stageSummary->setText(error); return; }
    m_stageSummary->setText(QStringLiteral("%1：计划 %2，已完工入库 %3。仅带出本阶段用料，遇到下级半成品直接领其库存；可使用其他任务生产的库存批号。")
        .arg(stage->name).arg(stage->perMachine*m_plannedQuantity->value(),0,'g',12).arg(received,0,'g',12));
    if (m_task.usesProcessBom() && m_task.issueStages().size() == 1)
        m_stageSummary->setText(m_stageSummary->text() + QStringLiteral(" 如需半成品领料，请先在物料维护中维护该产品的工艺 BOM。"));
}

void ProductionIssuePage::importProductBom()
{
    if (!m_stageCombo->currentData().toString().isEmpty()) {
        if (m_batchEdit->text().trimmed().isEmpty()) {
            QMessageBox::information(this, QStringLiteral("填写生产批次"), QStringLiteral("请先填写整机生产批次，后续分次领料会关联此任务。"));
            return;
        }
        const auto key = m_stageCombo->currentData().toString();
        QList<ProductionDemand> demand;
        QString error;
        if (!m_task.demand(m_database, m_taskRunId, key, m_plannedQuantity->value(), demand, &error)) {
            QMessageBox::warning(this, QStringLiteral("读取未领物料失败"), error); return;
        }
        if (QMessageBox::question(this, QStringLiteral("带入本阶段未领物料"),
                QStringLiteral("将替换当前明细，带入本阶段的剩余未领数量。可以删行或减少数量分次领取；库存批号由你选择。")) != QMessageBox::Yes) return;
        m_lines->setProductionQuantity(m_plannedQuantity->value() * m_task.stage(key)->perMachine);
        if (!m_lines->setProductionRemaining(demand, &error)) QMessageBox::warning(this, QStringLiteral("带入失败"), error);
        refreshStageProgress();
        return;
    }
    const qlonglong productId = m_productCombo->currentData().toLongLong();
    if (productId <= 0) {
        QMessageBox::information(this, QStringLiteral("请选择成品"),
                                 QStringLiteral("请先选择需要生产的成品物料。"));
        return;
    }

    QList<QPair<qlonglong, double>> materials;
    QStringList inactiveMaterials;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "WITH RECURSIVE bom_tree(id,component_material_id,total_quantity) AS ("
        " SELECT id,component_material_id,quantity FROM material_bom_items "
        " WHERE product_material_id=? AND parent_item_id IS NULL"
        " UNION ALL"
        " SELECT child.id,child.component_material_id,"
        "        parent.total_quantity*child.quantity"
        " FROM material_bom_items child JOIN bom_tree parent "
        " ON child.parent_item_id=parent.id WHERE child.product_material_id=?"
        ") "
        "SELECT tree.component_material_id,SUM(tree.total_quantity),m.code,m.name,m.is_active "
        "FROM bom_tree tree JOIN materials m ON m.id=tree.component_material_id "
        "WHERE NOT EXISTS(SELECT 1 FROM material_bom_items child "
        "                 WHERE child.parent_item_id=tree.id) "
        "GROUP BY tree.component_material_id,m.code,m.name,m.is_active ORDER BY m.code"));
    query.addBindValue(productId);
    query.addBindValue(productId);
    if (!query.exec()) {
        QMessageBox::warning(this, QStringLiteral("读取BOM失败"), query.lastError().text());
        return;
    }
    while (query.next()) {
        if (!query.value(4).toBool()) {
            inactiveMaterials.append(
                QStringLiteral("%1 - %2").arg(query.value(2).toString(),
                                               query.value(3).toString()));
            continue;
        }
        materials.append({query.value(0).toLongLong(), query.value(1).toDouble()});
    }

    if (!inactiveMaterials.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("BOM包含停用物料"),
            QStringLiteral("以下BOM末级物料已停用，无法生成完整领料明细：\n\n%1\n\n"
                           "请先在物料维护中启用或替换这些物料。")
                .arg(inactiveMaterials.join(QLatin1Char('\n'))));
        return;
    }
    if (materials.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("BOM没有可导入用料"),
            QStringLiteral("当前成品尚未维护BOM，或BOM中没有末级领用物料。"));
        return;
    }

    if (QMessageBox::question(
            this, QStringLiteral("确认导入BOM用料"),
            QStringLiteral("将按当前成品BOM导入 %1 种末级物料，并替换当前领料明细。\n\n"
                           "总用量将按“生产台数 × BOM累计用量”计算，导入后仍需逐项指定库存批次。")
                .arg(materials.size())) != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!m_lines->setProductionMaterials(materials, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入BOM用料失败"), error);
        return;
    }
    m_usageHint->setText(
        QStringLiteral("已从当前成品BOM导入 %1 种末级物料；总用量已按 %2 台计算，请逐项确认仓库、库位和库存批次。")
            .arg(materials.size())
            .arg(m_plannedQuantity->value(), 0, 'f', 0));
}

void ProductionIssuePage::refreshRecentDocuments()
{
    m_recentTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT d.id,d.document_no,CASE d.document_type "
        "WHEN 'SCLL' THEN '生产领料' WHEN 'SHLL' THEN '售后领料' "
        "WHEN 'YFLL' THEN '研发领料' WHEN 'QTLL' THEN '其他领料' "
        "WHEN 'WXLY' THEN '售后领料（历史）' WHEN 'YPLY' THEN '研发领料（历史）' "
        "ELSE '其他领料（历史）' END,d.document_date,"
        "COALESCE(p.batch_no,''),COALESCE(p.product_name,''),COUNT(i.id) "
        "FROM business_documents d LEFT JOIN production_runs p ON p.id=d.production_run_id "
        "LEFT JOIN business_document_items i ON i.document_id=d.id "
        "WHERE d.status<>'REVERSED' AND ("
        "d.document_type IN ('SCLL','SHLL','YFLL','QTLL') OR "
        "(d.document_type IN ('WXLY','YPLY','QTCK') AND (d.purpose LIKE '%领料%' OR EXISTS("
        "SELECT 1 FROM document_forms f WHERE f.document_id=d.id AND f.form_kind='productionIssue')))) "
        "GROUP BY d.id,p.id ORDER BY d.id DESC")
        + (m_recentTable->property("tableFullScreenActive").toBool() ? QString() : QStringLiteral(" LIMIT 20")));
    while (query.next()) {
        const int row = m_recentTable->rowCount();
        m_recentTable->insertRow(row);
        for (int column = 0; column < 6; ++column) {
            auto *item = new QTableWidgetItem(query.value(column + 1).toString());
            item->setData(Qt::UserRole, query.value(0));
            m_recentTable->setItem(row, column, item);
        }
    }
}

void ProductionIssuePage::submit()
{
    const QString issueType = m_issueTypeCombo->currentData().toString();
    const bool production = issueType == QStringLiteral("SCLL");
    const QString issueName = m_issueTypeCombo->currentText().section(QStringLiteral("（"), 0, 0);
    const qlonglong productId = m_productCombo->currentData().toLongLong();
    const bool hasProduct = productId > 0;
    const bool hasBatch = !m_batchEdit->text().trimmed().isEmpty();
    const bool hasProductionContext = production && hasProduct && hasBatch;
    if (production && hasProduct != hasBatch) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"),
                             QStringLiteral("生产信息可以全部不填；如需填写，请同时选择成品并填写生产批次。"));
        return;
    }
    QString error;
    QList<StockMovementRequest> movementLines = m_lines->lines(&error);
    if (movementLines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("领料明细有误"), error);
        return;
    }

    OfficeTemplateDocument issueForm;
    issueForm.kind = OfficeFormKind::ProductionIssue;
    issueForm.documentNumber = QStringLiteral("提交后自动生成");
    issueForm.documentDate = m_dateEdit->date();
    issueForm.fields.insert(QStringLiteral("handler"), m_handlerEdit->text().trimmed());
    issueForm.fields.insert(QStringLiteral("notes"), m_notesEdit->toPlainText().trimmed());
    issueForm.fields.insert(QStringLiteral("issueType"), issueType);
    issueForm.fields.insert(QStringLiteral("hasProductionContext"),
                            hasProductionContext ? QStringLiteral("1") : QStringLiteral("0"));
    issueForm.fields.insert(QStringLiteral("receivingDepartment"),
                            issueType == QStringLiteral("SCLL") ? QStringLiteral("生产部")
                            : issueType == QStringLiteral("SHLL") ? QStringLiteral("售后部")
                            : issueType == QStringLiteral("YFLL") ? QStringLiteral("研发部")
                                                                   : QStringLiteral("其他"));
    if (hasProduct) {
        issueForm.fields.insert(QStringLiteral("productName"),
                                m_productCombo->currentData(ProductNameRole).toString());
        issueForm.fields.insert(QStringLiteral("productModel"),
                                m_productModelEdit->text().trimmed());
    }
    if (hasProductionContext) {
        issueForm.fields.insert(QStringLiteral("productionBatch"), m_batchEdit->text().trimmed());
        issueForm.fields.insert(QStringLiteral("plannedQuantity"),
                                QString::number(m_plannedQuantity->value(), 'g', 12));
        const auto *stage = m_task.stage(m_stageCombo->currentData().toString());
        if (stage) {
            issueForm.fields.insert(QStringLiteral("productName"), stage->name);
            issueForm.fields.insert(QStringLiteral("productModel"), stage->model);
            issueForm.fields.insert(QStringLiteral("plannedQuantity"), QString::number(stage->perMachine*m_plannedQuantity->value(),'g',12));
        }
    }
    issueForm.lines = OfficeTemplateService::materialLines(m_database, movementLines, &error);
    if (issueForm.lines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法填写领料单模板"), error);
        return;
    }
    DocumentTemplateDialog issueDialog(issueForm, this);
    if (issueDialog.exec() != QDialog::Accepted) return;
    issueForm = issueDialog.document();
    const int synchronizedLineCount = qMin(static_cast<int>(movementLines.size()),
                                           static_cast<int>(issueForm.lines.size()));
    for (int index = 0; index < synchronizedLineCount; ++index) {
        const OfficeTemplateLine &formLine = issueForm.lines.at(index);
        StockMovementRequest &movement = movementLines[index];
        // 领料明细、确认表单和最终 Excel 始终使用同一份逐行单台用量。
        movement.unitUsage = formLine.unitUsage;
        movement.externalDispatch = formLine.externalQuantity > 0.0;
        movement.secondaryDispatch = formLine.reworkQuantity > 0.0;
        movement.lossMarked = formLine.lossQuantity > 0.0;
        movement.returnToStock = formLine.returnQuantity > 0.0;
        movement.notes = formLine.notes;
    }

    const QString confirmation = hasProductionContext
        ? QStringLiteral("确认按 %1 台提交 %2 条生产领料明细？库存将整单扣减并生成库存流水。")
              .arg(m_plannedQuantity->value(), 0, 'f', 0)
              .arg(movementLines.size())
        : QStringLiteral("确认提交 %1 条%2明细？库存将整单扣减并生成库存流水。")
              .arg(movementLines.size()).arg(issueName);
    if (QMessageBox::question(this, QStringLiteral("确认%1").arg(issueName), confirmation)
        != QMessageBox::Yes) {
        return;
    }

    StockDocumentRequest document;
    document.documentType = issueType;
    document.documentDate = m_dateEdit->date();
    document.handlerName = m_handlerEdit->text().trimmed();
    document.purpose = issueName;
    document.notes = m_notesEdit->toPlainText().trimmed();
    document.submissionToken = m_submissionToken;
    if (hasProductionContext) {
        document.productionStageKey = m_stageCombo->currentData().toString();
        document.productionBomFingerprint = m_bomFingerprint;
    }
    document.lines = movementLines;

    m_submitButton->setEnabled(false);
    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    qlonglong runId = 0;
    bool ok = false;
    if (hasProductionContext) {
        ProductionRunRequest run;
        run.batchNo = m_batchEdit->text().trimmed();
        run.productMaterialId = productId;
        run.plannedQuantity = m_plannedQuantity->value();
        ok = service.postProductionIssue(run, document, &posted, &runId, &error);
    } else {
        ok = service.postStockDocument(document, false, &posted, &error);
    }
    m_submitButton->setEnabled(m_session.canPostProduction());
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("%1失败").arg(issueName), error);
        return;
    }
    m_numberLabel->setText(posted.documentNumber);
    issueForm.documentNumber = posted.documentNumber;
    QString formError;
    if (OfficeTemplateService::attachToDocument(issueForm, m_database, m_session.userId,
                                                posted.documentId, &formError)) {
        QMessageBox::information(
            this, QStringLiteral("%1完成").arg(issueName),
            QStringLiteral("领料单 %1 已生效，模板表单已保存。\n\n文件：%2")
                .arg(posted.documentNumber,
                     QDir::toNativeSeparators(OfficeTemplateService::archiveFilePath(issueForm))));
    } else {
        QMessageBox::warning(
            this, QStringLiteral("领料已完成，但模板处理未全部完成"),
            QStringLiteral("领料单 %1 已生效，但以下保存步骤未完成：\n\n%2")
                .arg(posted.documentNumber, formError));
    }
    resetSubmissionToken();
    m_notesEdit->clear();
    m_lines->clearLines();
    emit stockChanged();
    refreshReferenceData();
}
