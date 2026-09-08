#include "ui/pages/ExcelImportPage.h"

#include "services/InventoryService.h"
#include "import/XlsxExporter.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateEdit>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QVBoxLayout>

#include <utility>

ExcelImportPage::ExcelImportPage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(20, 18, 20, 20);
    auto *heading = new QLabel(QStringLiteral("旧库存 Excel 导入"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    layout->addWidget(heading);
    auto *hint = new QLabel(QStringLiteral(
        "适配“冰美肌库存-0629.xlsx”结构。先预览再导入；零库存自动跳过，负库存和无编码数据不会入账。"), panel);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("mutedText"));
    layout->addWidget(hint);

    auto *fileRow = new QHBoxLayout;
    m_fileEdit = new QLineEdit(panel);
    m_fileEdit->setPlaceholderText(QStringLiteral("请选择 .xlsx 文件"));
    auto *browse = new QPushButton(QStringLiteral("浏览…"), panel);
    auto *previewButton = new QPushButton(QStringLiteral("解析并预览"), panel);
    fileRow->addWidget(m_fileEdit, 1);
    fileRow->addWidget(browse);
    fileRow->addWidget(previewButton);
    layout->addLayout(fileRow);
    auto *exchangeRow = new QHBoxLayout;
    auto *materialTemplate = new QPushButton(QStringLiteral("下载物料模板"), panel);
    auto *initialTemplate = new QPushButton(QStringLiteral("下载期初库存模板"), panel);
    auto *inventoryExport = new QPushButton(QStringLiteral("导出当前库存"), panel);
    exchangeRow->addWidget(materialTemplate);
    exchangeRow->addWidget(initialTemplate);
    exchangeRow->addWidget(inventoryExport);
    exchangeRow->addStretch();
    layout->addLayout(exchangeRow);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_warehouseCombo = new QComboBox(panel);
    m_locationCombo = new QComboBox(panel);
    m_dateEdit = new QDateEdit(QDate::currentDate(), panel);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_handlerEdit = new QLineEdit(m_session.displayName, panel);
    form->addRow(QStringLiteral("导入仓库 *"), m_warehouseCombo);
    form->addRow(QStringLiteral("导入库位 *"), m_locationCombo);
    form->addRow(QStringLiteral("入账日期 *"), m_dateEdit);
    form->addRow(QStringLiteral("经办人员"), m_handlerEdit);
    layout->addLayout(form);
    m_summaryLabel = new QLabel(QStringLiteral("尚未解析文件"), panel);
    m_summaryLabel->setObjectName(QStringLiteral("mutedText"));
    layout->addWidget(m_summaryLabel);
    m_table = new QTableWidget(0, 9, panel);
    m_table->setHorizontalHeaderLabels({QStringLiteral("状态"), QStringLiteral("来源"),
                                        QStringLiteral("物料编码"), QStringLiteral("物料名称"),
                                        QStringLiteral("规格"), QStringLiteral("分类"),
                                        QStringLiteral("批次"), QStringLiteral("数量"),
                                        QStringLiteral("说明")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);
    layout->addWidget(m_table, 1);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    m_importButton = new QPushButton(QStringLiteral("导入可用记录"), panel);
    m_importButton->setProperty("primary", true);
    m_importButton->setEnabled(false);
    actions->addWidget(m_importButton);
    layout->addLayout(actions);
    root->addWidget(panel, 1);

    connect(browse, &QPushButton::clicked, this, &ExcelImportPage::browseFile);
    connect(previewButton, &QPushButton::clicked, this, &ExcelImportPage::preview);
    connect(m_warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ExcelImportPage::loadLocations);
    connect(m_importButton, &QPushButton::clicked, this, &ExcelImportPage::importInventory);
    connect(materialTemplate, &QPushButton::clicked, this, &ExcelImportPage::exportMaterialTemplate);
    connect(initialTemplate, &QPushButton::clicked, this, &ExcelImportPage::exportInitialTemplate);
    connect(inventoryExport, &QPushButton::clicked, this, &ExcelImportPage::exportInventory);
    const QString defaultFile = QStringLiteral("D:/WMS/冰美肌库存-0629.xlsx");
    if (QFileInfo::exists(defaultFile)) m_fileEdit->setText(QDir::toNativeSeparators(defaultFile));
    refreshReferenceData();
}

void ExcelImportPage::exportMaterialTemplate()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存物料导入模板"),
        QStringLiteral("物料导入模板.xlsx"), QStringLiteral("Excel工作簿 (*.xlsx)"));
    if (path.isEmpty()) return;
    QString error;
    const QList<QList<QVariant>> example = {{QStringLiteral("MAT-001"), QStringLiteral("示例物料"),
        QStringLiteral("规格型号"), QStringLiteral("RAW"), QStringLiteral("个"), 0,
        QStringLiteral("仓库编码"), QStringLiteral("库位编码"), QStringLiteral("否"),
        QStringLiteral("否"), QStringLiteral("示例品牌"), QStringLiteral("备注")}};
    if (!XlsxExporter::writeSingleSheet(path, QStringLiteral("物料导入"),
        {QStringLiteral("物料编码"), QStringLiteral("物料名称"), QStringLiteral("规格"),
         QStringLiteral("分类编码"), QStringLiteral("单位"), QStringLiteral("最低库存"),
         QStringLiteral("默认仓库编码"), QStringLiteral("默认库位编码"),
         QStringLiteral("批次管理"), QStringLiteral("SN管理"), QStringLiteral("品牌"),
         QStringLiteral("备注")}, example, &error))
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
    else QMessageBox::information(this, QStringLiteral("导出完成"), QStringLiteral("物料导入模板已保存。"));
}

