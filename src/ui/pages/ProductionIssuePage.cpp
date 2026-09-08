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
    m_plannedQuantity->setDecimals(6);
    m_plannedQuantity->setRange(0.000001, 999999999999.0);
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
    headerForm->addRow(QStringLiteral("计划生产数量 *"), m_plannedQuantity);
    headerForm->addRow(QStringLiteral("领料日期 *"), m_dateEdit);
    headerForm->addRow(QStringLiteral("领料人员"), m_handlerEdit);
    headerForm->addRow(QStringLiteral("领料单号"), m_numberLabel);
    headerForm->addRow(QStringLiteral("备注"), m_notesEdit);
    panelLayout->addLayout(headerForm);

    m_lines = new StockLineTable(m_database, StockLineTable::Mode::Outbound, panel);
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
            QStringLiteral("确认提交 %1 条领料明细？库存将整单扣减并生成库存流水。")
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
