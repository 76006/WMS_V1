#include "ui/pages/StockInPage.h"

#include "services/InventoryService.h"
#include "services/OfficeTemplateService.h"
#include "ui/dialogs/DocumentTemplateDialog.h"
#include "ui/dialogs/InspectionDialog.h"
#include "ui/widgets/StockLineTable.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
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

StockInPage::StockInPage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(20, 18, 20, 20);
    auto *heading = new QLabel(QStringLiteral("在线填写入库单"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    layout->addWidget(heading);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_typeCombo = new QComboBox(panel);
    m_typeCombo->addItem(QStringLiteral("采购入库"), QStringLiteral("CGRK"));
    m_typeCombo->addItem(QStringLiteral("生产完工入库"), QStringLiteral("SCWG"));
    m_typeCombo->addItem(QStringLiteral("退料入库"), QStringLiteral("TLRK"));
    m_typeCombo->addItem(QStringLiteral("其他入库"), QStringLiteral("QTRK"));
    m_typeCombo->addItem(QStringLiteral("期初入库"), QStringLiteral("QC"));
    m_dateEdit = new QDateEdit(QDate::currentDate(), panel);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_handlerEdit = new QLineEdit(m_session.displayName, panel);
    m_supplierEdit = new QLineEdit(panel);
    m_supplierEdit->setPlaceholderText(QStringLiteral("采购入库时填写，可用于批次追溯"));
    m_purposeEdit = new QLineEdit(panel);
    m_numberLabel = new QLabel(QStringLiteral("提交时自动生成"), panel);
    m_numberLabel->setObjectName(QStringLiteral("mutedText"));
    m_inspectionCombo = new QComboBox(panel);
    m_inspectionCombo->addItem(QStringLiteral("否（直接入库）"), false);
    m_inspectionCombo->addItem(QStringLiteral("是（先填写送检单）"), true);
    m_inspectionButton = new QPushButton(QStringLiteral("填写送检单"), panel);
    m_inspectionStatusLabel = new QLabel(QStringLiteral("无需送检"), panel);
    m_inspectionStatusLabel->setObjectName(QStringLiteral("mutedText"));
    auto *inspectionRow = new QWidget(panel);
    auto *inspectionLayout = new QHBoxLayout(inspectionRow);
    inspectionLayout->setContentsMargins(0, 0, 0, 0);
    inspectionLayout->addWidget(m_inspectionCombo);
    inspectionLayout->addWidget(m_inspectionButton);
    inspectionLayout->addWidget(m_inspectionStatusLabel, 1);
    m_notesEdit = new QTextEdit(panel);
    m_notesEdit->setMaximumHeight(65);
    form->addRow(QStringLiteral("入库类型 *"), m_typeCombo);
    form->addRow(QStringLiteral("入库日期 *"), m_dateEdit);
    form->addRow(QStringLiteral("经办人员"), m_handlerEdit);
    form->addRow(QStringLiteral("供应商"), m_supplierEdit);
    form->addRow(QStringLiteral("业务用途"), m_purposeEdit);
    form->addRow(QStringLiteral("是否送检 *"), inspectionRow);
    form->addRow(QStringLiteral("入库单号"), m_numberLabel);
    form->addRow(QStringLiteral("备注"), m_notesEdit);
    layout->addLayout(form);
    m_lines = new StockLineTable(m_database, StockLineTable::Mode::Inbound, panel);
    m_lines->setPurchaseMode(true);
    layout->addWidget(m_lines);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    m_submitButton = new QPushButton(QStringLiteral("确认并入库"), panel);
    m_submitButton->setProperty("primary", true);
    actions->addWidget(m_submitButton);
    layout->addLayout(actions);
    root->addWidget(panel);

    auto *recentPanel = new QFrame(this);
    recentPanel->setObjectName(QStringLiteral("panel"));
    auto *recentLayout = new QVBoxLayout(recentPanel);
    recentLayout->addWidget(new QLabel(QStringLiteral("近期在线入库单"), recentPanel));
    m_recentTable = new QTableWidget(0, 8, recentPanel);
    m_recentTable->setHorizontalHeaderLabels({QStringLiteral("单据号"), QStringLiteral("类型"),
                                              QStringLiteral("日期"), QStringLiteral("是否送检"),
                                              QStringLiteral("送检单号"), QStringLiteral("检验结果"),
                                              QStringLiteral("检验附件"), QStringLiteral("明细数")});
    m_recentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recentTable->horizontalHeader()->setStretchLastSection(true);
    recentLayout->addWidget(m_recentTable);
    root->addWidget(recentPanel, 1);

    connect(m_submitButton, &QPushButton::clicked, this, &StockInPage::submit);
    connect(m_inspectionCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &StockInPage::updateInspectionRequirement);
    connect(m_inspectionButton, &QPushButton::clicked,
            this, &StockInPage::openInspectionForm);
    connect(m_typeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const bool purchase = m_typeCombo->currentData().toString() == QStringLiteral("CGRK");
        m_lines->setPurchaseMode(purchase);
        m_supplierEdit->setEnabled(purchase);
        if (!purchase) m_supplierEdit->clear();
    });
    updateInspectionRequirement();
    resetSubmissionToken();
    refreshReferenceData();
}

