#include "ui/pages/StockOutPage.h"

#include "services/InventoryService.h"
#include "services/OfficeTemplateService.h"
#include "ui/dialogs/DocumentTemplateDialog.h"
#include "ui/widgets/StockLineTable.h"
#include "ui/widgets/TableExcelExport.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTableWidget>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

StockOutPage::StockOutPage(QSqlDatabase database, Session session, QWidget *parent)
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
    auto *heading = new QLabel(QStringLiteral("新建多物料出库单"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    layout->addWidget(heading);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_typeCombo = new QComboBox(panel);
    m_typeCombo->addItem(QStringLiteral("销售出库"), QStringLiteral("XSCK"));
    m_typeCombo->addItem(QStringLiteral("维修领用"), QStringLiteral("WXLY"));
    m_typeCombo->addItem(QStringLiteral("研发领用"), QStringLiteral("YPLY"));
    m_typeCombo->addItem(QStringLiteral("其他出库"), QStringLiteral("QTCK"));
    m_dateEdit = new QDateEdit(QDate::currentDate(), panel);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_handlerEdit = new QLineEdit(m_session.displayName, panel);
    m_purposeEdit = new QLineEdit(panel);
    m_numberLabel = new QLabel(QStringLiteral("提交时自动生成"), panel);
    m_numberLabel->setObjectName(QStringLiteral("mutedText"));
    m_notesEdit = new QTextEdit(panel);
    m_notesEdit->setMaximumHeight(65);
    form->addRow(QStringLiteral("出库类型 *"), m_typeCombo);
    form->addRow(QStringLiteral("出库日期 *"), m_dateEdit);
    form->addRow(QStringLiteral("经办人员"), m_handlerEdit);
    form->addRow(QStringLiteral("业务用途"), m_purposeEdit);
    form->addRow(QStringLiteral("出库单号"), m_numberLabel);
    form->addRow(QStringLiteral("备注"), m_notesEdit);
    layout->addLayout(form);

    m_salesDetailsGroup = new QGroupBox(QStringLiteral("销售出库信息"), panel);
    auto *salesLayout = new QGridLayout(m_salesDetailsGroup);
    m_customerCompanyEdit = new QLineEdit(m_salesDetailsGroup);
    m_customerCompanyEdit->setPlaceholderText(QStringLiteral("购买方或客户公司全称"));
    m_destinationEdit = new QLineEdit(m_salesDetailsGroup);
    m_destinationEdit->setPlaceholderText(QStringLiteral("城市、园区或完整收货地址"));
    m_customerContactEdit = new QLineEdit(m_salesDetailsGroup);
    m_customerPhoneEdit = new QLineEdit(m_salesDetailsGroup);
    m_salesOrderEdit = new QLineEdit(m_salesDetailsGroup);
    m_logisticsCompanyEdit = new QLineEdit(m_salesDetailsGroup);
    m_trackingNumberEdit = new QLineEdit(m_salesDetailsGroup);
    m_deliveryDateEdit = new QDateEdit(QDate::currentDate(), m_salesDetailsGroup);
    m_deliveryDateEdit->setCalendarPopup(true);
    m_deliveryDateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    salesLayout->addWidget(new QLabel(QStringLiteral("客户公司名称 *"), m_salesDetailsGroup), 0, 0);
    salesLayout->addWidget(m_customerCompanyEdit, 0, 1);
    salesLayout->addWidget(new QLabel(QStringLiteral("销售目的地/收货地址 *"), m_salesDetailsGroup), 0, 2);
    salesLayout->addWidget(m_destinationEdit, 0, 3);
    salesLayout->addWidget(new QLabel(QStringLiteral("客户联系人 *"), m_salesDetailsGroup), 1, 0);
    salesLayout->addWidget(m_customerContactEdit, 1, 1);
    salesLayout->addWidget(new QLabel(QStringLiteral("联系电话 *"), m_salesDetailsGroup), 1, 2);
    salesLayout->addWidget(m_customerPhoneEdit, 1, 3);
    salesLayout->addWidget(new QLabel(QStringLiteral("客户合同号/订单号 *"), m_salesDetailsGroup), 2, 0);
    salesLayout->addWidget(m_salesOrderEdit, 2, 1);
    salesLayout->addWidget(new QLabel(QStringLiteral("物流/快递公司 *"), m_salesDetailsGroup), 2, 2);
    salesLayout->addWidget(m_logisticsCompanyEdit, 2, 3);
    salesLayout->addWidget(new QLabel(QStringLiteral("运单号 *"), m_salesDetailsGroup), 3, 0);
    salesLayout->addWidget(m_trackingNumberEdit, 3, 1);
    salesLayout->addWidget(new QLabel(QStringLiteral("送货日期 *"), m_salesDetailsGroup), 3, 2);
    salesLayout->addWidget(m_deliveryDateEdit, 3, 3);
    salesLayout->setColumnStretch(1, 1);
    salesLayout->setColumnStretch(3, 1);
    layout->addWidget(m_salesDetailsGroup);

    m_lines = new StockLineTable(m_database, StockLineTable::Mode::Outbound, panel);
    layout->addWidget(m_lines);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    m_submitButton = new QPushButton(QStringLiteral("确认并出库"), panel);
    m_submitButton->setProperty("primary", true);
    actions->addWidget(m_submitButton);
    layout->addLayout(actions);
    root->addWidget(panel);

    auto *recentPanel = new QFrame(this);
    recentPanel->setObjectName(QStringLiteral("panel"));
    auto *recentLayout = new QVBoxLayout(recentPanel);
    auto *recentToolbar = new QHBoxLayout;
    recentToolbar->addWidget(new QLabel(
        QStringLiteral("近期普通出库单（双击销售出库可查看完整销售资料）"), recentPanel));
    recentToolbar->addStretch();
    auto *fullScreenRecentButton = new QPushButton(QStringLiteral("全屏显示"), recentPanel);
    recentToolbar->addWidget(fullScreenRecentButton);
    recentLayout->addLayout(recentToolbar);
    m_recentTable = new QTableWidget(0, 6, recentPanel);
    m_recentTable->setProperty("businessDocumentTable", true);
    m_recentTable->setHorizontalHeaderLabels({QStringLiteral("单据号"), QStringLiteral("类型"),
                                              QStringLiteral("送货/单据日期"), QStringLiteral("客户公司"),
                                              QStringLiteral("销售目的地"), QStringLiteral("明细数")});
    m_recentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_recentTable->horizontalHeader()->setStretchLastSection(true);
    recentLayout->addWidget(m_recentTable);
    root->addWidget(recentPanel, 1);

    connect(fullScreenRecentButton, &QPushButton::clicked, this, [this] {
        TableExcelExport::fullScreenTable(
            m_recentTable, QStringLiteral("近期普通出库单"), this);
    });
    connect(m_submitButton, &QPushButton::clicked, this, &StockOutPage::submit);
    connect(m_typeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &StockOutPage::updateSalesFieldsVisibility);
    connect(m_recentTable, &QTableWidget::cellDoubleClicked,
            this, &StockOutPage::showSalesDetails);
    updateSalesFieldsVisibility();
    resetSubmissionToken();
    refreshReferenceData();
}

void StockOutPage::updateSalesFieldsVisibility()
{
    const QString documentType = m_typeCombo->currentData().toString();
    const bool salesOutbound = documentType == QStringLiteral("XSCK");
    m_salesDetailsGroup->setVisible(salesOutbound);
    const bool finishedGoodsOnly = documentType == QStringLiteral("XSCK")
        || documentType == QStringLiteral("WXLY")
        || documentType == QStringLiteral("YPLY");
    m_lines->setMaterialCategoryFilter(
        finishedGoodsOnly ? QStringLiteral("FINISHED") : QString());
}

void StockOutPage::resetSubmissionToken()
{
    m_submissionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void StockOutPage::refreshReferenceData()
{
    m_lines->refreshReferenceData();
    m_submitButton->setEnabled(m_session.canManageWarehouse());
    refreshRecentDocuments();
}

void StockOutPage::refreshRecentDocuments()
{
    m_recentTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT d.id,d.document_no,CASE d.document_type "
        "WHEN 'XSCK' THEN '销售出库' WHEN 'WXLY' THEN '维修领用' "
        "WHEN 'YPLY' THEN '研发领用' ELSE '其他出库' END,"
        "COALESCE(NULLIF(s.delivery_date,''),d.document_date),"
        "COALESCE(s.customer_company,''),COALESCE(s.destination,''),COUNT(i.id) "
        "FROM business_documents d LEFT JOIN business_document_items i ON i.document_id=d.id "
        "LEFT JOIN sales_outbound_details s ON s.document_id=d.id "
        "WHERE d.stock_direction='OUT' AND d.document_type IN ('XSCK','WXLY','YPLY','QTCK') "
        "GROUP BY d.id ORDER BY d.id DESC LIMIT 20"));
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

void StockOutPage::showSalesDetails(int row, int)
{
    const QTableWidgetItem *item = m_recentTable->item(row, 0);
    if (!item) return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT d.document_no,s.customer_company,s.destination,s.contact_name,s.contact_phone,"
        "s.sales_order_no,s.logistics_company,s.tracking_no,"
        "COALESCE(NULLIF(s.delivery_date,''),d.document_date) "
        "FROM business_documents d JOIN sales_outbound_details s ON s.document_id=d.id "
        "WHERE d.id=? AND d.document_type='XSCK'"));
    query.addBindValue(item->data(Qt::UserRole));
    if (!query.exec()) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), query.lastError().text());
        return;
    }
    if (!query.next()) {
        QMessageBox::information(this, QStringLiteral("无销售资料"),
                                 QStringLiteral("该单据不是销售出库，没有额外销售资料。"));
        return;
    }
    auto valueOrEmpty = [&query](int column) {
        const QString value = query.value(column).toString().trimmed();
        return value.isEmpty() ? QStringLiteral("未填写") : value;
    };
    QMessageBox::information(this, QStringLiteral("销售出库资料"),
        QStringLiteral("出库单号：%1\n送货日期：%2\n客户公司：%3\n销售目的地/收货地址：%4\n"
                       "客户联系人：%5\n联系电话：%6\n销售订单号：%7\n"
                       "物流/快递公司：%8\n运单号：%9")
            .arg(valueOrEmpty(0), valueOrEmpty(8), valueOrEmpty(1), valueOrEmpty(2),
                 valueOrEmpty(3), valueOrEmpty(4), valueOrEmpty(5), valueOrEmpty(6),
                 valueOrEmpty(7)));
}

