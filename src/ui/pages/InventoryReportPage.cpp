#include "ui/pages/InventoryReportPage.h"

#include "import/XlsxExporter.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDate>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include <utility>

namespace {
QString quantity(double value)
{
    return QString::number(value, 'g', 12);
}

QTableWidgetItem *textItem(const QString &value)
{
    return new QTableWidgetItem(value);
}

QTableWidgetItem *quantityItem(double value)
{
    auto *item = new QTableWidgetItem(quantity(value));
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

QFrame *metricCard(const QString &title, QLabel **valueLabel, QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("metricCard"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 12, 16, 12);
    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("mutedText"));
    *valueLabel = new QLabel(QStringLiteral("0"), card);
    (*valueLabel)->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;"));
    layout->addWidget(titleLabel);
    layout->addWidget(*valueLabel);
    return card;
}

void configureTable(QTableWidget *table)
{
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
}
}

InventoryReportPage::InventoryReportPage(QSqlDatabase database, QWidget *parent)
    : QWidget(parent), m_database(std::move(database))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *toolbar = new QHBoxLayout;
    m_periodMode = new QComboBox(this);
    m_periodMode->addItem(QStringLiteral("年度统计"), QStringLiteral("YEAR"));
    m_periodMode->addItem(QStringLiteral("月度统计"), QStringLiteral("MONTH"));
    m_yearSpin = new QSpinBox(this);
    m_yearSpin->setRange(2000, 2100);
    m_yearSpin->setValue(QDate::currentDate().year());
    m_yearSpin->setSuffix(QStringLiteral(" 年"));
    m_monthCombo = new QComboBox(this);
    for (int month = 1; month <= 12; ++month)
        m_monthCombo->addItem(QStringLiteral("%1 月").arg(month), month);
    m_monthCombo->setCurrentIndex(QDate::currentDate().month() - 1);
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("物料编码或名称"));
    m_searchEdit->setClearButtonEnabled(true);
    auto *searchButton = new QPushButton(QStringLiteral("查询"), this);
    searchButton->setProperty("primary", true);
    auto *currentButton = new QPushButton(QStringLiteral("本期"), this);
    m_exportButton = new QPushButton(QStringLiteral("导出月报Excel"), this);
    toolbar->addWidget(m_periodMode);
    toolbar->addWidget(m_yearSpin);
    toolbar->addWidget(m_monthCombo);
    toolbar->addWidget(m_searchEdit, 1);
    toolbar->addWidget(searchButton);
    toolbar->addWidget(currentButton);
    toolbar->addWidget(m_exportButton);
    root->addLayout(toolbar);

    auto *metrics = new QHBoxLayout;
    metrics->addWidget(metricCard(QStringLiteral("入库总量"), &m_inboundTotal, this));
    metrics->addWidget(metricCard(QStringLiteral("出库总量"), &m_outboundTotal, this));
    metrics->addWidget(metricCard(QStringLiteral("赠送净数量"), &m_giftTotal, this));
    metrics->addWidget(metricCard(QStringLiteral("对账净数量"), &m_billableTotal, this));
    root->addLayout(metrics);

    m_scopeLabel = new QLabel(this);
    m_scopeLabel->setObjectName(QStringLiteral("mutedText"));
    m_scopeLabel->setWordWrap(true);
    root->addWidget(m_scopeLabel);

    m_views = new QStackedWidget(this);
    m_annualTable = new QTableWidget(0, 8, m_views);
    m_annualTable->setHorizontalHeaderLabels({
        QStringLiteral("月份"), QStringLiteral("采购数量"), QStringLiteral("采购实收净量"),
        QStringLiteral("赠送净量"), QStringLiteral("对账净量"), QStringLiteral("入库总量"),
        QStringLiteral("出库总量"), QStringLiteral("库存净变动")});
    configureTable(m_annualTable);
    m_views->addWidget(m_annualTable);

    auto *monthlyTabs = new QTabWidget(m_views);
    m_monthlySummaryTable = new QTableWidget(0, 14, monthlyTabs);
    m_monthlySummaryTable->setHorizontalHeaderLabels({
        QStringLiteral("物料编码"), QStringLiteral("物料名称"), QStringLiteral("规格"),
        QStringLiteral("单位"), QStringLiteral("期初库存"), QStringLiteral("采购数量"),
        QStringLiteral("采购实收净量"), QStringLiteral("赠送净量"), QStringLiteral("对账净量"),
        QStringLiteral("其他入库"), QStringLiteral("入库总量"), QStringLiteral("出库总量"),
        QStringLiteral("库存净变动"), QStringLiteral("期末库存")});
    configureTable(m_monthlySummaryTable);
    m_monthlySummaryTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_monthlyDetailTable = new QTableWidget(0, 21, monthlyTabs);
    m_monthlyDetailTable->setHorizontalHeaderLabels({
        QStringLiteral("日期"), QStringLiteral("单据号"), QStringLiteral("业务类型"),
        QStringLiteral("方向"), QStringLiteral("物料编码"), QStringLiteral("物料名称"),
        QStringLiteral("规格"), QStringLiteral("单位"), QStringLiteral("供应商"),
        QStringLiteral("仓库"), QStringLiteral("库位"), QStringLiteral("批次"),
        QStringLiteral("采购数量"), QStringLiteral("采购实收变动"), QStringLiteral("赠送变动"),
        QStringLiteral("对账变动"), QStringLiteral("入库数量"), QStringLiteral("出库数量"),
        QStringLiteral("经办人"), QStringLiteral("操作人"), QStringLiteral("备注")});
    configureTable(m_monthlyDetailTable);
    monthlyTabs->addTab(m_monthlySummaryTable, QStringLiteral("月度汇总"));
    monthlyTabs->addTab(m_monthlyDetailTable, QStringLiteral("出入库明细"));
    m_views->addWidget(monthlyTabs);
    root->addWidget(m_views, 1);

    connect(m_periodMode, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &InventoryReportPage::updatePeriodMode);
    connect(searchButton, &QPushButton::clicked, this, &InventoryReportPage::refresh);
    connect(currentButton, &QPushButton::clicked, this, &InventoryReportPage::resetPeriod);
    connect(m_exportButton, &QPushButton::clicked, this, &InventoryReportPage::exportMonthly);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &InventoryReportPage::refresh);
    updatePeriodMode();
}

