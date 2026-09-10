#include "ui/pages/ShipmentQueryPage.h"

#include "ui/widgets/ComboBoxSearch.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QStringList>
#include <QTableView>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <QVariant>
#include <QVBoxLayout>

#include <utility>

namespace {
void configureTable(QTableView *table)
{
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSortingEnabled(false);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
}

QString materialProjectExpression(const QString &materialAlias)
{
    return QStringLiteral(
        "COALESCE((SELECT p.code || CASE WHEN p.name='' THEN '' ELSE ' - ' || p.name END "
        "FROM material_projects p "
        "WHERE length(%1.code)=length(p.code)+5 "
        "AND substr(%1.code,2,length(p.code))=p.code COLLATE NOCASE "
        "ORDER BY length(p.code) DESC LIMIT 1),'')")
        .arg(materialAlias);
}
}

ShipmentQueryPage::ShipmentQueryPage(QSqlDatabase database, QWidget *parent)
    : QWidget(parent), m_database(std::move(database))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *filterPanel = new QFrame(this);
    filterPanel->setObjectName(QStringLiteral("panel"));
    auto *filterLayout = new QVBoxLayout(filterPanel);
    filterLayout->setContentsMargins(16, 14, 16, 14);
    filterLayout->setSpacing(10);

    auto *filterTitle = new QLabel(QStringLiteral("发货查询条件"), filterPanel);
    filterTitle->setObjectName(QStringLiteral("sectionTitle"));
    filterLayout->addWidget(filterTitle);

    auto *filters = new QHBoxLayout;
    filters->setSpacing(10);
    m_orderEdit = new QLineEdit(filterPanel);
    m_orderEdit->setPlaceholderText(QStringLiteral("输入订单号或发货单号"));
    m_orderEdit->setClearButtonEnabled(true);
    m_orderEdit->setMinimumWidth(220);

    m_customerCombo = new QComboBox(filterPanel);
    m_customerCombo->setMinimumWidth(210);
    ComboBoxSearch::enableContainsSearch(m_customerCombo, QStringLiteral("选择或输入客户单位"));

    m_projectCombo = new QComboBox(filterPanel);
    m_projectCombo->setMinimumWidth(210);
    ComboBoxSearch::enableContainsSearch(m_projectCombo, QStringLiteral("选择项目"));

    auto *searchButton = new QPushButton(QStringLiteral("查询"), filterPanel);
    searchButton->setProperty("primary", true);
    auto *resetButton = new QPushButton(QStringLiteral("重置"), filterPanel);

    filters->addWidget(new QLabel(QStringLiteral("订单号"), filterPanel));
    filters->addWidget(m_orderEdit, 1);
    filters->addWidget(new QLabel(QStringLiteral("客户单位"), filterPanel));
    filters->addWidget(m_customerCombo);
    filters->addWidget(new QLabel(QStringLiteral("项目"), filterPanel));
    filters->addWidget(m_projectCombo);
    filters->addWidget(searchButton);
    filters->addWidget(resetButton);
    filterLayout->addLayout(filters);
    root->addWidget(filterPanel);

    auto *shipmentPanel = new QFrame(this);
    shipmentPanel->setObjectName(QStringLiteral("panel"));
    auto *shipmentLayout = new QVBoxLayout(shipmentPanel);
    shipmentLayout->setContentsMargins(12, 12, 12, 12);
    shipmentLayout->setSpacing(8);

    auto *shipmentHeader = new QHBoxLayout;
    auto *shipmentTitle = new QLabel(QStringLiteral("发货清单"), shipmentPanel);
    shipmentTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_summaryLabel = new QLabel(shipmentPanel);
    m_summaryLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_openTemplateButton = new QPushButton(QStringLiteral("打开送货确认单"), shipmentPanel);
    m_openTemplateButton->setEnabled(false);
    shipmentHeader->addWidget(shipmentTitle);
    shipmentHeader->addStretch();
    shipmentHeader->addWidget(m_summaryLabel);
    shipmentHeader->addWidget(m_openTemplateButton);
    shipmentLayout->addLayout(shipmentHeader);

    m_shipmentTable = new QTableView(shipmentPanel);
    m_shipmentTable->setProperty("businessDocumentTable", true);
    m_shipmentTable->setProperty("businessDocumentIdColumn", 0);
    m_shipmentModel = new QSqlQueryModel(this);
    m_shipmentTable->setModel(m_shipmentModel);
    configureTable(m_shipmentTable);
    shipmentLayout->addWidget(m_shipmentTable);
    root->addWidget(shipmentPanel, 1);

    auto *detailPanel = new QFrame(this);
    detailPanel->setObjectName(QStringLiteral("panel"));
    auto *detailLayout = new QVBoxLayout(detailPanel);
    detailLayout->setContentsMargins(12, 12, 12, 12);
    detailLayout->setSpacing(8);
    m_detailTitle = new QLabel(QStringLiteral("产品基础信息（请选择一条发货记录）"), detailPanel);
    m_detailTitle->setObjectName(QStringLiteral("sectionTitle"));
    detailLayout->addWidget(m_detailTitle);

    m_detailTable = new QTableView(detailPanel);
    m_detailModel = new QSqlQueryModel(this);
    m_detailTable->setModel(m_detailModel);
    configureTable(m_detailTable);
    detailLayout->addWidget(m_detailTable);
    root->addWidget(detailPanel, 1);

    connect(searchButton, &QPushButton::clicked, this, &ShipmentQueryPage::loadShipments);
    connect(resetButton, &QPushButton::clicked, this, &ShipmentQueryPage::resetFilters);
    connect(m_orderEdit, &QLineEdit::returnPressed, this, &ShipmentQueryPage::loadShipments);
    connect(m_openTemplateButton, &QPushButton::clicked,
            this, &ShipmentQueryPage::openSelectedDeliveryForm);
    connect(m_shipmentTable->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this] { loadSelectedShipmentDetails(); });

    refresh();
}

