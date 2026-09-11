#include "ui/dialogs/BusinessDocumentEditDialog.h"

#include "services/OfficeTemplateService.h"
#include "ui/widgets/ComboBoxSearch.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace {
enum Column {
    RowNumberColumn,
    MaterialColumn,
    EffectColumn,
    QuantityColumn,
    OrderedColumn,
    GiftColumn,
    BatchColumn,
    SourceLocationColumn,
    TargetLocationColumn,
    SerialColumn,
    SourceItemColumn,
    NotesColumn,
    DeleteColumn,
    ColumnCount
};

QDoubleSpinBox *quantitySpin(double value, QWidget *parent)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setDecimals(6);
    spin->setRange(0.000001, 999999999.0);
    spin->setValue(qMax(0.000001, value));
    spin->setMinimumWidth(115);
    return spin;
}

QStringList splitSerials(QString value)
{
    value.replace(QRegularExpression(QStringLiteral("[，、;；\\s]+")), QStringLiteral(","));
    QStringList result;
    for (const QString &part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString serial = part.trimmed().toUpper();
        if (!serial.isEmpty() && !result.contains(serial)) result.append(serial);
    }
    return result;
}
}

BusinessDocumentEditDialog::BusinessDocumentEditDialog(QSqlDatabase database,
                                                       Session session,
                                                       qlonglong documentId,
                                                       QWidget *parent)
    : QDialog(parent), m_database(std::move(database)), m_session(std::move(session)),
      m_documentId(documentId)
{
    setWindowTitle(QStringLiteral("完整修改已入账单据"));
    setModal(true);
    resize(1420, 860);
    setMinimumSize(1050, 680);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);
    auto *headingRow = new QHBoxLayout;
    auto *heading = new QLabel(QStringLiteral("完整修改单据及库存明细"), this);
    heading->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;"));
    auto *fullScreen = new QPushButton(QStringLiteral("全屏显示"), this);
    headingRow->addWidget(heading);
    headingRow->addStretch();
    headingRow->addWidget(fullScreen);
    root->addLayout(headingRow);
    auto *warning = new QLabel(
        QStringLiteral("保存后会整单替换单头、明细、库存流水和SN关联，并重新计算库存；数据库修改任一步失败都会回滚。"),
        this);
    warning->setWordWrap(true);
    warning->setObjectName(QStringLiteral("mutedText"));
    root->addWidget(warning);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget(scroll);
    auto *bodyLayout = new QVBoxLayout(body);
    auto *headerGrid = new QGridLayout;
    m_numberEdit = new QLineEdit(body);
    m_typeCombo = new QComboBox(body);
    m_typeCombo->setEditable(true);
    const QList<QPair<QString, QString>> types = {
        {QStringLiteral("CGRK"), QStringLiteral("采购入库")},
        {QStringLiteral("SCWG"), QStringLiteral("生产完工入库")},
        {QStringLiteral("TLRK"), QStringLiteral("退料入库")},
        {QStringLiteral("QTRK"), QStringLiteral("其他入库")},
        {QStringLiteral("QC"), QStringLiteral("期初入库")},
        {QStringLiteral("SCLL"), QStringLiteral("生产领料")},
        {QStringLiteral("SCTL"), QStringLiteral("生产退料")},
        {QStringLiteral("CPRK"), QStringLiteral("成品入库")},
        {QStringLiteral("XSCK"), QStringLiteral("销售出库")},
        {QStringLiteral("WXLY"), QStringLiteral("维修领用")},
        {QStringLiteral("YPLY"), QStringLiteral("研发领用")},
        {QStringLiteral("QTCK"), QStringLiteral("其他出库")},
        {QStringLiteral("DB"), QStringLiteral("库存调拨")},
        {QStringLiteral("PD"), QStringLiteral("库存盘点")},
        {QStringLiteral("CX"), QStringLiteral("撤销单")}};
    for (const auto &type : types)
        m_typeCombo->addItem(QStringLiteral("%1 - %2").arg(type.first, type.second), type.first);
    m_directionCombo = new QComboBox(body);
    m_directionCombo->addItem(QStringLiteral("入库"), QStringLiteral("IN"));
    m_directionCombo->addItem(QStringLiteral("出库"), QStringLiteral("OUT"));
    m_directionCombo->addItem(QStringLiteral("调拨"), QStringLiteral("TRANSFER"));
    m_directionCombo->addItem(QStringLiteral("调整"), QStringLiteral("ADJUST"));
    m_statusCombo = new QComboBox(body);
    m_statusCombo->addItem(QStringLiteral("草稿（不影响库存）"), QStringLiteral("DRAFT"));
    m_statusCombo->addItem(QStringLiteral("已入账"), QStringLiteral("POSTED"));
    m_statusCombo->addItem(QStringLiteral("部分撤销"), QStringLiteral("PARTIALLY_REVERSED"));
    m_statusCombo->addItem(QStringLiteral("全部撤销"), QStringLiteral("REVERSED"));
    m_dateEdit = new QDateEdit(body);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_deliveryDateEdit = new QDateEdit(body);
    m_deliveryDateEdit->setCalendarPopup(true);
    m_deliveryDateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_handlerEdit = new QLineEdit(body);
    m_purposeEdit = new QLineEdit(body);
    m_supplierEdit = new QLineEdit(body);
    m_sourceDocumentEdit = new QLineEdit(body);
    m_productionRunEdit = new QLineEdit(body);
    m_inspectionNoticeEdit = new QLineEdit(body);
    m_notesEdit = new QTextEdit(body);
    m_notesEdit->setMaximumHeight(65);

    const auto addHeaderField = [headerGrid](int row, int pair, const QString &label, QWidget *editor) {
        headerGrid->addWidget(new QLabel(label), row, pair * 2);
        headerGrid->addWidget(editor, row, pair * 2 + 1);
    };
    addHeaderField(0, 0, QStringLiteral("单据号 *"), m_numberEdit);
    addHeaderField(0, 1, QStringLiteral("单据类型 *"), m_typeCombo);
    addHeaderField(0, 2, QStringLiteral("库存方向 *"), m_directionCombo);
    addHeaderField(0, 3, QStringLiteral("单据状态 *"), m_statusCombo);
    addHeaderField(1, 0, QStringLiteral("单据日期 *"), m_dateEdit);
    addHeaderField(1, 1, QStringLiteral("送货日期"), m_deliveryDateEdit);
    addHeaderField(1, 2, QStringLiteral("经办人员"), m_handlerEdit);
    addHeaderField(1, 3, QStringLiteral("供应商"), m_supplierEdit);
    addHeaderField(2, 0, QStringLiteral("来源单据ID"), m_sourceDocumentEdit);
    addHeaderField(2, 1, QStringLiteral("生产批次ID"), m_productionRunEdit);
    addHeaderField(2, 2, QStringLiteral("送检通知ID"), m_inspectionNoticeEdit);
    addHeaderField(2, 3, QStringLiteral("业务用途"), m_purposeEdit);
    headerGrid->addWidget(new QLabel(QStringLiteral("备注")), 3, 0);
    headerGrid->addWidget(m_notesEdit, 3, 1, 1, 7);
    for (int column = 1; column < 8; column += 2) headerGrid->setColumnStretch(column, 1);
    bodyLayout->addLayout(headerGrid);

    auto *salesTitle = new QLabel(QStringLiteral("销售/发货资料（非销售单也允许填写或清空）"), body);
    salesTitle->setStyleSheet(QStringLiteral("font-weight:600;"));
    bodyLayout->addWidget(salesTitle);
    auto *salesGrid = new QGridLayout;
    m_customerCompanyEdit = new QLineEdit(body);
    m_destinationEdit = new QLineEdit(body);
    m_contactEdit = new QLineEdit(body);
    m_phoneEdit = new QLineEdit(body);
    m_orderEdit = new QLineEdit(body);
    m_logisticsEdit = new QLineEdit(body);
    m_trackingEdit = new QLineEdit(body);
    const auto addSalesField = [salesGrid](int row, int pair, const QString &label, QWidget *editor) {
        salesGrid->addWidget(new QLabel(label), row, pair * 2);
        salesGrid->addWidget(editor, row, pair * 2 + 1);
    };
    addSalesField(0, 0, QStringLiteral("客户单位"), m_customerCompanyEdit);
    addSalesField(0, 1, QStringLiteral("收货地址"), m_destinationEdit);
    addSalesField(0, 2, QStringLiteral("收货人"), m_contactEdit);
    addSalesField(0, 3, QStringLiteral("联系电话"), m_phoneEdit);
    addSalesField(1, 0, QStringLiteral("合同/订单号"), m_orderEdit);
    addSalesField(1, 1, QStringLiteral("物流公司"), m_logisticsEdit);
    addSalesField(1, 2, QStringLiteral("物流单号"), m_trackingEdit);
    for (int column = 1; column < 8; column += 2) salesGrid->setColumnStretch(column, 1);
    bodyLayout->addLayout(salesGrid);

    auto *lineToolbar = new QHBoxLayout;
    auto *lineTitle = new QLabel(QStringLiteral("库存明细"), body);
    lineTitle->setStyleSheet(QStringLiteral("font-weight:600;"));
    auto *addButton = new QPushButton(QStringLiteral("添加明细"), body);
    lineToolbar->addWidget(lineTitle);
    lineToolbar->addStretch();
    lineToolbar->addWidget(addButton);
    bodyLayout->addLayout(lineToolbar);
    m_lines = new QTableWidget(0, ColumnCount, body);
    m_lines->setHorizontalHeaderLabels({
        QStringLiteral("序号"), QStringLiteral("物料"), QStringLiteral("库存效果"),
        QStringLiteral("数量"), QStringLiteral("采购数量"), QStringLiteral("赠送数量"),
        QStringLiteral("批次"), QStringLiteral("源仓库/库位"), QStringLiteral("目标仓库/库位"),
        QStringLiteral("SN（逗号分隔）"), QStringLiteral("来源明细ID"),
        QStringLiteral("明细备注"), QStringLiteral("操作")});
    m_lines->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_lines->verticalHeader()->hide();
    m_lines->horizontalHeader()->setMinimumSectionSize(75);
    m_lines->setColumnWidth(MaterialColumn, 240);
    m_lines->setColumnWidth(SourceLocationColumn, 190);
    m_lines->setColumnWidth(TargetLocationColumn, 190);
    m_lines->setColumnWidth(SerialColumn, 220);
    m_lines->setColumnWidth(SourceItemColumn, 105);
    m_lines->setColumnWidth(NotesColumn, 180);
    bodyLayout->addWidget(m_lines, 1);
    scroll->setWidget(body);
    root->addWidget(scroll, 1);

    auto *buttons = new QDialogButtonBox(this);
    auto *cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    auto *save = buttons->addButton(QStringLiteral("保存全部修改"), QDialogButtonBox::AcceptRole);
    save->setProperty("primary", true);
    root->addWidget(buttons);
    connect(fullScreen, &QPushButton::clicked, this, &BusinessDocumentEditDialog::toggleFullScreen);
    connect(addButton, &QPushButton::clicked, this, qOverload<>(&BusinessDocumentEditDialog::addLine));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &BusinessDocumentEditDialog::saveDocument);
    if (!loadDocument()) QTimer::singleShot(0, this, &QDialog::reject);
}

