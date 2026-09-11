#include "ui/pages/ProductionReturnPage.h"

#include "services/InventoryService.h"
#include "services/OfficeTemplateService.h"
#include "ui/dialogs/DocumentTemplateDialog.h"
#include "ui/widgets/ComboBoxSearch.h"
#include "ui/widgets/TableExcelExport.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace {
constexpr int SelectColumn = 0;
constexpr int MaterialColumn = 1;
constexpr int BatchColumn = 2;
constexpr int OriginalColumn = 3;
constexpr int ReturnedColumn = 4;
constexpr int ReversedColumn = 5;
constexpr int RemainingColumn = 6;
constexpr int QuantityColumn = 7;
constexpr int WarehouseColumn = 8;
constexpr int LocationColumn = 9;
constexpr int SerialColumn = 10;
constexpr int SourceItemRole = Qt::UserRole;
constexpr int MaterialIdRole = Qt::UserRole + 1;
constexpr int RequireSerialRole = Qt::UserRole + 2;
}

ProductionReturnPage::ProductionReturnPage(QSqlDatabase database,
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
    auto *heading = new QLabel(QStringLiteral("生产退料"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    panelLayout->addWidget(heading);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_runCombo = new QComboBox(panel);
    ComboBoxSearch::enableContainsSearch(
        m_runCombo, QStringLiteral("输入生产批次、物料编码或名称检索"));
    m_documentCombo = new QComboBox(panel);
    m_dateEdit = new QDateEdit(QDate::currentDate(), panel);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_handlerEdit = new QLineEdit(m_session.displayName, panel);
    m_notesEdit = new QTextEdit(panel);
    m_notesEdit->setMaximumHeight(60);
    form->addRow(QStringLiteral("生产批次 *"), m_runCombo);
    form->addRow(QStringLiteral("原领料单 *"), m_documentCombo);
    form->addRow(QStringLiteral("退料日期 *"), m_dateEdit);
    form->addRow(QStringLiteral("退料人员"), m_handlerEdit);
    form->addRow(QStringLiteral("备注"), m_notesEdit);
    panelLayout->addLayout(form);

    auto *hint = new QLabel(QStringLiteral("勾选需要退回的原领料明细，并填写本次数量和退回库位。"), panel);
    hint->setObjectName(QStringLiteral("mutedText"));
    panelLayout->addWidget(hint);
    m_linesTable = new QTableWidget(0, 11, panel);
    m_linesTable->setHorizontalHeaderLabels({QStringLiteral("选择"), QStringLiteral("物料"),
                                             QStringLiteral("批次"), QStringLiteral("原领"),
                                             QStringLiteral("已退"), QStringLiteral("已撤销"),
                                             QStringLiteral("可退"), QStringLiteral("本次退料"),
                                             QStringLiteral("退回仓库"), QStringLiteral("退回库位"),
                                             QStringLiteral("SN")});
    m_linesTable->verticalHeader()->setVisible(false);
    m_linesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_linesTable->horizontalHeader()->setSectionResizeMode(MaterialColumn, QHeaderView::Stretch);
    m_linesTable->setMinimumHeight(230);
    panelLayout->addWidget(m_linesTable);
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 10, 0, 0);
    actions->addStretch();
    m_submitButton = new QPushButton(QStringLiteral("确认并退料"), panel);
    m_submitButton->setProperty("primary", true);
    m_submitButton->setFixedSize(110, 34);
    m_submitButton->setEnabled(m_session.canPostProduction());
    actions->addWidget(m_submitButton);
    panelLayout->addLayout(actions);
    root->addWidget(panel);

    auto *recentPanel = new QFrame(this);
    recentPanel->setObjectName(QStringLiteral("panel"));
    auto *recentLayout = new QVBoxLayout(recentPanel);
    auto *recentToolbar = new QHBoxLayout;
    recentToolbar->addWidget(new QLabel(QStringLiteral("近期生产退料单"), recentPanel));
    recentToolbar->addStretch();
    auto *fullScreenRecentButton = new QPushButton(QStringLiteral("全屏显示"), recentPanel);
    recentToolbar->addWidget(fullScreenRecentButton);
    recentLayout->addLayout(recentToolbar);
    m_recentTable = new QTableWidget(0, 4, recentPanel);
    m_recentTable->setProperty("businessDocumentTable", true);
    m_recentTable->setHorizontalHeaderLabels({QStringLiteral("退料单号"), QStringLiteral("日期"),
                                              QStringLiteral("生产批次"), QStringLiteral("原领料单")});
    m_recentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_recentTable->horizontalHeader()->setStretchLastSection(true);
    recentLayout->addWidget(m_recentTable);
    root->addWidget(recentPanel, 1);

    connect(fullScreenRecentButton, &QPushButton::clicked, this, [this] {
        TableExcelExport::fullScreenTable(
            m_recentTable, QStringLiteral("全部生产退料单"), this, [this] { refreshRecentDocuments(); });
    });
    connect(m_runCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ProductionReturnPage::loadDocuments);
    connect(m_documentCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ProductionReturnPage::loadSourceLines);
    connect(m_submitButton, &QPushButton::clicked, this, &ProductionReturnPage::submit);
    resetSubmissionToken();
    refreshReferenceData();
}

void ProductionReturnPage::resetSubmissionToken()
{
    m_submissionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

int ProductionReturnPage::rowForWidget(const QWidget *widget, int column) const
{
    for (int row = 0; row < m_linesTable->rowCount(); ++row) {
        if (m_linesTable->cellWidget(row, column) == widget) return row;
    }
    return -1;
}

void ProductionReturnPage::refreshReferenceData()
{
    const QVariant selected = m_runCombo->currentData();
    m_runCombo->blockSignals(true);
    m_runCombo->clear();
    QSqlQuery runs(m_database);
    runs.exec(QStringLiteral(
        "SELECT p.id,p.batch_no,p.product_name,m.code FROM production_runs p "
        "JOIN materials m ON m.id=p.product_material_id "
        "WHERE EXISTS(SELECT 1 FROM business_documents d JOIN business_document_items i "
        "ON i.document_id=d.id WHERE d.production_run_id=p.id AND d.document_type='SCLL' "
        "AND d.status IN ('POSTED','PARTIALLY_REVERSED') "
        "AND i.quantity-i.returned_quantity-i.reversed_quantity>0.0000001) "
        "ORDER BY p.id DESC"));
    while (runs.next()) {
        m_runCombo->addItem(QStringLiteral("%1 - %2 - %3")
                                .arg(runs.value(1).toString(), runs.value(3).toString(),
                                     runs.value(2).toString()),
                            runs.value(0));
    }
    const int index = m_runCombo->findData(selected);
    if (index >= 0) m_runCombo->setCurrentIndex(index);
    m_runCombo->blockSignals(false);
    loadDocuments();
    refreshRecentDocuments();
}

void ProductionReturnPage::loadDocuments()
{
    const QVariant selected = m_documentCombo->currentData();
    m_documentCombo->blockSignals(true);
    m_documentCombo->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT d.id,d.document_no,d.document_date FROM business_documents d "
        "WHERE d.production_run_id=? AND d.document_type='SCLL' "
        "AND d.status IN ('POSTED','PARTIALLY_REVERSED') "
        "AND EXISTS(SELECT 1 FROM business_document_items i WHERE i.document_id=d.id "
        "AND i.quantity-i.returned_quantity-i.reversed_quantity>0.0000001) "
        "ORDER BY d.id DESC"));
    query.addBindValue(m_runCombo->currentData());
    query.exec();
    while (query.next()) {
        m_documentCombo->addItem(QStringLiteral("%1（%2）")
                                     .arg(query.value(1).toString(), query.value(2).toString()),
                                 query.value(0));
    }
    const int index = m_documentCombo->findData(selected);
    if (index >= 0) m_documentCombo->setCurrentIndex(index);
    m_documentCombo->blockSignals(false);
    loadSourceLines();
}