void ExcelImportPage::exportInitialTemplate()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存期初库存模板"),
        QStringLiteral("期初库存导入模板.xlsx"), QStringLiteral("Excel工作簿 (*.xlsx)"));
    if (path.isEmpty()) return;
    QString error;
    const QList<QList<QVariant>> example = {{QStringLiteral("MAT-001"), QStringLiteral("示例物料"),
        QStringLiteral("规格型号"), QStringLiteral("RAW"), QStringLiteral("个"),
        QStringLiteral("BATCH-001"), 100.0}};
    if (!XlsxExporter::writeSingleSheet(path, QStringLiteral("期初库存"),
        {QStringLiteral("物料编码"), QStringLiteral("物料名称"), QStringLiteral("规格"),
         QStringLiteral("分类编码"), QStringLiteral("单位"), QStringLiteral("批次号"),
         QStringLiteral("库存数量")}, example, &error))
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
}

void ExcelImportPage::exportInventory()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出当前库存"),
        QStringLiteral("当前库存.xlsx"), QStringLiteral("Excel工作簿 (*.xlsx)"));
    if (path.isEmpty()) return;
    QList<QList<QVariant>> rows;
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT m.code,m.name,m.specification,c.name,m.unit,w.code,l.code,s.batch_no,s.quantity,s.updated_at "
        "FROM stock_balances s JOIN materials m ON m.id=s.material_id "
        "LEFT JOIN material_categories c ON c.id=m.category_id JOIN warehouses w ON w.id=s.warehouse_id "
        "JOIN locations l ON l.id=s.location_id WHERE s.quantity<>0 ORDER BY m.code,w.code,l.code,s.batch_no"));
    while (query.next()) {
        QList<QVariant> row; for (int column = 0; column < 10; ++column) row.append(query.value(column));
        rows.append(row);
    }
    QString error;
    if (!XlsxExporter::writeSingleSheet(path, QStringLiteral("当前库存"),
        {QStringLiteral("物料编码"), QStringLiteral("物料名称"), QStringLiteral("规格"),
         QStringLiteral("分类"), QStringLiteral("单位"), QStringLiteral("仓库"),
         QStringLiteral("库位"), QStringLiteral("批次"), QStringLiteral("数量"),
         QStringLiteral("更新时间")}, rows, &error))
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
    else QMessageBox::information(this, QStringLiteral("导出完成"), QStringLiteral("当前库存已导出。"));
}

void ExcelImportPage::refreshReferenceData()
{
    const QVariant previous = m_warehouseCombo->currentData();
    m_warehouseCombo->blockSignals(true);
    m_warehouseCombo->clear();
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("SELECT id,code,name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (query.next())
        m_warehouseCombo->addItem(QStringLiteral("%1 - %2").arg(query.value(1).toString(),
                                                                 query.value(2).toString()),
                                  query.value(0));
    const int selected = m_warehouseCombo->findData(previous);
    if (selected >= 0) m_warehouseCombo->setCurrentIndex(selected);
    m_warehouseCombo->blockSignals(false);
    loadLocations();
}

void ExcelImportPage::loadLocations()
{
    const QVariant previous = m_locationCombo->currentData();
    m_locationCombo->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id,code,name FROM locations WHERE warehouse_id=? AND is_active=1 ORDER BY code"));
    query.addBindValue(m_warehouseCombo->currentData());
    query.exec();
    while (query.next())
        m_locationCombo->addItem(QStringLiteral("%1 - %2").arg(query.value(1).toString(),
                                                                query.value(2).toString()),
                                 query.value(0));
    const int selected = m_locationCombo->findData(previous);
    if (selected >= 0) m_locationCombo->setCurrentIndex(selected);
    m_importButton->setEnabled(false);
}

void ExcelImportPage::browseFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择旧库存Excel"),
                                                       QFileInfo(m_fileEdit->text()).absolutePath(),
                                                       QStringLiteral("Excel工作簿 (*.xlsx)"));
    if (!path.isEmpty()) {
        m_fileEdit->setText(QDir::toNativeSeparators(path));
        m_importButton->setEnabled(false);
    }
}