bool BusinessDocumentEditDialog::loadDocument()
{
    InventoryService service(m_database, m_session.userId);
    QString error;
    if (!service.loadPostedDocument(m_documentId, &m_original, &error)) {
        QMessageBox::warning(this, QStringLiteral("无法打开单据"), error);
        return false;
    }
    m_numberEdit->setText(m_original.documentNumber);
    int index = m_typeCombo->findData(m_original.documentType);
    if (index >= 0) m_typeCombo->setCurrentIndex(index);
    else m_typeCombo->setEditText(m_original.documentType);
    index = m_directionCombo->findData(m_original.stockDirection);
    if (index >= 0) m_directionCombo->setCurrentIndex(index);
    index = m_statusCombo->findData(m_original.status);
    if (index >= 0) m_statusCombo->setCurrentIndex(index);
    m_dateEdit->setDate(m_original.documentDate);
    m_deliveryDateEdit->setDate(m_original.deliveryDate.isValid()
                                    ? m_original.deliveryDate : m_original.documentDate);
    m_handlerEdit->setText(m_original.handlerName);
    m_purposeEdit->setText(m_original.purpose);
    m_supplierEdit->setText(m_original.supplier);
    m_sourceDocumentEdit->setText(m_original.sourceDocumentId > 0
                                      ? QString::number(m_original.sourceDocumentId) : QString());
    m_productionRunEdit->setText(m_original.productionRunId > 0
                                     ? QString::number(m_original.productionRunId) : QString());
    m_inspectionNoticeEdit->setText(m_original.inspectionNoticeId > 0
                                        ? QString::number(m_original.inspectionNoticeId) : QString());
    m_notesEdit->setPlainText(m_original.notes);
    m_customerCompanyEdit->setText(m_original.customerCompany);
    m_destinationEdit->setText(m_original.destination);
    m_contactEdit->setText(m_original.customerContact);
    m_phoneEdit->setText(m_original.customerPhone);
    m_orderEdit->setText(m_original.salesOrderNumber);
    m_logisticsEdit->setText(m_original.logisticsCompany);
    m_trackingEdit->setText(m_original.trackingNumber);
    for (const PostedDocumentEditLine &line : std::as_const(m_original.lines)) addLine(line);
    return true;
}