QString ShipmentQueryPage::customerFilter() const
{
    if (m_customerCombo->currentIndex() == 0) return {};
    return m_customerCombo->currentText().trimmed();
}

QString ShipmentQueryPage::projectFilter() const
{
    if (m_projectCombo->currentIndex() == 0) return {};
    const QString selectedCode = m_projectCombo->currentData().toString().trimmed();
    return selectedCode.isEmpty() ? m_projectCombo->currentText().trimmed() : selectedCode;
}

void ShipmentQueryPage::loadFilterOptions()
{
    const QString previousCustomer = customerFilter();
    const QString previousProject = projectFilter();

    m_customerCombo->blockSignals(true);
    m_customerCombo->clear();
    m_customerCombo->addItem(QStringLiteral("全部客户"), QString());
    QSqlQuery customers(m_database);
    customers.exec(QStringLiteral(
        "SELECT DISTINCT customer_company FROM sales_outbound_details "
        "WHERE trim(customer_company)<>'' ORDER BY customer_company COLLATE NOCASE"));
    while (customers.next()) {
        const QString customer = customers.value(0).toString();
        m_customerCombo->addItem(customer, customer);
    }
    const int customerIndex = m_customerCombo->findData(previousCustomer);
    if (customerIndex < 0 && !previousCustomer.isEmpty()) {
        m_customerCombo->setEditText(previousCustomer);
    } else {
        m_customerCombo->setCurrentIndex(customerIndex >= 0 ? customerIndex : 0);
    }
    m_customerCombo->blockSignals(false);

    m_projectCombo->blockSignals(true);
    m_projectCombo->clear();
    m_projectCombo->addItem(QStringLiteral("全部项目"), QString());
    QSqlQuery projects(m_database);
    projects.exec(QStringLiteral(
        "SELECT code,name,is_active FROM material_projects "
        "ORDER BY is_active DESC, code COLLATE NOCASE"));
    while (projects.next()) {
        QString label = projects.value(0).toString();
        const QString name = projects.value(1).toString().trimmed();
        if (!name.isEmpty()) label += QStringLiteral(" - ") + name;
        if (!projects.value(2).toBool()) label += QStringLiteral("（已停用）");
        m_projectCombo->addItem(label, projects.value(0).toString());
    }
    const int projectIndex = m_projectCombo->findData(previousProject);
    m_projectCombo->setCurrentIndex(projectIndex >= 0 ? projectIndex : 0);
    m_projectCombo->blockSignals(false);
}

void ShipmentQueryPage::refresh()
{
    loadFilterOptions();
    loadShipments();
}

void ShipmentQueryPage::resetFilters()
{
    m_orderEdit->clear();
    m_customerCombo->setCurrentIndex(0);
    m_projectCombo->setCurrentIndex(0);
    loadShipments();
}