void StockInPage::resetSubmissionToken()
{
    m_submissionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void StockInPage::refreshReferenceData()
{
    m_lines->refreshReferenceData();
    m_submitButton->setEnabled(m_session.canManageWarehouse());
    refreshRecentDocuments();
}

void StockInPage::refreshRecentDocuments()
{
    m_recentTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT d.document_no,d.document_type,d.document_date,"
        "CASE WHEN COALESCE(q.requires_inspection,0)=1 THEN '是' ELSE '否' END,"
        "COALESCE(q.inspection_no,''),"
        "CASE COALESCE(q.inspection_result,'NOT_REQUIRED') "
        "WHEN 'QUALIFIED' THEN '合格' WHEN 'UNQUALIFIED' THEN '不合格' "
        "WHEN 'PENDING' THEN '待检验' ELSE '无需送检' END,"
        "COALESCE(a.original_file_name,''),COUNT(i.id) "
        "FROM business_documents d LEFT JOIN business_document_items i ON i.document_id=d.id "
        "LEFT JOIN inbound_inspection_details q ON q.document_id=d.id "
        "LEFT JOIN attachments a ON a.id=q.inspection_attachment_id AND a.is_deleted=0 "
        "WHERE d.stock_direction='IN' AND d.document_type IN ('CGRK','SCWG','TLRK','QTRK','QC') "
        "GROUP BY d.id ORDER BY d.id DESC LIMIT 20"));
    while (query.next()) {
        const int row = m_recentTable->rowCount();
        m_recentTable->insertRow(row);
        for (int column = 0; column < 8; ++column)
            m_recentTable->setItem(row, column, new QTableWidgetItem(query.value(column).toString()));
    }
    m_recentTable->resizeColumnsToContents();
}

void StockInPage::updateInspectionRequirement()
{
    const bool required = m_inspectionCombo->currentData().toBool();
    m_inspectionButton->setVisible(required);
    if (!required) {
        m_inspection = InboundInspectionRequest{};
        m_inspectionStatusLabel->setText(QStringLiteral("无需送检，可直接提交入库"));
        return;
    }
    m_inspection.required = true;
    if (m_inspection.inspectorName.trimmed().isEmpty()) {
        m_inspection.inspectorName = m_handlerEdit->text().trimmed();
    }
    m_inspectionStatusLabel->setText(QStringLiteral("尚未完成合格送检单"));
    openInspectionForm();
}