QComboBox *BusinessDocumentEditDialog::materialCombo(qlonglong selectedMaterialId) const
{
    auto *combo = new QComboBox(m_lines);
    combo->setMinimumWidth(230);
    combo->addItem(QStringLiteral("请选择物料"), qlonglong(0));
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("SELECT id,code,name FROM materials ORDER BY code COLLATE NOCASE"));
    while (query.next())
        combo->addItem(QStringLiteral("%1 - %2").arg(query.value(1).toString(), query.value(2).toString()),
                       query.value(0));
    ComboBoxSearch::enableContainsSearch(combo, QStringLiteral("输入物料号或名称"));
    const int index = combo->findData(selectedMaterialId);
    if (index >= 0) combo->setCurrentIndex(index);
    return combo;
}

QComboBox *BusinessDocumentEditDialog::locationCombo(qlonglong selectedWarehouseId,
                                                     qlonglong selectedLocationId) const
{
    auto *combo = new QComboBox(m_lines);
    combo->addItem(QStringLiteral("未选择"), QVariantMap{});
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT w.id,l.id,w.code,w.name,l.code,l.name FROM warehouses w "
        "JOIN locations l ON l.warehouse_id=w.id "
        "ORDER BY w.code,l.code"));
    while (query.next()) {
        QVariantMap ids;
        ids.insert(QStringLiteral("warehouseId"), query.value(0));
        ids.insert(QStringLiteral("locationId"), query.value(1));
        combo->addItem(QStringLiteral("%1 / %2 - %3")
                           .arg(query.value(2).toString(), query.value(4).toString(),
                                query.value(5).toString()), ids);
        if (query.value(0).toLongLong() == selectedWarehouseId
            && query.value(1).toLongLong() == selectedLocationId)
            combo->setCurrentIndex(combo->count() - 1);
    }
    return combo;
}