void ShipmentQueryPage::loadShipments()
{
    const QString orderKeyword = QStringLiteral("%%1%").arg(m_orderEdit->text().trimmed());
    const QString customer = customerFilter();
    const QString customerKeyword = QStringLiteral("%%1%").arg(customer);
    const QString projectCode = projectFilter();

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT d.id,d.document_no,"
        "COALESCE(NULLIF(s.delivery_date,''),d.document_date),s.sales_order_no,s.customer_company,"
        "s.destination,s.contact_name,s.contact_phone,s.logistics_company,s.tracking_no,"
        "d.handler_name,COUNT(i.id),COALESCE(SUM(i.quantity),0),"
        "COALESCE(SUM(i.reversed_quantity),0),"
        "COALESCE(SUM(i.quantity-i.reversed_quantity),0),"
        "CASE d.status WHEN 'POSTED' THEN '已发货' "
        "WHEN 'PARTIALLY_REVERSED' THEN '部分退回' "
        "WHEN 'REVERSED' THEN '已全部退回' ELSE d.status END,d.notes "
        "FROM business_documents d "
        "JOIN sales_outbound_details s ON s.document_id=d.id "
        "LEFT JOIN business_document_items i ON i.document_id=d.id "
        "WHERE d.document_type='XSCK' "
        "AND (s.sales_order_no LIKE ? OR d.document_no LIKE ?) "
        "AND (?='' OR s.customer_company LIKE ?) "
        "AND (?='' OR EXISTS(SELECT 1 FROM business_document_items pi "
        "JOIN materials pm ON pm.id=pi.material_id WHERE pi.document_id=d.id "
        "AND length(pm.code)=length(?)+5 "
        "AND substr(pm.code,2,length(?))=? COLLATE NOCASE)) "
        "GROUP BY d.id,d.document_no,COALESCE(NULLIF(s.delivery_date,''),d.document_date),"
        "s.sales_order_no,s.customer_company,"
        "s.destination,s.contact_name,s.contact_phone,s.logistics_company,s.tracking_no,"
        "d.handler_name,d.status,d.notes "
        "ORDER BY COALESCE(NULLIF(s.delivery_date,''),d.document_date) DESC,d.id DESC"));
    query.addBindValue(orderKeyword);
    query.addBindValue(orderKeyword);
    query.addBindValue(customer);
    query.addBindValue(customerKeyword);
    query.addBindValue(projectCode);
    query.addBindValue(projectCode);
    query.addBindValue(projectCode);
    query.addBindValue(projectCode);

    if (!query.exec()) {
        m_shipmentModel->clear();
        m_detailModel->clear();
        m_summaryLabel->setText(QStringLiteral("查询失败：%1").arg(query.lastError().text()));
        m_detailTitle->setText(QStringLiteral("产品基础信息"));
        return;
    }
    m_shipmentModel->setQuery(std::move(query));
    while (m_shipmentModel->canFetchMore()) m_shipmentModel->fetchMore();

    const QStringList headers = {
        QStringLiteral("ID"), QStringLiteral("发货单号"), QStringLiteral("发货日期"),
        QStringLiteral("订单号"), QStringLiteral("客户单位"), QStringLiteral("收货地址"),
        QStringLiteral("联系人"), QStringLiteral("联系电话"), QStringLiteral("物流公司"),
        QStringLiteral("物流单号"), QStringLiteral("经办人"), QStringLiteral("明细数"),
        QStringLiteral("发货数量"), QStringLiteral("已退数量"), QStringLiteral("实发数量"),
        QStringLiteral("状态"), QStringLiteral("备注")};
    for (int column = 0; column < headers.size(); ++column) {
        m_shipmentModel->setHeaderData(column, Qt::Horizontal, headers.at(column));
    }
    m_shipmentTable->hideColumn(0);
    m_shipmentTable->resizeColumnsToContents();
    m_shipmentTable->setColumnWidth(4, qMax(m_shipmentTable->columnWidth(4), 160));
    m_shipmentTable->setColumnWidth(5, qMax(m_shipmentTable->columnWidth(5), 180));

    double totalNetQuantity = 0;
    for (int row = 0; row < m_shipmentModel->rowCount(); ++row) {
        totalNetQuantity += m_shipmentModel->index(row, 14).data().toDouble();
    }
    m_summaryLabel->setText(QStringLiteral("共 %1 张发货单，实发数量 %2")
                                .arg(m_shipmentModel->rowCount())
                                .arg(totalNetQuantity, 0, 'f', 2));

    if (m_shipmentModel->rowCount() > 0) {
        m_shipmentTable->selectRow(0);
        m_shipmentTable->setCurrentIndex(m_shipmentModel->index(0, 1));
        loadSelectedShipmentDetails();
    } else {
        m_openTemplateButton->setEnabled(false);
        m_detailModel->clear();
        m_detailTitle->setText(QStringLiteral("产品基础信息（没有符合条件的发货记录）"));
    }
}

void ShipmentQueryPage::loadSelectedShipmentDetails()
{
    const QModelIndex current = m_shipmentTable->currentIndex();
    if (!current.isValid()) {
        m_openTemplateButton->setEnabled(false);
        m_detailModel->clear();
        m_detailTitle->setText(QStringLiteral("产品基础信息（请选择一条发货记录）"));
        return;
    }
    const qlonglong documentId = m_shipmentModel->index(current.row(), 0).data().toLongLong();
    m_openTemplateButton->setEnabled(documentId > 0);
    const QString documentNo = m_shipmentModel->index(current.row(), 1).data().toString();
    const QString orderNo = m_shipmentModel->index(current.row(), 3).data().toString();
    const QString customer = m_shipmentModel->index(current.row(), 4).data().toString();
    m_detailTitle->setText(QStringLiteral("产品基础信息｜发货单：%1　订单：%2　客户：%3")
                               .arg(documentNo, orderNo.isEmpty() ? QStringLiteral("-") : orderNo,
                                    customer));
    loadShipmentDetails(documentId);
}