void StockInPage::openInspectionForm()
{
    if (!m_inspectionCombo->currentData().toBool()) return;
    m_inspection.templateFields.insert(QStringLiteral("supplier"),
                                       m_supplierEdit->text().trimmed());
    m_inspection.templateFields.insert(QStringLiteral("purchaseOrderNumber"),
                                       m_purposeEdit->text().trimmed());
    m_inspection.templateFields.insert(QStringLiteral("arrivalDate"),
                                       m_dateEdit->date().toString(QStringLiteral("yyyy-MM-dd")));
    if (m_inspection.templateFields.value(QStringLiteral("entrustedBy")).isEmpty()) {
        m_inspection.templateFields.insert(QStringLiteral("entrustedBy"),
                                           m_handlerEdit->text().trimmed());
    }
    QString ignoredError;
    const QList<StockMovementRequest> currentLines = m_lines->lines(&ignoredError);
    InspectionDialog dialog(m_database, currentLines, m_inspection, this);
    if (dialog.exec() != QDialog::Accepted) return;
    m_inspection = dialog.inspection();
    QString resultText = QStringLiteral("待检验");
    if (m_inspection.result == QStringLiteral("QUALIFIED")) resultText = QStringLiteral("合格");
    else if (m_inspection.result == QStringLiteral("UNQUALIFIED")) resultText = QStringLiteral("不合格");
    const QString attachmentText = m_inspection.attachmentFileName.trimmed().isEmpty()
        ? QStringLiteral("未上传附件")
        : QStringLiteral("附件：%1").arg(m_inspection.attachmentFileName);
    m_inspectionStatusLabel->setText(QStringLiteral("%1｜%2｜%3")
                                         .arg(m_inspection.inspectionNumber, resultText,
                                              attachmentText));
    m_inspectionButton->setText(QStringLiteral("查看/修改送检单"));
}