void StockOutPage::submit()
{
    const bool salesOutbound = m_typeCombo->currentData().toString() == QStringLiteral("XSCK");
    if (salesOutbound) {
        QStringList missing;
        if (m_customerCompanyEdit->text().trimmed().isEmpty())
            missing.append(QStringLiteral("客户公司名称"));
        if (m_destinationEdit->text().trimmed().isEmpty())
            missing.append(QStringLiteral("销售目的地/收货地址"));
        if (m_customerContactEdit->text().trimmed().isEmpty())
            missing.append(QStringLiteral("客户联系人"));
        if (m_customerPhoneEdit->text().trimmed().isEmpty())
            missing.append(QStringLiteral("联系电话"));
        if (m_salesOrderEdit->text().trimmed().isEmpty())
            missing.append(QStringLiteral("客户合同号/订单号"));
        if (m_logisticsCompanyEdit->text().trimmed().isEmpty())
            missing.append(QStringLiteral("物流/快递公司"));
        if (m_trackingNumberEdit->text().trimmed().isEmpty())
            missing.append(QStringLiteral("运单号"));
        if (!m_deliveryDateEdit->date().isValid())
            missing.append(QStringLiteral("送货日期"));
        if (!missing.isEmpty()) {
            QMessageBox::warning(
                this, QStringLiteral("销售资料不完整"),
                QStringLiteral("销售出库必须完整填写以下带 * 的发货资料：\n\n%1\n\n"
                               "请补充后再提交，其他出库类型不受影响。")
                    .arg(missing.join(QStringLiteral("、"))));
            return;
        }
    }
    QString error;
    const QList<StockMovementRequest> lines = m_lines->lines(&error);
    if (lines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("出库明细有误"), error);
        return;
    }

    OfficeTemplateDocument outboundForm;
    outboundForm.kind = OfficeFormKind::StockOutbound;
    outboundForm.documentNumber = QStringLiteral("提交后自动生成");
    outboundForm.documentDate = m_dateEdit->date();
    outboundForm.fields.insert(QStringLiteral("handler"), m_handlerEdit->text().trimmed());
    outboundForm.fields.insert(QStringLiteral("purpose"), m_purposeEdit->text().trimmed());
    if (salesOutbound) {
        outboundForm.fields.insert(QStringLiteral("customerCompany"),
                                   m_customerCompanyEdit->text().trimmed());
        outboundForm.fields.insert(QStringLiteral("destination"),
                                   m_destinationEdit->text().trimmed());
        outboundForm.fields.insert(QStringLiteral("customerContact"),
                                   m_customerContactEdit->text().trimmed());
        outboundForm.fields.insert(QStringLiteral("customerPhone"),
                                   m_customerPhoneEdit->text().trimmed());
        outboundForm.fields.insert(QStringLiteral("salesOrderNumber"),
                                   m_salesOrderEdit->text().trimmed());
        outboundForm.fields.insert(QStringLiteral("logisticsCompany"),
                                   m_logisticsCompanyEdit->text().trimmed());
        outboundForm.fields.insert(QStringLiteral("trackingNumber"),
                                   m_trackingNumberEdit->text().trimmed());
    }
    outboundForm.lines = OfficeTemplateService::materialLines(m_database, lines, &error);
    if (outboundForm.lines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法填写出库单模板"), error);
        return;
    }
    DocumentTemplateDialog outboundDialog(outboundForm, this);
    if (outboundDialog.exec() != QDialog::Accepted) return;
    outboundForm = outboundDialog.document();

    OfficeTemplateDocument deliveryForm;
    if (salesOutbound) {
        deliveryForm = outboundForm;
        deliveryForm.kind = OfficeFormKind::DeliveryConfirmation;
        // 送货确认单上的“送货日期”取所选送货日期；出库单/单据日期仍是出库日期。
        deliveryForm.documentDate = m_deliveryDateEdit->date();
        for (OfficeTemplateLine &line : deliveryForm.lines)
            line.orderNumber = m_salesOrderEdit->text().trimmed();
        DocumentTemplateDialog deliveryDialog(deliveryForm, this);
        if (deliveryDialog.exec() != QDialog::Accepted) return;
        deliveryForm = deliveryDialog.document();
        if (!deliveryForm.documentDate.isValid()) {
            QMessageBox::warning(this, QStringLiteral("送货日期无效"),
                                 QStringLiteral("送货确认单的送货日期无效，请重新选择送货日期。"));
            return;
        }
    }

    if (QMessageBox::question(this, QStringLiteral("确认出库"),
        QStringLiteral("确认提交 %1 条出库明细？库存将整单扣减并生成流水。")
            .arg(lines.size())) != QMessageBox::Yes) return;
    StockDocumentRequest request;
    request.documentType = m_typeCombo->currentData().toString();
    request.documentDate = m_dateEdit->date();
    request.handlerName = m_handlerEdit->text().trimmed();
    request.purpose = m_purposeEdit->text().trimmed();
    if (salesOutbound) {
        // 与送货确认单模板保持同一天，避免保存数据与生成表单不一致。
        request.deliveryDate = deliveryForm.documentDate;
        request.customerCompany = m_customerCompanyEdit->text().trimmed();
        request.destination = m_destinationEdit->text().trimmed();
        request.customerContact = m_customerContactEdit->text().trimmed();
        request.customerPhone = m_customerPhoneEdit->text().trimmed();
        request.salesOrderNumber = m_salesOrderEdit->text().trimmed();
        request.logisticsCompany = m_logisticsCompanyEdit->text().trimmed();
        request.trackingNumber = m_trackingNumberEdit->text().trimmed();
    }
    request.notes = m_notesEdit->toPlainText().trimmed();
    request.submissionToken = m_submissionToken;
    request.lines = lines;
    m_submitButton->setEnabled(false);
    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    const bool ok = service.postStockDocument(request, false, &posted, &error);
    m_submitButton->setEnabled(m_session.canManageWarehouse());
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("出库失败"), error);
        return;
    }
    m_numberLabel->setText(posted.documentNumber);
    outboundForm.documentNumber = posted.documentNumber;
    QStringList formErrors;
    QString formError;
    if (!OfficeTemplateService::attachToDocument(outboundForm, m_database, m_session.userId,
                                                  posted.documentId, &formError)) {
        formErrors.append(QStringLiteral("出库单：%1").arg(formError));
    }
    if (salesOutbound) {
        deliveryForm.documentNumber = posted.documentNumber;
        formError.clear();
        if (!OfficeTemplateService::attachToDocument(deliveryForm, m_database, m_session.userId,
                                                      posted.documentId, &formError)) {
            formErrors.append(QStringLiteral("送货确认单：%1").arg(formError));
        }
    }
    if (formErrors.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("出库完成"),
            QStringLiteral("出库单 %1 已生效，模板表单已保存到数据库附件和“我的文档\\冰美肌仓库系统表单”分类文件夹，并已自动打开。")
                .arg(posted.documentNumber));
    } else {
        QMessageBox::warning(
            this, QStringLiteral("出库已完成，但模板处理未全部完成"),
            QStringLiteral("出库单 %1 已生效，但以下保存或打开步骤未完成：\n\n%2")
                .arg(posted.documentNumber, formErrors.join(QStringLiteral("\n"))));
    }
    resetSubmissionToken();
    m_notesEdit->clear();
    if (salesOutbound) {
        m_customerCompanyEdit->clear();
        m_destinationEdit->clear();
        m_customerContactEdit->clear();
        m_customerPhoneEdit->clear();
        m_salesOrderEdit->clear();
        m_logisticsCompanyEdit->clear();
        m_trackingNumberEdit->clear();
        m_deliveryDateEdit->setDate(QDate::currentDate());
    }
    m_lines->clearLines();
    emit stockChanged();
    refreshReferenceData();
}