void InventoryReportPage::updatePeriodMode()
{
    const bool monthly = m_periodMode->currentData().toString() == QStringLiteral("MONTH");
    m_monthCombo->setEnabled(monthly);
    m_exportButton->setEnabled(monthly);
    m_views->setCurrentIndex(monthly ? 1 : 0);
    refresh();
}

void InventoryReportPage::resetPeriod()
{
    m_yearSpin->setValue(QDate::currentDate().year());
    m_monthCombo->setCurrentIndex(QDate::currentDate().month() - 1);
    m_searchEdit->clear();
    refresh();
}

void InventoryReportPage::refresh()
{
    InventoryReportService service(m_database);
    QString error;
    if (m_periodMode->currentData().toString() == QStringLiteral("YEAR")) {
        QList<AnnualInventoryReportRow> rows;
        if (!service.loadAnnual(m_yearSpin->value(), m_searchEdit->text(), &rows, &error)) {
            QMessageBox::warning(this, QStringLiteral("统计失败"), error);
            return;
        }
        showAnnual(rows);
    } else {
        QList<MonthlyMaterialReportRow> summaryRows;
        QList<InventoryMovementReportRow> detailRows;
        if (!service.loadMonthly(m_yearSpin->value(), m_monthCombo->currentData().toInt(),
                                 m_searchEdit->text(), &summaryRows, &detailRows, &error)) {
            QMessageBox::warning(this, QStringLiteral("统计失败"), error);
            return;
        }
        showMonthly(summaryRows, detailRows);
    }
}