void BusinessDocumentEditDialog::addLine()
{
    PostedDocumentEditLine line;
    line.quantity = 1.0;
    line.movementDirection = m_directionCombo->currentData().toString() == QStringLiteral("OUT")
        ? QStringLiteral("OUT") : QStringLiteral("IN");
    addLine(line);
}

void BusinessDocumentEditDialog::addLine(const PostedDocumentEditLine &line)
{
    const int row = m_lines->rowCount();
    m_lines->insertRow(row);
    auto *numberItem = new QTableWidgetItem(QString::number(row + 1));
    numberItem->setData(Qt::UserRole, line.itemId);
    numberItem->setFlags(numberItem->flags() & ~Qt::ItemIsEditable);
    m_lines->setItem(row, RowNumberColumn, numberItem);
    m_lines->setCellWidget(row, MaterialColumn, materialCombo(line.materialId));
    auto *effect = new QComboBox(m_lines);
    effect->addItem(QStringLiteral("入库"), QStringLiteral("IN"));
    effect->addItem(QStringLiteral("出库"), QStringLiteral("OUT"));
    int effectIndex = effect->findData(line.movementDirection);
    effect->setCurrentIndex(effectIndex >= 0 ? effectIndex : 0);
    m_lines->setCellWidget(row, EffectColumn, effect);
    m_lines->setCellWidget(row, QuantityColumn, quantitySpin(line.quantity, m_lines));
    auto *ordered = quantitySpin(qMax(0.000001, line.orderedQuantity), m_lines);
    ordered->setMinimum(0.0);
    ordered->setValue(line.orderedQuantity);
    m_lines->setCellWidget(row, OrderedColumn, ordered);
    auto *gift = quantitySpin(qMax(0.000001, line.giftQuantity), m_lines);
    gift->setMinimum(0.0);
    gift->setValue(line.giftQuantity);
    m_lines->setCellWidget(row, GiftColumn, gift);
    auto *batch = new QLineEdit(line.batchNo, m_lines);
    m_lines->setCellWidget(row, BatchColumn, batch);
    m_lines->setCellWidget(row, SourceLocationColumn,
                           locationCombo(line.warehouseId, line.locationId));
    m_lines->setCellWidget(row, TargetLocationColumn,
                           locationCombo(line.targetWarehouseId, line.targetLocationId));
    auto *serials = new QLineEdit(line.serialNumbers.join(QStringLiteral(", ")), m_lines);
    serials->setPlaceholderText(QStringLiteral("多个SN用逗号分隔"));
    m_lines->setCellWidget(row, SerialColumn, serials);
    auto *sourceItem = new QLineEdit(line.sourceItemId > 0
                                         ? QString::number(line.sourceItemId) : QString(), m_lines);
    m_lines->setCellWidget(row, SourceItemColumn, sourceItem);
    m_lines->setCellWidget(row, NotesColumn, new QLineEdit(line.notes, m_lines));
    auto *remove = new QPushButton(QStringLiteral("删除"), m_lines);
    remove->setProperty("danger", true);
    m_lines->setCellWidget(row, DeleteColumn, remove);
    connect(remove, &QPushButton::clicked, this, [this, remove] {
        for (int row = 0; row < m_lines->rowCount(); ++row) {
            if (m_lines->cellWidget(row, DeleteColumn) != remove) continue;
            m_lines->removeRow(row);
            for (int next = row; next < m_lines->rowCount(); ++next)
                m_lines->item(next, RowNumberColumn)->setText(QString::number(next + 1));
            break;
        }
    });
}

