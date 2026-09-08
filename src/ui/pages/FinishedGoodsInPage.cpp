#include "ui/pages/FinishedGoodsInPage.h"

#include "services/InventoryService.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace {
constexpr int ProductMaterialRole = Qt::UserRole + 1;
constexpr int ProductCodeRole = Qt::UserRole + 2;
constexpr int ProductNameRole = Qt::UserRole + 3;
constexpr int ProductModelRole = Qt::UserRole + 4;
constexpr int PlannedRole = Qt::UserRole + 5;
constexpr int DefaultWarehouseRole = Qt::UserRole + 6;
constexpr int DefaultLocationRole = Qt::UserRole + 7;
constexpr int RequireSerialRole = Qt::UserRole + 8;
}

FinishedGoodsInPage::FinishedGoodsInPage(QSqlDatabase database,
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
    panel->setMaximumWidth(980);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(22, 20, 22, 22);
    auto *heading = new QLabel(QStringLiteral("成品入库"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    panelLayout->addWidget(heading);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_runCombo = new QComboBox(panel);
    m_runCombo->setEditable(true);
    m_runCombo->setInsertPolicy(QComboBox::NoInsert);
    m_productLabel = new QLabel(panel);
    m_productLabel->setObjectName(QStringLiteral("mutedText"));
    m_progressLabel = new QLabel(panel);
    m_progressLabel->setObjectName(QStringLiteral("mutedText"));
    m_dateEdit = new QDateEdit(QDate::currentDate(), panel);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_quantitySpin = new QDoubleSpinBox(panel);
    m_quantitySpin->setDecimals(6);
    m_quantitySpin->setRange(0.000001, 999999999999.0);
    m_quantitySpin->setValue(1.0);
    m_warehouseCombo = new QComboBox(panel);
    m_locationCombo = new QComboBox(panel);
    m_handlerEdit = new QLineEdit(m_session.displayName, panel);
    m_serialEdit = new QTextEdit(panel);
    m_serialEdit->setMaximumHeight(115);
    m_serialEdit->setPlaceholderText(QStringLiteral("每行一个成品SN"));
    m_generateButton = new QPushButton(QStringLiteral("按本次数量批量生成SN"), panel);
    auto *serialBlock = new QWidget(panel);
    auto *serialLayout = new QVBoxLayout(serialBlock);
    serialLayout->setContentsMargins(0, 0, 0, 0);
    serialLayout->addWidget(m_serialEdit);
    serialLayout->addWidget(m_generateButton, 0, Qt::AlignLeft);
    m_notesEdit = new QTextEdit(panel);
    m_notesEdit->setMaximumHeight(70);
    form->addRow(QStringLiteral("生产批次 *"), m_runCombo);
    form->addRow(QStringLiteral("成品资料"), m_productLabel);
    form->addRow(QStringLiteral("生产进度"), m_progressLabel);
    form->addRow(QStringLiteral("生产日期 *"), m_dateEdit);
    form->addRow(QStringLiteral("本次入库数量 *"), m_quantitySpin);
    form->addRow(QStringLiteral("仓库 *"), m_warehouseCombo);
    form->addRow(QStringLiteral("库位 *"), m_locationCombo);
    form->addRow(QStringLiteral("操作人员"), m_handlerEdit);
    form->addRow(QStringLiteral("产品SN"), serialBlock);
    form->addRow(QStringLiteral("备注"), m_notesEdit);
    panelLayout->addLayout(form);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    m_submitButton = new QPushButton(QStringLiteral("确认成品入库"), panel);
    m_submitButton->setProperty("primary", true);
    actions->addWidget(m_submitButton);
    panelLayout->addLayout(actions);
    root->addWidget(panel, 0, Qt::AlignHCenter | Qt::AlignTop);

    auto *recentPanel = new QFrame(this);
    recentPanel->setObjectName(QStringLiteral("panel"));
    auto *recentLayout = new QVBoxLayout(recentPanel);
    recentLayout->addWidget(new QLabel(QStringLiteral("近期成品入库单"), recentPanel));
    m_recentTable = new QTableWidget(0, 5, recentPanel);
    m_recentTable->setHorizontalHeaderLabels({QStringLiteral("单据号"), QStringLiteral("日期"),
                                              QStringLiteral("生产批次"), QStringLiteral("成品"),
                                              QStringLiteral("数量")});
    m_recentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recentTable->horizontalHeader()->setStretchLastSection(true);
    recentLayout->addWidget(m_recentTable);
    root->addWidget(recentPanel, 1);

    connect(m_runCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &FinishedGoodsInPage::loadRunDetails);
    connect(m_warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &FinishedGoodsInPage::loadLocations);
    connect(m_generateButton, &QPushButton::clicked,
            this, &FinishedGoodsInPage::generateSerialNumbers);
    connect(m_submitButton, &QPushButton::clicked, this, &FinishedGoodsInPage::submit);
    resetSubmissionToken();
    refreshReferenceData();
}

void FinishedGoodsInPage::resetSubmissionToken()
{
    m_submissionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void FinishedGoodsInPage::refreshReferenceData()
{
    const QVariant selectedRun = m_runCombo->currentData();
    m_runCombo->blockSignals(true);
    m_runCombo->clear();
    QSqlQuery runs(m_database);
    runs.exec(QStringLiteral(
        "SELECT p.id,p.batch_no,m.id,m.code,p.product_name,p.product_model,p.planned_quantity,"
        "m.default_warehouse_id,m.default_location_id,m.require_serial,p.status "
        "FROM production_runs p JOIN materials m ON m.id=p.product_material_id ORDER BY p.id DESC"));
    while (runs.next()) {
        const int index = m_runCombo->count();
        m_runCombo->addItem(QStringLiteral("%1 - %2（%3）")
                                .arg(runs.value(1).toString(), runs.value(4).toString(),
                                     runs.value(10).toString() == QStringLiteral("COMPLETED")
                                         ? QStringLiteral("已完成") : QStringLiteral("进行中")),
                            runs.value(0));
        m_runCombo->setItemData(index, runs.value(2), ProductMaterialRole);
        m_runCombo->setItemData(index, runs.value(3), ProductCodeRole);
        m_runCombo->setItemData(index, runs.value(4), ProductNameRole);
        m_runCombo->setItemData(index, runs.value(5), ProductModelRole);
        m_runCombo->setItemData(index, runs.value(6), PlannedRole);
        m_runCombo->setItemData(index, runs.value(7), DefaultWarehouseRole);
        m_runCombo->setItemData(index, runs.value(8), DefaultLocationRole);
        m_runCombo->setItemData(index, runs.value(9), RequireSerialRole);
    }
    const int selectedIndex = m_runCombo->findData(selectedRun);
    if (selectedIndex >= 0) m_runCombo->setCurrentIndex(selectedIndex);
    m_runCombo->blockSignals(false);

    const QVariant selectedWarehouse = m_warehouseCombo->currentData();
    m_warehouseCombo->blockSignals(true);
    m_warehouseCombo->clear();
    QSqlQuery warehouses(m_database);
    warehouses.exec(QStringLiteral(
        "SELECT id,code,name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (warehouses.next()) {
        m_warehouseCombo->addItem(QStringLiteral("%1 - %2")
                                      .arg(warehouses.value(1).toString(),
                                           warehouses.value(2).toString()),
                                  warehouses.value(0));
    }
    const int warehouseIndex = m_warehouseCombo->findData(selectedWarehouse);
    if (warehouseIndex >= 0) m_warehouseCombo->setCurrentIndex(warehouseIndex);
    m_warehouseCombo->blockSignals(false);
    loadRunDetails();
    refreshRecentDocuments();
}

void FinishedGoodsInPage::loadRunDetails()
{
    const int index = m_runCombo->currentIndex();
    if (index < 0) {
        m_productMaterialId = 0;
        m_serialEdit->clear();
        m_productLabel->setText(QStringLiteral("请先在生产领料页创建生产批次。"));
        m_progressLabel->clear();
        m_submitButton->setEnabled(false);
        return;
    }
    const qlonglong productMaterialId = m_runCombo->itemData(index, ProductMaterialRole).toLongLong();
    if (m_productMaterialId != productMaterialId) m_serialEdit->clear();
    m_productMaterialId = productMaterialId;
    m_productCode = m_runCombo->itemData(index, ProductCodeRole).toString();
    m_plannedQuantity = m_runCombo->itemData(index, PlannedRole).toDouble();
    m_requireSerial = m_runCombo->itemData(index, RequireSerialRole).toBool();
    InventoryService service(m_database, m_session.userId);
    QString error;
    m_receivedQuantity = service.productionRunReceivedQuantity(m_runCombo->currentData().toLongLong(), &error);
    if (m_receivedQuantity < 0.0) m_receivedQuantity = 0.0;
    m_productLabel->setText(QStringLiteral("%1    型号：%2")
                                .arg(m_runCombo->itemData(index, ProductNameRole).toString(),
                                     m_runCombo->itemData(index, ProductModelRole).toString()));
    m_progressLabel->setText(QStringLiteral("计划 %1 / 已入库 %2 / 剩余 %3")
                                 .arg(m_plannedQuantity, 0, 'g', 12)
                                 .arg(m_receivedQuantity, 0, 'g', 12)
                                 .arg(qMax(0.0, m_plannedQuantity - m_receivedQuantity), 0, 'g', 12));
    const qlonglong defaultWarehouse = m_runCombo->itemData(index, DefaultWarehouseRole).toLongLong();
    const int warehouseIndex = m_warehouseCombo->findData(defaultWarehouse);
    if (warehouseIndex >= 0) m_warehouseCombo->setCurrentIndex(warehouseIndex);
    m_locationCombo->setProperty("preferredLocation",
                                 m_runCombo->itemData(index, DefaultLocationRole));
    loadLocations();
    m_serialEdit->setEnabled(m_requireSerial);
    m_generateButton->setEnabled(m_requireSerial);
    if (!m_requireSerial) m_serialEdit->clear();
    m_submitButton->setEnabled(m_session.canPostProduction()
                               && m_warehouseCombo->count() > 0
                               && m_locationCombo->count() > 0);
}

void FinishedGoodsInPage::loadLocations()
{
    const QVariant preferred = m_locationCombo->property("preferredLocation");
    const QVariant selected = preferred.isValid() ? preferred : m_locationCombo->currentData();
    m_locationCombo->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id,code,name FROM locations WHERE warehouse_id=? AND is_active=1 ORDER BY code"));
    query.addBindValue(m_warehouseCombo->currentData());
    query.exec();
    while (query.next()) {
        QString text = query.value(1).toString();
        if (!query.value(2).toString().isEmpty()) text += QStringLiteral(" - ") + query.value(2).toString();
        m_locationCombo->addItem(text, query.value(0));
    }
    const int index = m_locationCombo->findData(selected);
    if (index >= 0) m_locationCombo->setCurrentIndex(index);
    m_locationCombo->setProperty("preferredLocation", QVariant());
}

QStringList FinishedGoodsInPage::enteredSerialNumbers() const
{
    QStringList values = m_serialEdit->toPlainText().split(
        QRegularExpression(QStringLiteral("[,;\\r\\n]+")), Qt::SkipEmptyParts);
    for (QString &value : values) value = value.trimmed().toUpper();
    return values;
}

void FinishedGoodsInPage::generateSerialNumbers()
{
    if (!m_requireSerial || m_productMaterialId <= 0) return;
    const double quantity = m_quantitySpin->value();
    if (std::abs(quantity - std::round(quantity)) > 0.0000001) {
        QMessageBox::warning(this, QStringLiteral("数量不正确"),
                             QStringLiteral("SN管理成品的入库数量必须是整数。"));
        return;
    }
    bool ok = false;
    const QString prefix = QInputDialog::getText(this, QStringLiteral("批量生成成品SN"),
                                                 QStringLiteral("SN前缀"), QLineEdit::Normal,
                                                 m_productCode, &ok);
    if (!ok) return;
    InventoryService service(m_database, m_session.userId);
    QString error;
    const QStringList serials = service.previewSerialNumbers(
        m_productMaterialId, prefix, static_cast<int>(std::round(quantity)), &error);
    if (serials.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法生成SN"), error);
        return;
    }
    m_serialEdit->setPlainText(serials.join(QLatin1Char('\n')));
}

void FinishedGoodsInPage::refreshRecentDocuments()
{
    m_recentTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT d.document_no,d.document_date,p.batch_no,p.product_name,i.quantity "
        "FROM business_documents d JOIN production_runs p ON p.id=d.production_run_id "
        "JOIN business_document_items i ON i.document_id=d.id "
        "WHERE d.document_type='CPRK' ORDER BY d.id DESC LIMIT 20"));
    while (query.next()) {
        const int row = m_recentTable->rowCount();
        m_recentTable->insertRow(row);
        for (int column = 0; column < 5; ++column) {
            m_recentTable->setItem(row, column,
                                   new QTableWidgetItem(query.value(column).toString()));
        }
    }
}

void FinishedGoodsInPage::submit()
{
    if (m_runCombo->currentIndex() < 0 || m_locationCombo->currentIndex() < 0) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"),
                             QStringLiteral("请选择生产批次、仓库和库位。"));
        return;
    }
    const QStringList serials = enteredSerialNumbers();
    if (m_requireSerial
        && (std::abs(m_quantitySpin->value() - std::round(m_quantitySpin->value())) > 0.0000001
            || serials.size() != static_cast<int>(std::round(m_quantitySpin->value())))) {
        QMessageBox::warning(this, QStringLiteral("SN数量不一致"),
                             QStringLiteral("SN管理成品的数量必须是整数，且SN数量必须一致。"));
        return;
    }
    const double afterReceipt = m_receivedQuantity + m_quantitySpin->value();
    QString confirmation = QStringLiteral("确认本次成品入库 %1？库存将立即增加并生成流水。")
                               .arg(m_quantitySpin->value(), 0, 'g', 12);
    if (afterReceipt > m_plannedQuantity + 0.0000001) {
        confirmation += QStringLiteral("\n本次提交后将超过计划数量 %1。")
                            .arg(afterReceipt - m_plannedQuantity, 0, 'g', 12);
    }
    if (QMessageBox::question(this, QStringLiteral("确认成品入库"), confirmation)
        != QMessageBox::Yes) {
        return;
    }

    StockMovementRequest line;
    line.materialId = m_productMaterialId;
    line.quantity = m_quantitySpin->value();
    line.warehouseId = m_warehouseCombo->currentData().toLongLong();
    line.locationId = m_locationCombo->currentData().toLongLong();
    line.serialNumbers = serials;
    StockDocumentRequest document;
    document.documentType = QStringLiteral("CPRK");
    document.documentDate = m_dateEdit->date();
    document.handlerName = m_handlerEdit->text().trimmed();
    document.notes = m_notesEdit->toPlainText().trimmed();
    document.submissionToken = m_submissionToken;
    document.productionRunId = m_runCombo->currentData().toLongLong();
    document.lines = {line};

    m_submitButton->setEnabled(false);
    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    QString error;
    const bool ok = service.postFinishedGoodsInbound(document, &posted, &error);
    m_submitButton->setEnabled(m_session.canPostProduction());
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("成品入库失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("成品入库完成"),
                             QStringLiteral("成品入库单 %1 已生效。").arg(posted.documentNumber));
    resetSubmissionToken();
    m_serialEdit->clear();
    m_notesEdit->clear();
    emit stockChanged();
    refreshReferenceData();
}