void InventoryReportPage::showAnnual(const QList<AnnualInventoryReportRow> &rows)
{
    m_annualTable->setRowCount(rows.size());
    double inbound = 0.0, outbound = 0.0, gift = 0.0, billable = 0.0;
    for (int index = 0; index < rows.size(); ++index) {
        const AnnualInventoryReportRow &value = rows.at(index);
        m_annualTable->setItem(index, 0, textItem(QStringLiteral("%1 月").arg(value.month)));
        m_annualTable->setItem(index, 1, quantityItem(value.orderedQuantity));
        m_annualTable->setItem(index, 2, quantityItem(value.purchaseReceivedQuantity));
        m_annualTable->setItem(index, 3, quantityItem(value.giftQuantity));
        m_annualTable->setItem(index, 4, quantityItem(value.billableQuantity));
        m_annualTable->setItem(index, 5, quantityItem(value.inboundQuantity));
        m_annualTable->setItem(index, 6, quantityItem(value.outboundQuantity));
        m_annualTable->setItem(index, 7,
                               quantityItem(value.inboundQuantity - value.outboundQuantity));
        inbound += value.inboundQuantity;
        outbound += value.outboundQuantity;
        gift += value.giftQuantity;
        billable += value.billableQuantity;
    }
    m_inboundTotal->setText(quantity(inbound));
    m_outboundTotal->setText(quantity(outbound));
    m_giftTotal->setText(quantity(gift));
    m_billableTotal->setText(quantity(billable));
    m_scopeLabel->setText(QStringLiteral(
        "%1 年统计。采购实收、赠送和对账均为扣除采购入库撤销后的净量；内部调拨不计入公司级出入库总量。")
                              .arg(m_yearSpin->value()));
}

void InventoryReportPage::showMonthly(
    const QList<MonthlyMaterialReportRow> &summaryRows,
    const QList<InventoryMovementReportRow> &detailRows)
{
    m_summaryRows = summaryRows;
    m_detailRows = detailRows;
    m_monthlySummaryTable->setRowCount(summaryRows.size());
    double inbound = 0.0, outbound = 0.0, gift = 0.0, billable = 0.0;
    for (int row = 0; row < summaryRows.size(); ++row) {
        const MonthlyMaterialReportRow &value = summaryRows.at(row);
        const QList<QString> texts = {value.materialCode, value.materialName,
                                      value.specification, value.unit};
        for (int column = 0; column < texts.size(); ++column)
            m_monthlySummaryTable->setItem(row, column, textItem(texts.at(column)));
        const QList<double> values = {value.openingQuantity, value.orderedQuantity,
            value.purchaseReceivedQuantity, value.giftQuantity, value.billableQuantity,
            value.otherInboundQuantity, value.inboundQuantity, value.outboundQuantity,
            value.inboundQuantity - value.outboundQuantity, value.closingQuantity};
        for (int column = 0; column < values.size(); ++column)
            m_monthlySummaryTable->setItem(row, column + 4, quantityItem(values.at(column)));
        inbound += value.inboundQuantity;
        outbound += value.outboundQuantity;
        gift += value.giftQuantity;
        billable += value.billableQuantity;
    }

    m_monthlyDetailTable->setRowCount(detailRows.size());
    for (int row = 0; row < detailRows.size(); ++row) {
        const InventoryMovementReportRow &value = detailRows.at(row);
        const QList<QString> first = {value.documentDate.toString(QStringLiteral("yyyy-MM-dd")),
            value.documentNumber, InventoryReportService::documentTypeName(value.documentType),
            InventoryReportService::directionName(value.direction), value.materialCode,
            value.materialName, value.specification, value.unit, value.supplier, value.warehouse,
            value.location, value.batchNumber};
        for (int column = 0; column < first.size(); ++column)
            m_monthlyDetailTable->setItem(row, column, textItem(first.at(column)));
        const QList<double> values = {value.orderedQuantity, value.purchaseReceivedQuantity,
            value.giftQuantity, value.billableQuantity, value.inboundQuantity,
            value.outboundQuantity};
        for (int column = 0; column < values.size(); ++column)
            m_monthlyDetailTable->setItem(row, column + 12, quantityItem(values.at(column)));
        m_monthlyDetailTable->setItem(row, 18, textItem(value.handlerName));
        m_monthlyDetailTable->setItem(row, 19, textItem(value.operatorName));
        m_monthlyDetailTable->setItem(row, 20, textItem(value.notes));
    }
    m_inboundTotal->setText(quantity(inbound));
    m_outboundTotal->setText(quantity(outbound));
    m_giftTotal->setText(quantity(gift));
    m_billableTotal->setText(quantity(billable));
    m_scopeLabel->setText(QStringLiteral(
        "%1 年 %2 月。期末库存=期初库存+入库总量-出库总量；采购、赠送和对账显示净量；内部调拨不计入本报表。")
                              .arg(m_yearSpin->value())
                              .arg(m_monthCombo->currentData().toInt()));
}