qlonglong BusinessDocumentEditDialog::idText(const QLineEdit *edit) const
{
    bool ok = false;
    const qlonglong value = edit->text().trimmed().toLongLong(&ok);
    return ok && value > 0 ? value : 0;
}

PostedDocumentEdit BusinessDocumentEditDialog::editedDocument(QString *errorMessage) const
{
    PostedDocumentEdit result;
    result.documentId = m_documentId;
    result.documentNumber = m_numberEdit->text().trimmed();
    // 下拉框允许直接输入自定义类型；始终以当前可见文本的代码部分为准，
    // 避免用户改了文字但仍误用原下拉项的隐藏值。
    result.documentType = m_typeCombo->currentText()
                              .section(QStringLiteral(" - "), 0, 0)
                              .trimmed().toUpper();
    if (result.documentType.isEmpty())
        result.documentType = m_typeCombo->currentData().toString().trimmed().toUpper();
    result.stockDirection = m_directionCombo->currentData().toString();
    result.status = m_statusCombo->currentData().toString();
    result.documentDate = m_dateEdit->date();
    result.deliveryDate = m_deliveryDateEdit->date();
    result.sourceDocumentId = idText(m_sourceDocumentEdit);
    result.productionRunId = idText(m_productionRunEdit);
    result.inspectionNoticeId = idText(m_inspectionNoticeEdit);
    result.handlerName = m_handlerEdit->text().trimmed();
    result.purpose = m_purposeEdit->text().trimmed();
    result.supplier = m_supplierEdit->text().trimmed();
    result.notes = m_notesEdit->toPlainText().trimmed();
    result.customerCompany = m_customerCompanyEdit->text().trimmed();
    result.destination = m_destinationEdit->text().trimmed();
    result.customerContact = m_contactEdit->text().trimmed();
    result.customerPhone = m_phoneEdit->text().trimmed();
    result.salesOrderNumber = m_orderEdit->text().trimmed();
    result.logisticsCompany = m_logisticsEdit->text().trimmed();
    result.trackingNumber = m_trackingEdit->text().trimmed();
    for (int row = 0; row < m_lines->rowCount(); ++row) {
        PostedDocumentEditLine line;
        line.itemId = m_lines->item(row, RowNumberColumn)->data(Qt::UserRole).toLongLong();
        auto *material = qobject_cast<QComboBox *>(m_lines->cellWidget(row, MaterialColumn));
        auto *effect = qobject_cast<QComboBox *>(m_lines->cellWidget(row, EffectColumn));
        auto *quantity = qobject_cast<QDoubleSpinBox *>(m_lines->cellWidget(row, QuantityColumn));
        auto *ordered = qobject_cast<QDoubleSpinBox *>(m_lines->cellWidget(row, OrderedColumn));
        auto *gift = qobject_cast<QDoubleSpinBox *>(m_lines->cellWidget(row, GiftColumn));
        auto *batch = qobject_cast<QLineEdit *>(m_lines->cellWidget(row, BatchColumn));
        auto *source = qobject_cast<QComboBox *>(m_lines->cellWidget(row, SourceLocationColumn));
        auto *target = qobject_cast<QComboBox *>(m_lines->cellWidget(row, TargetLocationColumn));
        auto *serials = qobject_cast<QLineEdit *>(m_lines->cellWidget(row, SerialColumn));
        auto *sourceItem = qobject_cast<QLineEdit *>(m_lines->cellWidget(row, SourceItemColumn));
        auto *notes = qobject_cast<QLineEdit *>(m_lines->cellWidget(row, NotesColumn));
        if (!material || !quantity || !source) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 行编辑控件不完整。").arg(row + 1);
            return {};
        }
        line.materialId = material->currentData().toLongLong();
        line.movementDirection = effect ? effect->currentData().toString() : QStringLiteral("IN");
        line.quantity = quantity->value();
        line.orderedQuantity = ordered ? ordered->value() : 0.0;
        line.giftQuantity = gift ? gift->value() : 0.0;
        line.batchNo = batch ? batch->text().trimmed() : QString();
        const QVariantMap sourceIds = source->currentData().toMap();
        line.warehouseId = sourceIds.value(QStringLiteral("warehouseId")).toLongLong();
        line.locationId = sourceIds.value(QStringLiteral("locationId")).toLongLong();
        const QVariantMap targetIds = target ? target->currentData().toMap() : QVariantMap{};
        line.targetWarehouseId = targetIds.value(QStringLiteral("warehouseId")).toLongLong();
        line.targetLocationId = targetIds.value(QStringLiteral("locationId")).toLongLong();
        line.serialNumbers = splitSerials(serials ? serials->text() : QString());
        if (sourceItem) {
            bool sourceOk = false;
            const qlonglong sourceId = sourceItem->text().trimmed().toLongLong(&sourceOk);
            line.sourceItemId = sourceOk && sourceId > 0 ? sourceId : 0;
        }
        line.notes = notes ? notes->text().trimmed() : QString();
        result.lines.append(line);
    }
    return result;
}

