#include "ui/pages/ProductionIssuePage.h"

#include "services/InventoryService.h"
#include "ui/widgets/ComboBoxSearch.h"
#include "ui/widgets/StockLineTable.h"

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
#include <QSignalBlocker>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

namespace {
constexpr int ProductCodeRole = Qt::UserRole + 1;
}

ProductionIssuePage::ProductionIssuePage(QSqlDatabase database,
                                         Session session,
                                         QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(20, 18, 20, 20);
    auto *heading = new QLabel(QStringLiteral("新建生产领料单"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    panelLayout->addWidget(heading);

    auto *headerForm = new QFormLayout;
    headerForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_productCombo = new QComboBox(panel);
    ComboBoxSearch::enableContainsSearch(
        m_productCombo, QStringLiteral("输入成品编码或名称检索"));
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
    m_numberLabel = new QLabel(QStringLiteral("提交时自动生成"), panel);
    m_numberLabel->setObjectName(QStringLiteral("mutedText"));
    m_notesEdit = new QTextEdit(panel);
    m_notesEdit->setMaximumHeight(65);
    headerForm->addRow(QStringLiteral("成品物料 *"), m_productCombo);
    headerForm->addRow(QStringLiteral("生产批次 *"), m_batchEdit);
    headerForm->addRow(QStringLiteral("生产台数 *"), m_plannedQuantity);
    headerForm->addRow(QStringLiteral("领料日期 *"), m_dateEdit);
    headerForm->addRow(QStringLiteral("领料人员"), m_handlerEdit);
    headerForm->addRow(QStringLiteral("领料单号"), m_numberLabel);
    headerForm->addRow(QStringLiteral("备注"), m_notesEdit);
    panelLayout->addLayout(headerForm);

    m_lines = new StockLineTable(m_database, StockLineTable::Mode::Outbound, panel);
    m_lines->setProductionUsageMode(true);
    m_lines->setProductionQuantity(m_plannedQuantity->value());
    m_usageHint = new QLabel(
        QStringLiteral("选择成品后将递归展开BOM并带出全部末级物料；总用量按“生产台数 × BOM累计用量”计算，库存批次仍由用户指定。"),
        panel);
    m_usageHint->setObjectName(QStringLiteral("mutedText"));
    m_usageHint->setWordWrap(true);
    panelLayout->addWidget(m_usageHint);
    panelLayout->addWidget(m_lines);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    m_submitButton = new QPushButton(QStringLiteral("确认并领料"), panel);
    m_submitButton->setProperty("primary", true);
    m_submitButton->setEnabled(m_session.canPostProduction());
    actions->addWidget(m_submitButton);
    panelLayout->addLayout(actions);
    root->addWidget(panel);

    auto *recentPanel = new QFrame(this);
    recentPanel->setObjectName(QStringLiteral("panel"));
    auto *recentLayout = new QVBoxLayout(recentPanel);
    recentLayout->addWidget(new QLabel(QStringLiteral("近期生产领料单"), recentPanel));
    m_recentTable = new QTableWidget(0, 5, recentPanel);
    m_recentTable->setHorizontalHeaderLabels({QStringLiteral("单据号"), QStringLiteral("日期"),
                                              QStringLiteral("生产批次"), QStringLiteral("成品"),
                                              QStringLiteral("明细数")});
    m_recentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recentTable->horizontalHeader()->setStretchLastSection(true);
    recentLayout->addWidget(m_recentTable);
    root->addWidget(recentPanel, 1);

    connect(m_submitButton, &QPushButton::clicked, this, &ProductionIssuePage::submit);
    connect(m_plannedQuantity, qOverload<double>(&QDoubleSpinBox::valueChanged),
            m_lines, &StockLineTable::setProductionQuantity);
    connect(m_productCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ProductionIssuePage::loadProductMaterials);
    resetSubmissionToken();
    refreshReferenceData();
}

void ProductionIssuePage::resetSubmissionToken()
{
    m_submissionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void ProductionIssuePage::refreshReferenceData()
{
    const QVariant selected = m_productCombo->currentData();
    const QSignalBlocker productBlocker(m_productCombo);
    m_productCombo->clear();
    QSqlQuery products(m_database);
    products.exec(QStringLiteral(
        "SELECT m.id,m.code,m.name,m.specification FROM materials m "
        "JOIN material_categories c ON c.id=m.category_id "
        "WHERE m.is_active=1 AND c.code='FINISHED' ORDER BY m.code"));
    while (products.next()) {
        const int index = m_productCombo->count();
        m_productCombo->addItem(QStringLiteral("%1 - %2（%3）")
                                    .arg(products.value(1).toString(), products.value(2).toString(),
                                         products.value(3).toString()),
                                products.value(0));
        m_productCombo->setItemData(index, products.value(1), ProductCodeRole);
    }
    const int selectedIndex = m_productCombo->findData(selected);
    if (selectedIndex >= 0) m_productCombo->setCurrentIndex(selectedIndex);
    m_lines->refreshReferenceData();
    loadProductMaterials();
    m_submitButton->setEnabled(m_session.canPostProduction()
                               && m_productCombo->count() > 0);
    refreshRecentDocuments();
}

void ProductionIssuePage::loadProductMaterials()
{
    if (m_productCombo->currentIndex() < 0) {
        m_lines->clearLines();
        m_usageHint->setText(QStringLiteral("请先选择成品，系统将自动带出生产所需原材料。"));
        return;
    }

    const qlonglong productId = m_productCombo->currentData().toLongLong();
    const QString productCode = m_productCombo->currentData(ProductCodeRole).toString();
    QList<QPair<qlonglong, double>> productionMaterials;
    QSqlQuery materials(m_database);
    materials.prepare(QStringLiteral(
        "WITH RECURSIVE bom_tree(id,component_material_id,total_quantity) AS ("
        " SELECT id,component_material_id,quantity FROM material_bom_items "
        " WHERE product_material_id=? AND parent_item_id IS NULL"
        " UNION ALL"
        " SELECT child.id,child.component_material_id,"
        "        parent.total_quantity*child.quantity"
        " FROM material_bom_items child JOIN bom_tree parent "
        " ON child.parent_item_id=parent.id WHERE child.product_material_id=?"
        ") "
        "SELECT tree.component_material_id,SUM(tree.total_quantity),m.code "
        "FROM bom_tree tree JOIN materials m ON m.id=tree.component_material_id "
        "WHERE m.is_active=1 AND NOT EXISTS("
        " SELECT 1 FROM material_bom_items child WHERE child.parent_item_id=tree.id) "
        "GROUP BY tree.component_material_id,m.code ORDER BY m.code"));
    materials.addBindValue(productId);
    materials.addBindValue(productId);
    if (!materials.exec()) {
        m_lines->clearLines();
        m_usageHint->setText(QStringLiteral("读取成品所需原材料失败，请重新进入页面后再试。"));
        return;
    }
    while (materials.next()) {
        productionMaterials.append({materials.value(0).toLongLong(),
                                    materials.value(1).toDouble()});
    }

    QString error;
    if (!m_lines->setProductionMaterials(productionMaterials, &error)) {
        m_usageHint->setText(error);
        return;
    }
    if (productionMaterials.isEmpty()) {
        m_usageHint->setText(
            QStringLiteral("成品 %1 尚未维护BOM，请先在物料维护的“BOM层级”中导入或添加BOM。")
                .arg(productCode));
        return;
    }
    m_usageHint->setText(
        QStringLiteral("已递归展开成品 %1 的BOM并汇总 %2 种末级物料；总领用量会随生产台数自动更新。")
            .arg(productCode)
            .arg(productionMaterials.size()));
}

void ProductionIssuePage::refreshRecentDocuments()
{
    m_recentTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT d.document_no,d.document_date,p.batch_no,p.product_name,COUNT(i.id) "
        "FROM business_documents d JOIN production_runs p ON p.id=d.production_run_id "
        "JOIN business_document_items i ON i.document_id=d.id "
        "WHERE d.document_type='SCLL' GROUP BY d.id ORDER BY d.id DESC LIMIT 20"));
    while (query.next()) {
        const int row = m_recentTable->rowCount();
        m_recentTable->insertRow(row);
        for (int column = 0; column < 5; ++column) {
            m_recentTable->setItem(row, column,
                                   new QTableWidgetItem(query.value(column).toString()));
        }
    }
}

void ProductionIssuePage::submit()
{
    if (m_productCombo->currentIndex() < 0 || m_batchEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"),
                             QStringLiteral("请选择成品并填写生产批次。"));
        return;
    }
    QString error;
    const QList<StockMovementRequest> movementLines = m_lines->lines(&error);
    if (movementLines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("领料明细有误"), error);
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("确认生产领料"),
            QStringLiteral("确认按 %1 台提交 %2 条领料明细？库存将整单扣减并生成库存流水。")
                .arg(m_plannedQuantity->value(), 0, 'f', 0)
                .arg(movementLines.size())) != QMessageBox::Yes) {
        return;
    }

    ProductionRunRequest run;
    run.batchNo = m_batchEdit->text().trimmed();
    run.productMaterialId = m_productCombo->currentData().toLongLong();
    run.plannedQuantity = m_plannedQuantity->value();
    StockDocumentRequest document;
    document.documentType = QStringLiteral("SCLL");
    document.documentDate = m_dateEdit->date();
    document.handlerName = m_handlerEdit->text().trimmed();
    document.notes = m_notesEdit->toPlainText().trimmed();
    document.submissionToken = m_submissionToken;
    document.lines = movementLines;

    m_submitButton->setEnabled(false);
    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    qlonglong runId = 0;
    const bool ok = service.postProductionIssue(run, document, &posted, &runId, &error);
    m_submitButton->setEnabled(m_session.canPostProduction());
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("生产领料失败"), error);
        return;
    }
    m_numberLabel->setText(posted.documentNumber);
    QMessageBox::information(this, QStringLiteral("生产领料完成"),
                             QStringLiteral("领料单 %1 已生效。")
                                 .arg(posted.documentNumber));
    resetSubmissionToken();
    m_notesEdit->clear();
    emit stockChanged();
    refreshReferenceData();
}