void StockInPage::submit()
{
    QString error;
    const QList<StockMovementRequest> lines = m_lines->lines(&error);
    if (lines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("入库明细有误"), error);
        return;
    }
    const bool requiresInspection = m_inspectionCombo->currentData().toBool();
    if (requiresInspection
        && (m_inspection.result != QStringLiteral("QUALIFIED")
            || m_inspection.attachmentFileName.trimmed().isEmpty()
            || m_inspection.attachmentData.isEmpty())) {
        QMessageBox::warning(this, QStringLiteral("送检尚未完成"),
                             QStringLiteral("需要送检的入库单，必须检验合格并上传检验附件后才能入库。"));
        openInspectionForm();
        return;
    }

    OfficeTemplateDocument inboundForm;
    inboundForm.kind = m_typeCombo->currentData().toString() == QStringLiteral("SCWG")
        ? OfficeFormKind::FinishedGoodsInbound
        : OfficeFormKind::RawMaterialInbound;
    inboundForm.documentDate = m_dateEdit->date();
    inboundForm.documentNumber = QStringLiteral("提交后自动生成");
    inboundForm.fields.insert(QStringLiteral("handler"), m_handlerEdit->text().trimmed());
    inboundForm.fields.insert(QStringLiteral("supplier"), m_supplierEdit->text().trimmed());
    inboundForm.fields.insert(QStringLiteral("purpose"), m_purposeEdit->text().trimmed());
    inboundForm.lines = OfficeTemplateService::materialLines(m_database, lines, &error);
    if (inboundForm.lines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法填写入库单模板"), error);
        return;
    }
    DocumentTemplateDialog inboundDialog(inboundForm, this);
    if (inboundDialog.exec() != QDialog::Accepted) return;
    inboundForm = inboundDialog.document();

    QString confirmation = QStringLiteral("确认提交 %1 条入库明细？库存将整单增加并生成流水。")
                               .arg(lines.size());
    if (m_typeCombo->currentData().toString() == QStringLiteral("CGRK")) {
        double ordered = 0.0;
        double received = 0.0;
        double gifted = 0.0;
        for (const StockMovementRequest &line : lines) {
            ordered += line.orderedQuantity;
            received += line.quantity;
            gifted += line.giftQuantity;
        }
        confirmation += QStringLiteral("\n\n采购数量：%1\n实际入库：%2\n其中赠送：%3\n对账数量：%4")
                            .arg(ordered, 0, 'g', 12)
                            .arg(received, 0, 'g', 12)
                            .arg(gifted, 0, 'g', 12)
                            .arg(received - gifted, 0, 'g', 12);
        const QStringList warnings = m_lines->purchaseWarnings();
        if (!warnings.isEmpty()) {
            confirmation += QStringLiteral("\n\n请注意：\n• ")
                                + warnings.join(QStringLiteral("\n• "));
        }
    }
    if (requiresInspection) {
        confirmation += QStringLiteral("\n\n送检单：%1\n检验结果：合格\n检验附件：%2")
                            .arg(m_inspection.inspectionNumber,
                                 m_inspection.attachmentFileName);
    } else {
        confirmation += QStringLiteral("\n\n该入库单选择无需送检。");
    }
    if (QMessageBox::question(this, QStringLiteral("确认入库"), confirmation)
        != QMessageBox::Yes) return;
    StockDocumentRequest request;
    request.documentType = m_typeCombo->currentData().toString();
    request.documentDate = m_dateEdit->date();
    request.handlerName = m_handlerEdit->text().trimmed();
    request.supplier = request.documentType == QStringLiteral("CGRK")
        ? m_supplierEdit->text().trimmed() : QString();
    request.purpose = m_purposeEdit->text().trimmed();
    request.notes = m_notesEdit->toPlainText().trimmed();
    request.submissionToken = m_submissionToken;
    request.inspection = requiresInspection ? m_inspection : InboundInspectionRequest{};
    request.lines = lines;
    m_submitButton->setEnabled(false);
    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    const bool ok = service.postStockDocument(request, true, &posted, &error);
    m_submitButton->setEnabled(m_session.canManageWarehouse());
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("入库失败"), error);
        return;
    }
    m_numberLabel->setText(posted.documentNumber);
    inboundForm.documentNumber = posted.documentNumber;
    QStringList formErrors;
    QString formError;
    if (!OfficeTemplateService::attachToDocument(inboundForm, m_database, m_session.userId,
                                                  posted.documentId, &formError)) {
        formErrors.append(QStringLiteral("%1：%2")
                              .arg(OfficeTemplateService::formTitle(inboundForm.kind), formError));
    }
    if (requiresInspection) {
        OfficeTemplateDocument inspectionForm;
        inspectionForm.kind = OfficeFormKind::Inspection;
        inspectionForm.documentNumber = m_inspection.inspectionNumber;
        inspectionForm.documentDate = m_inspection.inspectionDate;
        inspectionForm.fields = m_inspection.templateFields;
        inspectionForm.fields.insert(QStringLiteral("inspectionDate"),
                                     m_inspection.inspectionDate.toString(QStringLiteral("yyyy-MM-dd")));
        inspectionForm.fields.insert(
            QStringLiteral("inspectionResult"),
            m_inspection.result == QStringLiteral("QUALIFIED") ? QStringLiteral("合格")
                                                                 : QStringLiteral("不合格"));
        inspectionForm.fields.insert(QStringLiteral("conclusion"), m_inspection.conclusion);
        inspectionForm.lines = OfficeTemplateService::materialLines(m_database, lines, &formError);
        for (OfficeTemplateLine &line : inspectionForm.lines) {
            line.orderNumber = m_inspection.templateFields.value(QStringLiteral("purchaseOrderNumber"));
            line.supplier = m_inspection.templateFields.value(QStringLiteral("supplier"));
        }
        formError.clear();
        if (!OfficeTemplateService::attachToDocument(inspectionForm, m_database, m_session.userId,
                                                      posted.documentId, &formError)) {
            formErrors.append(QStringLiteral("送检单：%1").arg(formError));
        }
    }
    if (formErrors.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("入库完成"),
            QStringLiteral("入库单 %1 已生效，模板表单已保存到数据库附件和“我的文档\\冰美肌仓库系统表单”分类文件夹，并已自动打开。")
                .arg(posted.documentNumber));
    } else {
        QMessageBox::warning(
            this, QStringLiteral("入库已完成，但模板处理未全部完成"),
            QStringLiteral("入库单 %1 已生效，但以下保存或打开步骤未完成：\n\n%2")
                .arg(posted.documentNumber, formErrors.join(QStringLiteral("\n"))));
    }
    resetSubmissionToken();
    m_supplierEdit->clear();
    m_notesEdit->clear();
    m_lines->clearLines();
    m_inspectionCombo->setCurrentIndex(0);
    m_inspectionButton->setText(QStringLiteral("填写送检单"));
    emit stockChanged();
    refreshReferenceData();
}