void BusinessDocumentEditDialog::saveDocument()
{
    QString error;
    const PostedDocumentEdit document = editedDocument(&error);
    if (!error.isEmpty() || document.lines.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("单据资料不完整"),
                             error.isEmpty() ? QStringLiteral("至少保留一行有效明细。") : error);
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("确认完整修改"),
            QStringLiteral("确认保存对 %1 的全部修改？\n\n系统会重建该单据的库存流水，并重新计算库存和SN状态。")
                .arg(document.documentNumber)) != QMessageBox::Yes) return;
    InventoryService service(m_database, m_session.userId);
    if (!service.revisePostedDocument(document, &error)) {
        QMessageBox::warning(this, QStringLiteral("修改失败，已回滚"), error);
        return;
    }
    QStringList formErrors;
    OfficeTemplateService::synchronizeDocumentForms(
        m_database, m_session.userId, document.documentId, &formErrors, false);
    m_saved = true;
    if (formErrors.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("修改完成"),
                                 QStringLiteral("单据、库存、SN、数据库表单附件和本地归档已同步更新。"));
    } else {
        QMessageBox::warning(
            this, QStringLiteral("单据已修改，部分表单未同步"),
            QStringLiteral("单据和库存已成功更新；以下表单可在附件管理中重新生成：\n\n%1")
                .arg(formErrors.join(QStringLiteral("\n"))));
    }
    accept();
}

void BusinessDocumentEditDialog::toggleFullScreen()
{
    if (m_fullScreen) showNormal();
    else showMaximized();
    m_fullScreen = !m_fullScreen;
}