void ProductionReturnPage::loadSourceLines()
{
    m_linesTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT i.id,m.id,m.code,m.name,i.batch_no,i.quantity,i.returned_quantity,"
        "i.reversed_quantity,i.quantity-i.returned_quantity-i.reversed_quantity,"
        "m.require_serial,i.warehouse_id,i.location_id FROM business_document_items i "
        "JOIN materials m ON m.id=i.material_id WHERE i.document_id=? "
        "AND i.quantity-i.returned_quantity-i.reversed_quantity>0.0000001 ORDER BY i.line_number"));
    query.addBindValue(m_documentCombo->currentData());
    query.exec();
    while (query.next()) {
        const int row = m_linesTable->rowCount();
        m_linesTable->insertRow(row);
        auto *selection = new QTableWidgetItem;
        selection->setCheckState(Qt::Unchecked);
        selection->setData(SourceItemRole, query.value(0));
        selection->setData(MaterialIdRole, query.value(1));
        selection->setData(RequireSerialRole, query.value(9));
        m_linesTable->setItem(row, SelectColumn, selection);
        m_linesTable->setItem(row, MaterialColumn,
                              new QTableWidgetItem(QStringLiteral("%1 - %2")
                                                       .arg(query.value(2).toString(),
                                                            query.value(3).toString())));
        for (int column = BatchColumn; column <= RemainingColumn; ++column) {
            m_linesTable->setItem(row, column,
                                  new QTableWidgetItem(query.value(column + 2).toString()));
        }
        auto *quantity = new QDoubleSpinBox(m_linesTable);
        quantity->setDecimals(6);
        quantity->setRange(0.000001, query.value(8).toDouble());
        quantity->setValue(query.value(8).toDouble());
        auto *warehouse = new QComboBox(m_linesTable);
        QSqlQuery warehouses(m_database);
        warehouses.exec(QStringLiteral(
            "SELECT id,code,name FROM warehouses WHERE is_active=1 ORDER BY code"));
        while (warehouses.next()) {
            warehouse->addItem(QStringLiteral("%1 - %2")
                                   .arg(warehouses.value(1).toString(),
                                        warehouses.value(2).toString()),
                               warehouses.value(0));
        }
        const int warehouseIndex = warehouse->findData(query.value(10));
        if (warehouseIndex >= 0) warehouse->setCurrentIndex(warehouseIndex);
        auto *location = new QComboBox(m_linesTable);
        location->setProperty("preferredLocation", query.value(11));
        auto *serial = new QPushButton(query.value(9).toBool()
                                           ? QStringLiteral("选择SN")
                                           : QStringLiteral("无需选择"),
                                       m_linesTable);
        serial->setEnabled(query.value(9).toBool());
        m_linesTable->setCellWidget(row, QuantityColumn, quantity);
        m_linesTable->setCellWidget(row, WarehouseColumn, warehouse);
        m_linesTable->setCellWidget(row, LocationColumn, location);
        m_linesTable->setCellWidget(row, SerialColumn, serial);
        connect(warehouse, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, warehouse] {
                    const int currentRow = rowForWidget(warehouse, WarehouseColumn);
                    if (currentRow >= 0) loadReturnLocations(currentRow);
                });
        connect(serial, &QPushButton::clicked, this,
                [this, serial] {
                    const int currentRow = rowForWidget(serial, SerialColumn);
                    if (currentRow >= 0) chooseSerials(currentRow);
                });
        loadReturnLocations(row);
    }
    m_submitButton->setEnabled(m_session.canPostProduction()
                               && m_linesTable->rowCount() > 0);
}

