#include "ui/pages/StockOutPage.h"

#include "services/InventoryService.h"
#include "services/OfficeTemplateService.h"
#include "ui/dialogs/DocumentTemplateDialog.h"
#include "ui/widgets/StockLineTable.h"
#include "ui/widgets/TableExcelExport.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
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
#include <QScrollArea>
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
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(20, 18, 20, 20);
    auto *heading = new QLabel(QStringLiteral("新建多物料出库单"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    layout->addWidget(heading);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_typeCombo = new QComboBox(panel);
    m_typeCombo->addItem(QStringLiteral("销售出库"), QStringLiteral("XSCK"));
    m_dateEdit = new QDateEdit(QDate::currentDate(), panel);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_handlerEdit = new QLineEdit(m_session.displayName, panel);
    m_handlerEdit->setProperty("currentUserDefault", true);
    m_handlerEdit->setProperty("lastCurrentUserDefault", m_session.displayName);
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
    salesLayout->addWidget(new QLabel(QStringLiteral("客户公司名称"), m_salesDetailsGroup), 0, 0);
    salesLayout->addWidget(m_customerCompanyEdit, 0, 1);
    salesLayout->addWidget(new QLabel(QStringLiteral("销售目的地/收货地址"), m_salesDetailsGroup), 0, 2);
    salesLayout->addWidget(m_destinationEdit, 0, 3);
    salesLayout->addWidget(new QLabel(QStringLiteral("客户联系人"), m_salesDetailsGroup), 1, 0);
    salesLayout->addWidget(m_customerContactEdit, 1, 1);
    salesLayout->addWidget(new QLabel(QStringLiteral("联系电话"), m_salesDetailsGroup), 1, 2);
    salesLayout->addWidget(m_customerPhoneEdit, 1, 3);
    salesLayout->addWidget(new QLabel(QStringLiteral("客户合同号/订单号"), m_salesDetailsGroup), 2, 0);
    salesLayout->addWidget(m_salesOrderEdit, 2, 1);
    salesLayout->addWidget(new QLabel(QStringLiteral("物流/快递公司"), m_salesDetailsGroup), 2, 2);
    salesLayout->addWidget(m_logisticsCompanyEdit, 2, 3);
    salesLayout->addWidget(new QLabel(QStringLiteral("运单号"), m_salesDetailsGroup), 3, 0);
    salesLayout->addWidget(m_trackingNumberEdit, 3, 1);
    salesLayout->addWidget(new QLabel(QStringLiteral("送货日期"), m_salesDetailsGroup), 3, 2);
    salesLayout->addWidget(m_deliveryDateEdit, 3, 3);
    salesLayout->setColumnStretch(1, 1);
    salesLayout->setColumnStretch(3, 1);
    layout->addWidget(m_salesDetailsGroup);

    m_lines = new StockLineTable(m_database, StockLineTable::Mode::Outbound, panel);
    m_submitButton = new QPushButton(QStringLiteral("确认并出库"), m_lines);
    m_submitButton->setProperty("primary", true);
    m_submitButton->setFixedSize(110, 34);
    m_lines->addToolbarAction(m_submitButton);
    layout->addWidget(m_lines);
    pageLayout->addWidget(panel);

    auto *recentPanel = new QFrame(pageBody);
    recentPanel->setObjectName(QStringLiteral("panel"));
    auto *recentLayout = new QVBoxLayout(recentPanel);
    auto *recentToolbar = new QHBoxLayout;
    recentToolbar->addWidget(new QLabel(
        QStringLiteral("近期销售出库单（可先出库，发货时再补充销售资料）"), recentPanel));
    recentToolbar->addStretch();
    auto *editRecentButton = new QPushButton(QStringLiteral("修改单据"), recentPanel);
    editRecentButton->setProperty("primary", true);
    auto *shipmentButton = new QPushButton(QStringLiteral("发货"), recentPanel);
    shipmentButton->setProperty("primary", true);
    auto *fullScreenRecentButton = new QPushButton(QStringLiteral("全屏显示"), recentPanel);
    recentToolbar->addWidget(shipmentButton);
    recentToolbar->addWidget(editRecentButton);
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
    // 保留近期出库单的可见高度。窗口高度不足时，让外层滚动区域产生
    // 纵向滚动条，而不是把该表格压缩到完全不可见。
    m_recentTable->setMinimumHeight(180);
    recentLayout->addWidget(m_recentTable);
    pageLayout->addWidget(recentPanel, 1);
    pageScroll->setWidget(pageBody);
    root->addWidget(pageScroll);

    connect(editRecentButton, &QPushButton::clicked, this, [this] {
        TableExcelExport::editSelectedBusinessDocument(
            m_recentTable, this, [this] { refreshRecentDocuments(); });
    });
    connect(shipmentButton, &QPushButton::clicked, this, &StockOutPage::editShipmentDetails);
    connect(fullScreenRecentButton, &QPushButton::clicked, this, [this] {
        TableExcelExport::fullScreenTable(
            m_recentTable, QStringLiteral("全部普通出库单"), this, [this] { refreshRecentDocuments(); });
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
    m_lines->setMaterialCategoryFilter(
        salesOutbound ? QStringLiteral("FINISHED") : QString());
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
        "SELECT d.id,d.document_no,'销售出库',"
        "COALESCE(NULLIF(s.delivery_date,''),d.document_date),"
        "COALESCE(s.customer_company,''),COALESCE(s.destination,''),COUNT(i.id) "
        "FROM business_documents d LEFT JOIN business_document_items i ON i.document_id=d.id "
        "LEFT JOIN sales_outbound_details s ON s.document_id=d.id "
        "WHERE d.stock_direction='OUT' AND d.document_type='XSCK' "
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

void StockOutPage::editShipmentDetails()
{
    const int row = m_recentTable->currentRow();
    const QTableWidgetItem *selected = row >= 0 ? m_recentTable->item(row, 0) : nullptr;
    const qlonglong documentId = selected ? selected->data(Qt::UserRole).toLongLong() : 0;
    if (documentId <= 0) {
        QMessageBox::information(this, QStringLiteral("请选择出库单"),
                                 QStringLiteral("请先在近期销售出库单中选择一条记录。"));
        return;
    }

    QSqlQuery current(m_database);
    current.prepare(QStringLiteral(
        "SELECT d.document_no,d.document_date,COALESCE(s.customer_company,''),"
        "COALESCE(s.destination,''),COALESCE(s.contact_name,''),COALESCE(s.contact_phone,''),"
        "COALESCE(s.sales_order_no,''),COALESCE(s.logistics_company,''),"
        "COALESCE(s.tracking_no,''),COALESCE(s.delivery_date,'') "
        "FROM business_documents d LEFT JOIN sales_outbound_details s ON s.document_id=d.id "
        "WHERE d.id=? AND d.document_type='XSCK'"));
    current.addBindValue(documentId);
    if (!current.exec() || !current.next()) {
        QMessageBox::warning(this, QStringLiteral("读取发货资料失败"),
                             current.lastError().text().isEmpty()
                                 ? QStringLiteral("所选记录不是销售出库单。")
                                 : current.lastError().text());
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("发货｜%1").arg(current.value(0).toString()));
    dialog.setMinimumWidth(720);
    auto *dialogLayout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(
        QStringLiteral("补充或修改销售出库信息；所有栏目均可留空，保存后同步到发货查询和表单。"),
        &dialog);
    hint->setObjectName(QStringLiteral("mutedText"));
    dialogLayout->addWidget(hint);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto *customerCompany = new QLineEdit(current.value(2).toString(), &dialog);
    auto *destination = new QLineEdit(current.value(3).toString(), &dialog);
    auto *contact = new QLineEdit(current.value(4).toString(), &dialog);
    auto *phone = new QLineEdit(current.value(5).toString(), &dialog);
    auto *orderNumber = new QLineEdit(current.value(6).toString(), &dialog);
    auto *logistics = new QLineEdit(current.value(7).toString(), &dialog);
    auto *tracking = new QLineEdit(current.value(8).toString(), &dialog);
    auto *deliveryDate = new QDateEdit(&dialog);
    const QDate emptyDate(1900, 1, 1);
    deliveryDate->setMinimumDate(emptyDate);
    deliveryDate->setMaximumDate(QDate(2999, 12, 31));
    deliveryDate->setCalendarPopup(true);
    deliveryDate->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    deliveryDate->setSpecialValueText(QStringLiteral("未填写"));
    const QDate savedDeliveryDate = QDate::fromString(current.value(9).toString(), Qt::ISODate);
    deliveryDate->setDate(savedDeliveryDate.isValid() ? savedDeliveryDate : emptyDate);
    form->addRow(QStringLiteral("客户单位"), customerCompany);
    form->addRow(QStringLiteral("收货地址"), destination);
    form->addRow(QStringLiteral("收货人"), contact);
    form->addRow(QStringLiteral("联系电话"), phone);
    form->addRow(QStringLiteral("客户合同号/订单号"), orderNumber);
    form->addRow(QStringLiteral("物流/快递公司"), logistics);
    form->addRow(QStringLiteral("运单号"), tracking);
    form->addRow(QStringLiteral("送货日期"), deliveryDate);
    dialogLayout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存发货资料"));
    buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    dialogLayout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    if (!m_database.transaction()) {
        QMessageBox::warning(this, QStringLiteral("保存发货资料失败"), m_database.lastError().text());
        return;
    }
    QSqlQuery save(m_database);
    save.prepare(QStringLiteral(
        "INSERT INTO sales_outbound_details(document_id,customer_company,destination,contact_name,"
        "contact_phone,sales_order_no,logistics_company,tracking_no,delivery_date) "
        "VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(document_id) DO UPDATE SET "
        "customer_company=excluded.customer_company,destination=excluded.destination,"
        "contact_name=excluded.contact_name,contact_phone=excluded.contact_phone,"
        "sales_order_no=excluded.sales_order_no,logistics_company=excluded.logistics_company,"
        "tracking_no=excluded.tracking_no,delivery_date=excluded.delivery_date"));
    save.addBindValue(documentId);
    save.addBindValue(customerCompany->text().trimmed());
    save.addBindValue(destination->text().trimmed());
    save.addBindValue(contact->text().trimmed());
    save.addBindValue(phone->text().trimmed());
    save.addBindValue(orderNumber->text().trimmed());
    save.addBindValue(logistics->text().trimmed());
    save.addBindValue(tracking->text().trimmed());
    save.addBindValue(deliveryDate->date() == emptyDate
                          ? QStringLiteral("") : deliveryDate->date().toString(Qt::ISODate));
    bool ok = save.exec();
    QString error = save.lastError().text();
    if (ok) {
        QSqlQuery audit(m_database);
        audit.prepare(QStringLiteral(
            "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
            "VALUES(?,'SHIPMENT_DETAILS_UPDATE','business_document',?,?)"));
        audit.addBindValue(m_session.userId);
        audit.addBindValue(documentId);
        audit.addBindValue(current.value(0).toString());
        ok = audit.exec();
        if (!ok) error = audit.lastError().text();
    }
    if (!ok || !m_database.commit()) {
        if (error.isEmpty()) error = m_database.lastError().text();
        m_database.rollback();
        QMessageBox::warning(this, QStringLiteral("保存发货资料失败"), error);
        return;
    }

    QStringList formErrors;
    QStringList savedPaths;
    OfficeTemplateService::synchronizeDocumentForms(
        m_database, m_session.userId, documentId, &formErrors, false, &savedPaths);
    refreshRecentDocuments();
    emit stockChanged();
    if (formErrors.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("发货资料已保存"),
                                 QStringLiteral("销售出库信息、发货查询和对应表单已同步更新。"));
    } else {
        QMessageBox::warning(
            this, QStringLiteral("发货资料已保存，部分表单未同步"),
            QStringLiteral("销售出库信息已经保存；以下表单可在附件管理中重新生成：\n\n%1")
                .arg(formErrors.join(QStringLiteral("\n"))));
    }
}

void StockOutPage::submit()
{
    const bool salesOutbound = m_typeCombo->currentData().toString() == QStringLiteral("XSCK");
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
    outboundForm.fields.insert(QStringLiteral("notes"), m_notesEdit->toPlainText().trimmed());
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
        // 最后确认的在线表单是发货资料的最终值，回填页面并统一两张表单。
        m_customerCompanyEdit->setText(deliveryForm.fields.value(QStringLiteral("customerCompany")));
        m_destinationEdit->setText(deliveryForm.fields.value(QStringLiteral("destination")));
        m_customerContactEdit->setText(deliveryForm.fields.value(QStringLiteral("customerContact")));
        m_customerPhoneEdit->setText(deliveryForm.fields.value(QStringLiteral("customerPhone")));
        m_salesOrderEdit->setText(deliveryForm.fields.value(QStringLiteral("salesOrderNumber")));
        m_logisticsCompanyEdit->setText(deliveryForm.fields.value(QStringLiteral("logisticsCompany")));
        m_trackingNumberEdit->setText(deliveryForm.fields.value(QStringLiteral("trackingNumber")));
        outboundForm.fields = deliveryForm.fields;
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
    QStringList savedFiles;
    QString formError;
    if (!OfficeTemplateService::attachToDocument(outboundForm, m_database, m_session.userId,
                                                  posted.documentId, &formError)) {
        formErrors.append(QStringLiteral("出库单：%1").arg(formError));
    } else {
        savedFiles.append(QDir::toNativeSeparators(
            OfficeTemplateService::archiveFilePath(outboundForm)));
    }
    if (salesOutbound) {
        deliveryForm.documentNumber = posted.documentNumber;
        formError.clear();
        if (!OfficeTemplateService::attachToDocument(deliveryForm, m_database, m_session.userId,
                                                      posted.documentId, &formError)) {
            formErrors.append(QStringLiteral("送货确认单：%1").arg(formError));
        } else {
            savedFiles.append(QDir::toNativeSeparators(
                OfficeTemplateService::archiveFilePath(deliveryForm)));
        }
    }
    if (formErrors.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("出库完成"),
            QStringLiteral("出库单 %1 已生效，模板表单已保存。\n\n文件：\n%2")
                .arg(posted.documentNumber, savedFiles.join(QStringLiteral("\n"))));
    } else {
        QMessageBox::warning(
            this, QStringLiteral("出库已完成，但模板处理未全部完成"),
            QStringLiteral("出库单 %1 已生效，但以下保存步骤未完成：\n\n%2")
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