void InventoryReportPage::exportMonthly()
{
    if (m_periodMode->currentData().toString() != QStringLiteral("MONTH")) return;
    const QString fileName = QStringLiteral("物料出入库月报-%1-%2.xlsx")
                                 .arg(m_yearSpin->value())
                                 .arg(m_monthCombo->currentData().toInt(), 2, 10, QLatin1Char('0'));
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出物料出入库月报"), documents + QLatin1Char('/') + fileName,
        QStringLiteral("Excel 工作簿 (*.xlsx)"));
    if (path.isEmpty()) return;

    QList<QList<QVariant>> summaryData;
    for (const MonthlyMaterialReportRow &row : std::as_const(m_summaryRows)) {
        summaryData.append({row.materialCode, row.materialName, row.specification, row.unit,
            row.openingQuantity, row.orderedQuantity, row.purchaseReceivedQuantity,
            row.giftQuantity, row.billableQuantity, row.otherInboundQuantity,
            row.inboundQuantity, row.outboundQuantity,
            row.inboundQuantity - row.outboundQuantity, row.closingQuantity});
    }
    QList<QList<QVariant>> detailData;
    for (const InventoryMovementReportRow &row : std::as_const(m_detailRows)) {
        detailData.append({row.documentDate, row.documentNumber,
            InventoryReportService::documentTypeName(row.documentType),
            InventoryReportService::directionName(row.direction), row.materialCode,
            row.materialName, row.specification, row.unit, row.supplier, row.warehouse,
            row.location, row.batchNumber, row.orderedQuantity,
            row.purchaseReceivedQuantity, row.giftQuantity, row.billableQuantity,
            row.inboundQuantity, row.outboundQuantity, row.handlerName,
            row.operatorName, row.notes});
    }
    const QStringList summaryHeaders = {
        QStringLiteral("物料编码"), QStringLiteral("物料名称"), QStringLiteral("规格"),
        QStringLiteral("单位"), QStringLiteral("期初库存"), QStringLiteral("采购数量"),
        QStringLiteral("采购实收净量"), QStringLiteral("赠送净量"), QStringLiteral("对账净量"),
        QStringLiteral("其他入库"), QStringLiteral("入库总量"), QStringLiteral("出库总量"),
        QStringLiteral("库存净变动"), QStringLiteral("期末库存")};
    const QStringList detailHeaders = {
        QStringLiteral("日期"), QStringLiteral("单据号"), QStringLiteral("业务类型"),
        QStringLiteral("方向"), QStringLiteral("物料编码"), QStringLiteral("物料名称"),
        QStringLiteral("规格"), QStringLiteral("单位"), QStringLiteral("供应商"),
        QStringLiteral("仓库"), QStringLiteral("库位"), QStringLiteral("批次"),
        QStringLiteral("采购数量"), QStringLiteral("采购实收变动"), QStringLiteral("赠送变动"),
        QStringLiteral("对账变动"), QStringLiteral("入库数量"), QStringLiteral("出库数量"),
        QStringLiteral("经办人"), QStringLiteral("操作人"), QStringLiteral("备注")};
    QString error;
    if (!XlsxExporter::writeWorkbook(path,
            {{QStringLiteral("月度汇总"), summaryHeaders, summaryData},
             {QStringLiteral("出入库明细"), detailHeaders, detailData}}, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("导出完成"),
                             QStringLiteral("月度报表已保存：\n%1").arg(path));
}