void ExcelImportPage::preview()
{
    QString error;
    if (!LegacyInventoryImporter::parseFile(m_fileEdit->text().trimmed(), &m_rows, &error)) {
        QMessageBox::warning(this, QStringLiteral("解析失败"), error);
        return;
    }
    m_table->setRowCount(0);
    int ready = 0;
    int warnings = 0;
    int errors = 0;
    int skipped = 0;
    for (const LegacyImportRow &item : std::as_const(m_rows)) {
        if (item.status == LegacyImportStatus::Ready) ++ready;
        else if (item.status == LegacyImportStatus::Warning) ++warnings;
        else if (item.status == LegacyImportStatus::Error) ++errors;
        else ++skipped;
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QString source = item.sourceRow > 0
            ? QStringLiteral("%1 第%2行").arg(item.sourceSheet).arg(item.sourceRow)
            : item.sourceSheet;
        const QString quantity = item.rawQuantity.isEmpty()
            ? QString() : (item.status == LegacyImportStatus::Ready
                               ? QString::number(item.quantity, 'g', 15) : item.rawQuantity);
        const QStringList columns = {LegacyInventoryImporter::statusText(item.status), source,
                                     item.materialCode, item.materialName, item.specification,
                                     item.categoryCode, item.batchNo, quantity, item.message};
        for (int column = 0; column < columns.size(); ++column)
            m_table->setItem(row, column, new QTableWidgetItem(columns.at(column)));
    }
    m_summaryLabel->setText(QStringLiteral("解析完成：可导入 %1 条，警告 %2 条，错误 %3 条，零库存跳过 %4 条。")
                                .arg(ready).arg(warnings).arg(errors).arg(skipped));
    m_importButton->setEnabled(m_session.canManageWarehouse() && ready > 0
                               && m_locationCombo->currentIndex() >= 0);
}

void ExcelImportPage::importInventory()
{
    if (m_locationCombo->currentIndex() < 0) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"), QStringLiteral("请选择导入库位。"));
        return;
    }
    InitialInventoryRequest request;
    request.documentDate = m_dateEdit->date();
    request.handlerName = m_handlerEdit->text().trimmed();
    request.sourceFile = QFileInfo(m_fileEdit->text().trimmed()).fileName();
    request.warehouseId = m_warehouseCombo->currentData().toLongLong();
    request.locationId = m_locationCombo->currentData().toLongLong();
    for (const LegacyImportRow &item : std::as_const(m_rows)) {
        if (item.status != LegacyImportStatus::Ready) continue;
        InitialInventoryLine line;
        line.materialCode = item.materialCode;
        line.materialName = item.materialName;
        line.specification = item.specification;
        line.categoryCode = item.categoryCode;
        line.unit = item.unit;
        line.batchNo = item.batchNo;
        line.quantity = item.quantity;
        line.notes = QStringLiteral("来源：%1 第%2行").arg(item.sourceSheet).arg(item.sourceRow);
        request.lines.append(line);
    }
    QFile file(m_fileEdit->text().trimmed());
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), QStringLiteral("无法重新读取所选Excel文件。"));
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    request.submissionToken = QStringLiteral("legacy-import:%1:%2:%3")
                                  .arg(QString::fromLatin1(hash.result().toHex()))
                                  .arg(request.warehouseId).arg(request.locationId);
    if (QMessageBox::question(this, QStringLiteral("确认导入"),
        QStringLiteral("确认把 %1 条有效记录导入所选库位？系统会自动创建缺少的物料并生成期初入库单。")
            .arg(request.lines.size())) != QMessageBox::Yes) return;
    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    QString error;
    if (!service.importInitialInventory(request, &posted, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }
    m_importButton->setEnabled(false);
    QMessageBox::information(this, QStringLiteral("导入完成"),
                             QStringLiteral("已导入 %1 条库存，期初入库单：%2")
                                 .arg(request.lines.size()).arg(posted.documentNumber));
    emit stockChanged();
}