void ProductionReturnPage::loadReturnLocations(int row)
{
    auto *warehouse = qobject_cast<QComboBox *>(m_linesTable->cellWidget(row, WarehouseColumn));
    auto *location = qobject_cast<QComboBox *>(m_linesTable->cellWidget(row, LocationColumn));
    if (!warehouse || !location) return;
    const QVariant preferred = location->property("preferredLocation");
    location->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id,code,name FROM locations WHERE warehouse_id=? AND is_active=1 ORDER BY code"));
    query.addBindValue(warehouse->currentData());
    query.exec();
    while (query.next()) {
        QString text = query.value(1).toString();
        if (!query.value(2).toString().isEmpty()) text += QStringLiteral(" - ") + query.value(2).toString();
        location->addItem(text, query.value(0));
    }
    const int index = location->findData(preferred);
    if (index >= 0) location->setCurrentIndex(index);
    location->setProperty("preferredLocation", QVariant());
}

void ProductionReturnPage::chooseSerials(int row)
{
    QTableWidgetItem *source = m_linesTable->item(row, SelectColumn);
    auto *button = qobject_cast<QPushButton *>(m_linesTable->cellWidget(row, SerialColumn));
    if (!source || !button) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("选择退料SN"));
    dialog.resize(480, 430);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("仅显示由该原领料明细发出且尚未退回的SN。"), &dialog));
    auto *list = new QListWidget(&dialog);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    const QStringList previous = button->property("serials").toStringList();
    const QSet<QString> selected(previous.cbegin(), previous.cend());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT DISTINCT sn.serial_no FROM serial_numbers sn "
        "JOIN inventory_ledger_serials ils ON ils.serial_id=sn.id "
        "JOIN inventory_ledger l ON l.id=ils.ledger_id "
        "WHERE l.document_item_id=? AND l.business_type='SCLL' AND sn.status='OUTBOUND' "
        "ORDER BY sn.serial_no"));
    query.addBindValue(source->data(SourceItemRole));
    query.exec();
    while (query.next()) {
        auto *item = new QListWidgetItem(query.value(0).toString(), list);
        item->setSelected(selected.contains(item->text()));
    }
    layout->addWidget(list);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    QStringList serials;
    for (const QListWidgetItem *item : list->selectedItems()) serials.append(item->text());
    serials.sort();
    button->setProperty("serials", serials);
    button->setText(QStringLiteral("已选 %1 个").arg(serials.size()));
}

