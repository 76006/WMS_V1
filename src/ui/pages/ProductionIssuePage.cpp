#include "ui/pages/ProductionIssuePage.h"

#include "services/InventoryService.h"
#include "services/OfficeTemplateService.h"
#include "ui/dialogs/DocumentTemplateDialog.h"
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
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

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
        QStringLiteral("领料明细默认保持为空。可点击“添加物料”逐项选择，也可点击“一键导入BOM用料”主动带出当前成品的全部末级用料；库存批次由用户指定。"),
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
    connect(m_lines, &StockLineTable::productionBomRequested,
            this, &ProductionIssuePage::importProductBom);
    connect(m_productCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        m_lines->clearLines();
        m_usageHint->setText(
            QStringLiteral("领料明细已清空。可逐项添加物料，或点击“一键导入BOM用料”。"));
    });
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
        m_productCombo->addItem(QStringLiteral("%1 - %2（%3）")
                                    .arg(products.value(1).toString(), products.value(2).toString(),
                                         products.value(3).toString()),
                                products.value(0));
    }
    const int selectedIndex = m_productCombo->findData(selected);
    if (selectedIndex >= 0) m_productCombo->setCurrentIndex(selectedIndex);
    m_lines->refreshReferenceData();
    m_submitButton->setEnabled(m_session.canPostProduction()
                               && m_productCombo->count() > 0);
    refreshRecentDocuments();
}

void ProductionIssuePage::importProductBom()
{
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

    OfficeTemplateDocument issueForm;
    issueForm.kind = OfficeFormKind::ProductionIssue;
    issueForm.documentNumber = QStringLiteral("提交后自动生成");
    issueForm.documentDate = m_dateEdit->date();
    issueForm.fields.insert(QStringLiteral("handler"), m_handlerEdit->text().trimmed());
    issueForm.fields.insert(QStringLiteral("productionBatch"), m_batchEdit->text().trimmed());
    issueForm.fields.insert(QStringLiteral("plannedQuantity"),
                            QString::number(m_plannedQuantity->value(), 'g', 12));
    QSqlQuery product(m_database);
    product.prepare(QStringLiteral("SELECT name,specification FROM materials WHERE id=?"));
    product.addBindValue(m_productCombo->currentData());
    if (product.exec() && product.next()) {
        issueForm.fields.insert(QStringLiteral("productName"), product.value(0).toString());
        issueForm.fields.insert(QStringLiteral("productModel"), product.value(1).toString());
    }
    issueForm.lines = OfficeTemplateService::materialLines(m_database, movementLines, &error);
    if (issueForm.lines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法填写领料单模板"), error);
        return;
    }
    for (OfficeTemplateLine &line : issueForm.lines)
        line.unitUsage = line.quantity / m_plannedQuantity->value();
    DocumentTemplateDialog issueDialog(issueForm, this);
    if (issueDialog.exec() != QDialog::Accepted) return;
    issueForm = issueDialog.document();

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
    issueForm.documentNumber = posted.documentNumber;
    QString formError;
    if (OfficeTemplateService::attachToDocument(issueForm, m_database, m_session.userId,
                                                posted.documentId, &formError)) {
        QMessageBox::information(
            this, QStringLiteral("生产领料完成"),
            QStringLiteral("领料单 %1 已生效，模板表单已保存到数据库附件和“我的文档\\冰美肌仓库系统表单\\领料单”，并已自动打开。")
                .arg(posted.documentNumber));
    } else {
        QMessageBox::warning(
            this, QStringLiteral("领料已完成，但模板处理未全部完成"),
            QStringLiteral("领料单 %1 已生效，但以下保存或打开步骤未完成：\n\n%2")
                .arg(posted.documentNumber, formError));
    }
    resetSubmissionToken();
    m_notesEdit->clear();
    m_lines->clearLines();
    emit stockChanged();
    refreshReferenceData();
}
