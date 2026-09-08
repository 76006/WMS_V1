#include "ui/pages/StockOutPage.h"

#include "services/InventoryService.h"
#include "ui/widgets/StockLineTable.h"

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
    m_typeCombo->addItem(QStringLiteral("样品领用"), QStringLiteral("YPLY"));
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
    salesLayout->addWidget(new QLabel(QStringLiteral("客户公司名称 *"), m_salesDetailsGroup), 0, 0);
    salesLayout->addWidget(m_customerCompanyEdit, 0, 1);
    salesLayout->addWidget(new QLabel(QStringLiteral("销售目的地/收货地址 *"), m_salesDetailsGroup), 0, 2);
    salesLayout->addWidget(m_destinationEdit, 0, 3);
    salesLayout->addWidget(new QLabel(QStringLiteral("客户联系人"), m_salesDetailsGroup), 1, 0);
    salesLayout->addWidget(m_customerContactEdit, 1, 1);
    salesLayout->addWidget(new QLabel(QStringLiteral("联系电话"), m_salesDetailsGroup), 1, 2);
    salesLayout->addWidget(m_customerPhoneEdit, 1, 3);
    salesLayout->addWidget(new QLabel(QStringLiteral("销售订单号"), m_salesDetailsGroup), 2, 0);
    salesLayout->addWidget(m_salesOrderEdit, 2, 1);
    salesLayout->addWidget(new QLabel(QStringLiteral("物流/快递公司"), m_salesDetailsGroup), 2, 2);
    salesLayout->addWidget(m_logisticsCompanyEdit, 2, 3);
    salesLayout->addWidget(new QLabel(QStringLiteral("运单号"), m_salesDetailsGroup), 3, 0);
    salesLayout->addWidget(m_trackingNumberEdit, 3, 1);
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
    recentLayout->addWidget(new QLabel(
        QStringLiteral("近期普通出库单（双击销售出库可查看完整销售资料）"), recentPanel));
    m_recentTable = new QTableWidget(0, 6, recentPanel);
    m_recentTable->setHorizontalHeaderLabels({QStringLiteral("单据号"), QStringLiteral("类型"),
                                              QStringLiteral("日期"), QStringLiteral("客户公司"),
                                              QStringLiteral("销售目的地"), QStringLiteral("明细数")});
    m_recentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recentTable->horizontalHeader()->setStretchLastSection(true);
    recentLayout->addWidget(m_recentTable);
    root->addWidget(recentPanel, 1);

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
    const bool salesOutbound = m_typeCombo->currentData().toString() == QStringLiteral("XSCK");
    m_salesDetailsGroup->setVisible(salesOutbound);
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
        "SELECT d.id,d.document_no,d.document_type,d.document_date,"
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
        "s.sales_order_no,s.logistics_company,s.tracking_no "
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
        QStringLiteral("出库单号：%1\n客户公司：%2\n销售目的地/收货地址：%3\n"
                       "客户联系人：%4\n联系电话：%5\n销售订单号：%6\n"
                       "物流/快递公司：%7\n运单号：%8")
            .arg(valueOrEmpty(0), valueOrEmpty(1), valueOrEmpty(2), valueOrEmpty(3),
                 valueOrEmpty(4), valueOrEmpty(5), valueOrEmpty(6), valueOrEmpty(7)));
}

void StockOutPage::submit()
{
    const bool salesOutbound = m_typeCombo->currentData().toString() == QStringLiteral("XSCK");
    if (salesOutbound
        && (m_customerCompanyEdit->text().trimmed().isEmpty()
            || m_destinationEdit->text().trimmed().isEmpty())) {
        QMessageBox::warning(this, QStringLiteral("销售资料不完整"),
                             QStringLiteral("销售出库必须填写客户公司名称和销售目的地/收货地址。"));
        return;
    }
    QString error;
    const QList<StockMovementRequest> lines = m_lines->lines(&error);
    if (lines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("出库明细有误"), error);
        return;
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
    QMessageBox::information(this, QStringLiteral("出库完成"),
                             QStringLiteral("出库单 %1 已生效。").arg(posted.documentNumber));
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
    }
    m_lines->clearLines();
    emit stockChanged();
    refreshReferenceData();
}