void ProductionReturnPage::refreshRecentDocuments()
{
    m_recentTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT d.id,d.document_no,d.document_date,p.batch_no,s.document_no "
        "FROM business_documents d JOIN production_runs p ON p.id=d.production_run_id "
        "JOIN business_documents s ON s.id=d.source_document_id "
        "WHERE d.document_type='SCTL' ORDER BY d.id DESC")
        + (m_recentTable->property("tableFullScreenActive").toBool() ? QString() : QStringLiteral(" LIMIT 20")));
    while (query.next()) {
        const int row = m_recentTable->rowCount();
        m_recentTable->insertRow(row);
        for (int column = 0; column < 4; ++column) {
            auto *item = new QTableWidgetItem(query.value(column + 1).toString());
            item->setData(Qt::UserRole, query.value(0));
            m_recentTable->setItem(row, column, item);
        }
    }
}

bool ProductionReturnPage::buildReturnForm(const ProductionReturnRequest &request,
                                           OfficeTemplateDocument *form,
                                           QString *errorMessage) const
{
    if (!form) return false;
    QSqlQuery source(m_database);
    source.prepare(QStringLiteral(
        "SELECT d.document_no,p.batch_no,p.product_name,p.product_model,p.planned_quantity "
        "FROM business_documents d LEFT JOIN production_runs p ON p.id=d.production_run_id "
        "WHERE d.id=? AND d.document_type='SCLL'"));
    source.addBindValue(request.sourceDocumentId);
    if (!source.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("读取原领料单资料失败：%1")
                                .arg(source.lastError().text());
        }
        return false;
    }
    if (!source.next()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("找不到原领料单，无法填写领料单模板。");
        }
        return false;
    }
    const QString issueNumber = source.value(0).toString();
    const double plannedQuantity = source.value(4).toDouble();

    OfficeTemplateDocument document;
    document.kind = OfficeFormKind::ProductionIssue;
    document.documentNumber = QStringLiteral("提交后自动生成");
    document.documentDate = request.documentDate;
    document.fields.insert(QStringLiteral("handler"), request.handlerName);
    document.fields.insert(QStringLiteral("productionBatch"), source.value(1).toString());
    document.fields.insert(QStringLiteral("productName"), source.value(2).toString());
    document.fields.insert(QStringLiteral("productModel"), source.value(3).toString());
    document.fields.insert(QStringLiteral("plannedQuantity"),
                           QString::number(plannedQuantity, 'g', 12));
    document.fields.insert(QStringLiteral("originalIssueNumber"), issueNumber);

    QSqlQuery item(m_database);
    item.prepare(QStringLiteral(
        "SELECT m.code,m.name,m.specification,m.unit,i.batch_no,i.quantity,i.notes "
        "FROM business_document_items i JOIN materials m ON m.id=i.material_id "
        "WHERE i.id=? AND i.document_id=?"));
    for (const ProductionReturnLine &line : request.lines) {
        item.bindValue(0, line.sourceItemId);
        item.bindValue(1, request.sourceDocumentId);
        if (!item.exec() || !item.next()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("找不到原领料明细（%1），无法填写领料单模板。")
                                    .arg(line.sourceItemId);
            }
            return false;
        }
        OfficeTemplateLine templateLine;
        templateLine.materialCode = item.value(0).toString();
        templateLine.materialName = item.value(1).toString();
        templateLine.specification = item.value(2).toString();
        templateLine.unit = item.value(3).toString();
        templateLine.batchNo = item.value(4).toString();
        templateLine.quantity = item.value(5).toDouble();
        if (plannedQuantity > 0.0) {
            templateLine.unitUsage = templateLine.quantity / plannedQuantity;
        }
        templateLine.returnQuantity = line.quantity;
        templateLine.serialNumbers = line.serialNumbers.join(QStringLiteral("、"));
        QStringList notes;
        notes << QStringLiteral("本次退料：%1").arg(QString::number(line.quantity, 'g', 12));
        notes << QStringLiteral("原领料单：%1").arg(issueNumber);
        const QString sourceNotes = item.value(6).toString().trimmed();
        if (!sourceNotes.isEmpty() && !notes.contains(sourceNotes)) notes << sourceNotes;
        templateLine.notes = notes.join(QStringLiteral("；"));
        document.lines.append(templateLine);
    }
    *form = document;
    return true;
}