void ShipmentQueryPage::openSelectedDeliveryForm()
{
    const QModelIndex current = m_shipmentTable->currentIndex();
    if (!current.isValid()) return;
    const qlonglong documentId = m_shipmentModel->index(current.row(), 0).data().toLongLong();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT original_file_name,file_data FROM attachments "
        "WHERE business_type='business_document' AND business_id=? AND is_deleted=0 "
        "AND original_file_name LIKE '送货确认单\\_%' ESCAPE '\\' "
        "ORDER BY id DESC LIMIT 1"));
    query.addBindValue(documentId);
    if (!query.exec()) {
        QMessageBox::warning(this, QStringLiteral("读取模板失败"), query.lastError().text());
        return;
    }
    if (!query.next()) {
        QMessageBox::information(
            this, QStringLiteral("尚无送货确认单"),
            QStringLiteral("该发货记录还没有模板附件。新提交的销售出库会自动生成并保存。"));
        return;
    }
    const QString originalName = QFileInfo(query.value(0).toString()).fileName();
    const QByteArray data = query.value(1).toByteArray();
    const QString directory = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                  .filePath(QStringLiteral("IceBeautyWms/shipment-forms"));
    QDir().mkpath(directory);
    const QString path = QDir(directory).filePath(
        QStringLiteral("%1_%2").arg(QUuid::createUuid().toString(QUuid::WithoutBraces),
                                    originalName));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        QMessageBox::warning(this, QStringLiteral("打开模板失败"),
                             QStringLiteral("无法写入临时表单文件：%1").arg(file.errorString()));
        return;
    }
    file.close();
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::warning(this, QStringLiteral("打开模板失败"),
                             QStringLiteral("Windows 无法打开已保存的送货确认单。"));
    }
}

void ShipmentQueryPage::loadShipmentDetails(qlonglong documentId)
{
    const QString projectCode = projectFilter();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT i.line_number,m.code,m.name,m.specification,%1,COALESCE(c.name,''),"
        "m.brand,m.unit,i.quantity,i.reversed_quantity,(i.quantity-i.reversed_quantity),"
        "i.batch_no,COALESCE(w.name,''),COALESCE(l.code,''),m.processing_method,"
        "m.unit_usage,m.notes,i.notes "
        "FROM business_document_items i "
        "JOIN materials m ON m.id=i.material_id "
        "LEFT JOIN material_categories c ON c.id=m.category_id "
        "LEFT JOIN warehouses w ON w.id=i.warehouse_id "
        "LEFT JOIN locations l ON l.id=i.location_id "
        "WHERE i.document_id=? AND (?='' OR (length(m.code)=length(?)+5 "
        "AND substr(m.code,2,length(?))=? COLLATE NOCASE)) "
        "ORDER BY i.line_number")
                      .arg(materialProjectExpression(QStringLiteral("m"))));
    query.addBindValue(documentId);
    query.addBindValue(projectCode);
    query.addBindValue(projectCode);
    query.addBindValue(projectCode);
    query.addBindValue(projectCode);
    if (!query.exec()) {
        m_detailModel->clear();
        m_detailTitle->setText(QStringLiteral("产品基础信息读取失败：%1")
                                   .arg(query.lastError().text()));
        return;
    }
    m_detailModel->setQuery(std::move(query));

    const QStringList headers = {
        QStringLiteral("行号"), QStringLiteral("物料编码"), QStringLiteral("产品名称"),
        QStringLiteral("规格型号"), QStringLiteral("项目"), QStringLiteral("物料类别"),
        QStringLiteral("品牌"), QStringLiteral("单位"), QStringLiteral("发货数量"),
        QStringLiteral("已退数量"), QStringLiteral("实发数量"), QStringLiteral("批次"),
        QStringLiteral("发货仓库"), QStringLiteral("发货库位"), QStringLiteral("加工方式"),
        QStringLiteral("单台用量"), QStringLiteral("产品备注"), QStringLiteral("发货备注")};
    for (int column = 0; column < headers.size(); ++column) {
        m_detailModel->setHeaderData(column, Qt::Horizontal, headers.at(column));
    }
    m_detailTable->resizeColumnsToContents();
    m_detailTable->setColumnWidth(2, qMax(m_detailTable->columnWidth(2), 150));
    m_detailTable->setColumnWidth(3, qMax(m_detailTable->columnWidth(3), 150));
}
