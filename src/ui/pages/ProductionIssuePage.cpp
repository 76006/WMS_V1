#include "ui/pages/ProductionIssuePage.h"

#include "services/InventoryService.h"
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

    m_productionFieldsGroup = new QGroupBox(QStringLiteral("生产批次信息（可选）"), panel);
    auto *productionForm = new QFormLayout(m_productionFieldsGroup);
    productionForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    productionForm->addRow(QStringLiteral("生产批次"), m_batchEdit);
    productionForm->addRow(QStringLiteral("生产台数"), m_plannedQuantity);
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
        QStringLiteral("领料明细默认保持为空。可点击“添加物料”逐项选择，也可点击“一键导入BOM用料”主动带出当前成品的全部末级用料；库存批次由用户指定。"),
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
    connect(m_plannedQuantity, qOverload<double>(&QDoubleSpinBox::valueChanged),
            m_lines, &StockLineTable::setProductionQuantity);
    connect(m_lines, &StockLineTable::productionBomRequested,
            this, &ProductionIssuePage::importProductBom);
    connect(m_issueTypeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        m_lines->clearLines();
        updateIssueType();
    });
    connect(m_productCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        m_lines->clearLines();
        if (m_productCombo->currentData().toLongLong() <= 0) m_batchEdit->clear();
        m_productModelEdit->setText(
            m_productCombo->currentData(ProductModelRole).toString());
        updateIssueType();
        m_usageHint->setText(
            QStringLiteral("领料明细已清空。可逐项添加物料，或点击“一键导入BOM用料”。"));
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
    m_plannedQuantity->setEnabled(hasProductionProduct);
    m_lines->setProductionUsageMode(hasProductionProduct);
    m_lines->setMaterialCategoryFilter(QString());
    m_heading->setText(QStringLiteral("新建%1单")
                           .arg(m_issueTypeCombo->currentText().section(QStringLiteral("（"), 0, 0)));
    m_usageHint->setText(
        production
            ? (hasProductionProduct
                ? QStringLiteral("已填写生产信息：可逐项添加物料，也可点击“一键导入BOM用料”；总用量按生产台数计算。")
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
    updateIssueType();
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
        "GROUP BY d.id ORDER BY d.id DESC")
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
    if (production) {
        m_batchEdit->clear();
        m_productCombo->setCurrentIndex(0);
    }
    m_lines->clearLines();
    emit stockChanged();
    refreshReferenceData();
}