void ProductionReturnPage::submit()
{
    ProductionReturnRequest request;
    request.sourceDocumentId = m_documentCombo->currentData().toLongLong();
    request.documentDate = m_dateEdit->date();
    request.handlerName = m_handlerEdit->text().trimmed();
    request.notes = m_notesEdit->toPlainText().trimmed();
    request.submissionToken = m_submissionToken;
    for (int row = 0; row < m_linesTable->rowCount(); ++row) {
        QTableWidgetItem *source = m_linesTable->item(row, SelectColumn);
        if (!source || source->checkState() != Qt::Checked) continue;
        auto *quantity = qobject_cast<QDoubleSpinBox *>(m_linesTable->cellWidget(row, QuantityColumn));
        auto *warehouse = qobject_cast<QComboBox *>(m_linesTable->cellWidget(row, WarehouseColumn));
        auto *location = qobject_cast<QComboBox *>(m_linesTable->cellWidget(row, LocationColumn));
        auto *serial = qobject_cast<QPushButton *>(m_linesTable->cellWidget(row, SerialColumn));
        if (!quantity || !warehouse || !location || location->currentIndex() < 0) {
            QMessageBox::warning(this, QStringLiteral("资料不完整"),
                                 QStringLiteral("第 %1 行缺少退回仓库或库位。").arg(row + 1));
            return;
        }
        ProductionReturnLine line;
        line.sourceItemId = source->data(SourceItemRole).toLongLong();
        line.quantity = quantity->value();
        line.warehouseId = warehouse->currentData().toLongLong();
        line.locationId = location->currentData().toLongLong();
        line.serialNumbers = serial ? serial->property("serials").toStringList() : QStringList();
        if (source->data(RequireSerialRole).toBool()) {
            const double roundedQuantity = std::round(line.quantity);
            if (std::abs(line.quantity - roundedQuantity) > 0.0000001
                || line.serialNumbers.size() != static_cast<int>(roundedQuantity)) {
                QMessageBox::warning(this, QStringLiteral("SN数量不一致"),
                                     QStringLiteral("第 %1 行SN数量必须与整数退料数量一致。").arg(row + 1));
                return;
            }
        }
        request.lines.append(line);
    }
    if (request.sourceDocumentId <= 0 || request.lines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("未选择退料明细"),
                             QStringLiteral("请选择原领料单并勾选至少一条退料明细。"));
        return;
    }

    OfficeTemplateDocument returnForm;
    QString formError;
    if (!buildReturnForm(request, &returnForm, &formError)) {
        QMessageBox::warning(this, QStringLiteral("无法填写领料单模板"), formError);
        return;
    }
    DocumentTemplateDialog formDialog(returnForm, this);
    if (formDialog.exec() != QDialog::Accepted) return;
    returnForm = formDialog.document();

    if (QMessageBox::question(this, QStringLiteral("确认生产退料"),
        QStringLiteral("确认提交 %1 条退料明细？库存将整单增加并生成流水。")
            .arg(request.lines.size())) != QMessageBox::Yes) {
        return;
    }
    m_submitButton->setEnabled(false);
    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    QString error;
    const bool ok = service.postProductionReturn(request, &posted, &error);
    m_submitButton->setEnabled(m_session.canPostProduction());
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("生产退料失败"), error);
        return;
    }
    returnForm.documentNumber = posted.documentNumber;
    if (OfficeTemplateService::attachToDocument(returnForm, m_database, m_session.userId,
                                                posted.documentId, &formError)) {
        QMessageBox::information(
            this, QStringLiteral("生产退料完成"),
            QStringLiteral("退料单 %1 已生效，模板表单已保存到数据库附件和“我的文档\\冰美肌仓库系统表单\\领料单”，并已自动打开。")
                .arg(posted.documentNumber));
    } else {
        QMessageBox::warning(
            this, QStringLiteral("退料已完成，但模板处理未全部完成"),
            QStringLiteral("退料单 %1 及本次库存退料已生效，请勿重复提交退料；"
                           "以下表单保存或打开步骤未完成：\n\n%2")
                .arg(posted.documentNumber, formError));
    }
    resetSubmissionToken();
    m_notesEdit->clear();
    emit stockChanged();
    refreshReferenceData();
}
