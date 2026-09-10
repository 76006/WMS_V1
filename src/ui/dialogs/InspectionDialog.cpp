#include "ui/dialogs/InspectionDialog.h"

#include "services/OfficeTemplateService.h"
#include "ui/widgets/ComboBoxSearch.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPushButton>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {
constexpr qint64 MaximumAttachmentBytes = 50LL * 1024LL * 1024LL;

QString displayQuantity(double value)
{
    return QString::number(value, 'g', 12);
}

QString inspectionNumberPrefix(const QDate &date)
{
    return QStringLiteral("BMJ-JY-%1-").arg(date.toString(QStringLiteral("yyyyMMdd")));
}

bool isInspectionNumberForDate(const QString &number, const QDate &date)
{
    const QString prefix = inspectionNumberPrefix(date);
    if (!number.startsWith(prefix) || number.size() != prefix.size() + 3) return false;
    bool ok = false;
    const int sequence = number.right(3).toInt(&ok);
    return ok && sequence >= 1 && sequence <= 999
        && number.right(3) == QStringLiteral("%1").arg(sequence, 3, 10, QLatin1Char('0'));
}

// 返回 true 表示查询成功；used 输出单号是否已被入库送检明细占用。
bool inspectionNumberUsed(QSqlDatabase database,
                          const QString &number,
                          bool *used,
                          QString *errorMessage)
{
    if (used) *used = false;
    if (errorMessage) errorMessage->clear();
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM inbound_inspection_details WHERE inspection_no=? LIMIT 1"));
    query.addBindValue(number.trimmed());
    if (!query.exec()) {
        if (errorMessage) *errorMessage = query.lastError().text();
        return false;
    }
    if (used) *used = query.next();
    return true;
}

QString nextInspectionNumber(QSqlDatabase database, const QDate &date, QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (!date.isValid()) {
        if (errorMessage) *errorMessage = QStringLiteral("送检日期无效。");
        return {};
    }
    const QString prefix = inspectionNumberPrefix(date);
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT inspection_no FROM inbound_inspection_details WHERE inspection_no LIKE ?"));
    query.addBindValue(prefix + QLatin1Char('%'));
    if (!query.exec()) {
        if (errorMessage) *errorMessage = query.lastError().text();
        return {};
    }
    int maximumSequence = 0;
    while (query.next()) {
        const QString number = query.value(0).toString().trimmed();
        if (!isInspectionNumberForDate(number, date)) continue;
        maximumSequence = qMax(maximumSequence, number.right(3).toInt());
    }
    if (maximumSequence >= 999) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("%1的三位送检单流水号已用完。")
                                .arg(date.toString(QStringLiteral("yyyy-MM-dd")));
        }
        return {};
    }
    return prefix + QStringLiteral("%1").arg(maximumSequence + 1, 3, 10, QLatin1Char('0'));
}

// 优先沿用格式合法且未被占用的单号；否则重新取当日下一个流水号。
// 失败时返回空串并写入错误信息（含当日流水号用尽）。
QString resolveInspectionNumber(QSqlDatabase database,
                                const QString &preferredNumber,
                                const QDate &date,
                                QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    const QString preferred = preferredNumber.trimmed();
    if (isInspectionNumberForDate(preferred, date)) {
        bool used = false;
        QString error;
        if (!inspectionNumberUsed(database, preferred, &used, &error)) {
            if (errorMessage) *errorMessage = error;
            return {};
        }
        if (!used) return preferred;
    }
    return nextInspectionNumber(database, date, errorMessage);
}
}

InspectionDialog::InspectionDialog(QSqlDatabase database,
                                   QList<StockMovementRequest> lines,
                                   InboundInspectionRequest inspection,
                                   QWidget *parent)
    : QDialog(parent),
      m_database(std::move(database)),
      m_lines(std::move(lines)),
      m_inspection(std::move(inspection))
{
    setWindowTitle(QStringLiteral("在线填写送检单"));
    setModal(true);
    resize(1050, 720);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("入库送检单"), this);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;"));
    root->addWidget(title);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    const QDate initialInspectionDate = m_inspection.inspectionDate.isValid()
        ? m_inspection.inspectionDate : QDate::currentDate();
    m_numberEdit = new QLineEdit(this);
    QString numberError;
    // 沿用外部单号的前提是：格式与送检日期匹配，且未被其他送检单占用。
    m_inspection.inspectionNumber = resolveInspectionNumber(
        m_database, m_inspection.inspectionNumber, initialInspectionDate, &numberError);
    m_numberEdit->setText(m_inspection.inspectionNumber);
    m_numberEdit->setReadOnly(true);
    m_numberEdit->setToolTip(QStringLiteral(
        "送检单号按 BMJ-JY-年月日-三位当日流水号自动生成；"
        "单号被占用或日期变化时会自动重新取号"));
    m_dateEdit = new QDateEdit(initialInspectionDate, this);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    const QDate arrivalDate = QDate::fromString(
        m_inspection.templateFields.value(QStringLiteral("arrivalDate")),
        QStringLiteral("yyyy-MM-dd"));
    m_arrivalDateEdit = new QDateEdit(arrivalDate.isValid() ? arrivalDate : QDate::currentDate(), this);
    m_arrivalDateEdit->setCalendarPopup(true);
    m_arrivalDateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_inspectorEdit = new QLineEdit(m_inspection.inspectorName, this);
    m_entrustedEdit = new QLineEdit(
        m_inspection.templateFields.value(QStringLiteral("entrustedBy"),
                                          m_inspection.inspectorName), this);
    m_notificationDepartmentEdit = new QLineEdit(
        m_inspection.templateFields.value(QStringLiteral("notificationDepartment"),
                                          QStringLiteral("检验部")), this);
    m_purchaseOrderEdit = new QLineEdit(
        m_inspection.templateFields.value(QStringLiteral("purchaseOrderNumber")), this);
    m_supplierCombo = new QComboBox(this);
    m_supplierCombo->setMinimumWidth(200);
    ComboBoxSearch::enableContainsSearch(m_supplierCombo,
                                         QStringLiteral("选择或输入供应商"));
    populateSupplierCombo();
    m_resultCombo = new QComboBox(this);
    m_resultCombo->addItem(QStringLiteral("待检验"), QStringLiteral("PENDING"));
    m_resultCombo->addItem(QStringLiteral("合格"), QStringLiteral("QUALIFIED"));
    m_resultCombo->addItem(QStringLiteral("不合格"), QStringLiteral("UNQUALIFIED"));
    const int resultIndex = m_resultCombo->findData(m_inspection.result);
    m_resultCombo->setCurrentIndex(resultIndex >= 0 ? resultIndex : 0);
    m_conclusionEdit = new QTextEdit(this);
    m_conclusionEdit->setPlainText(m_inspection.conclusion);
    m_conclusionEdit->setMaximumHeight(85);
    m_conclusionEdit->setPlaceholderText(QStringLiteral("填写检验项目、检验情况和结论"));

    form->addRow(QStringLiteral("送检单号 *"), m_numberEdit);
    form->addRow(QStringLiteral("送检日期 *"), m_dateEdit);
    form->addRow(QStringLiteral("到货日期 *"), m_arrivalDateEdit);
    form->addRow(QStringLiteral("委托人员 *"), m_entrustedEdit);
    form->addRow(QStringLiteral("通知单位 *"), m_notificationDepartmentEdit);
    form->addRow(QStringLiteral("采购单号"), m_purchaseOrderEdit);
    form->addRow(QStringLiteral("供应商"), m_supplierCombo);
    form->addRow(QStringLiteral("检验员 *"), m_inspectorEdit);
    form->addRow(QStringLiteral("检验结果 *"), m_resultCombo);
    form->addRow(QStringLiteral("检验说明"), m_conclusionEdit);
    root->addLayout(form);

    auto *attachmentRow = new QHBoxLayout;
    auto *attachmentButton = new QPushButton(QStringLiteral("上传检验附件"), this);
    m_attachmentLabel = new QLabel(this);
    m_attachmentLabel->setText(m_inspection.attachmentFileName.trimmed().isEmpty()
                                   ? QStringLiteral("尚未上传附件（检验合格后必须上传）")
                                   : QStringLiteral("已选择：%1").arg(m_inspection.attachmentFileName));
    attachmentRow->addWidget(new QLabel(QStringLiteral("检验附件"), this));
    attachmentRow->addWidget(attachmentButton);
    attachmentRow->addWidget(m_attachmentLabel, 1);
    root->addLayout(attachmentRow);

    auto *lineTitle = new QLabel(QStringLiteral("送检产品明细"), this);
    lineTitle->setStyleSheet(QStringLiteral("font-weight:600;"));
    root->addWidget(lineTitle);
    m_lineTable = new QTableWidget(0, 7, this);
    m_lineTable->setHorizontalHeaderLabels({
        QStringLiteral("物料编码"), QStringLiteral("产品名称"), QStringLiteral("规格型号"),
        QStringLiteral("送检数量"), QStringLiteral("批次"), QStringLiteral("入库仓库"),
        QStringLiteral("入库库位")});
    m_lineTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_lineTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_lineTable->verticalHeader()->hide();
    m_lineTable->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(m_lineTable, 1);
    populateLines();

    auto *buttons = new QHBoxLayout;
    auto *printButton = new QPushButton(QStringLiteral("打开模板预览/打印"), this);
    auto *saveButton = new QPushButton(QStringLiteral("保存并返回入库单"), this);
    saveButton->setProperty("primary", true);
    auto *cancelButton = new QPushButton(QStringLiteral("取消"), this);
    buttons->addWidget(printButton);
    buttons->addStretch();
    buttons->addWidget(cancelButton);
    buttons->addWidget(saveButton);
    root->addLayout(buttons);

    connect(attachmentButton, &QPushButton::clicked, this, &InspectionDialog::chooseAttachment);
    connect(printButton, &QPushButton::clicked, this, &InspectionDialog::printInspectionForm);
    connect(saveButton, &QPushButton::clicked, this, &InspectionDialog::acceptInspection);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_dateEdit, &QDateEdit::dateChanged, this, [this](const QDate &date) {
        QString error;
        const QString number = nextInspectionNumber(m_database, date, &error);
        m_numberEdit->setText(number);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("生成送检单号失败"), error);
        }
    });
    if (!numberError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("生成送检单号失败"), numberError);
    }
}

void InspectionDialog::populateLines()
{
    m_lineTable->setRowCount(0);
    QSqlQuery material(m_database);
    material.prepare(QStringLiteral(
        "SELECT m.code,m.name,m.specification,COALESCE(w.name,''),COALESCE(l.code,'') "
        "FROM materials m LEFT JOIN warehouses w ON w.id=? "
        "LEFT JOIN locations l ON l.id=? WHERE m.id=?"));
    for (const StockMovementRequest &line : std::as_const(m_lines)) {
        material.bindValue(0, line.warehouseId);
        material.bindValue(1, line.locationId);
        material.bindValue(2, line.materialId);
        if (!material.exec() || !material.next()) continue;
        const int row = m_lineTable->rowCount();
        m_lineTable->insertRow(row);
        const QStringList values = {
            material.value(0).toString(), material.value(1).toString(),
            material.value(2).toString(), displayQuantity(line.quantity), line.batchNo,
            material.value(3).toString(), material.value(4).toString()};
        for (int column = 0; column < values.size(); ++column) {
            m_lineTable->setItem(row, column, new QTableWidgetItem(values.at(column)));
        }
    }
    m_lineTable->resizeColumnsToContents();
    m_lineTable->setColumnWidth(1, qMax(m_lineTable->columnWidth(1), 160));
    m_lineTable->setColumnWidth(2, qMax(m_lineTable->columnWidth(2), 160));
}

// 供应商历史来自三处：入库批次、业务单据和物料品牌（本系统把品牌当供应商使用）。
void InspectionDialog::populateSupplierCombo()
{
    const QString current = m_inspection.templateFields.value(QStringLiteral("supplier")).trimmed();
    m_supplierCombo->clear();
    // 供应商目前不是必填项，保留空选项。
    m_supplierCombo->addItem(QString());

    QStringList suppliers;
    QSet<QString> seen;
    const auto collect = [&suppliers, &seen](QSqlQuery &query) {
        while (query.next()) {
            const QString name = query.value(0).toString().trimmed();
            if (name.isEmpty()) continue;
            const QString key = name.toCaseFolded();
            if (seen.contains(key)) continue;
            seen.insert(key);
            suppliers.append(name);
        }
    };
    const QStringList historyQueries = {
        QStringLiteral("SELECT supplier FROM batches WHERE trim(supplier)<>''"),
        QStringLiteral("SELECT supplier FROM business_documents WHERE trim(supplier)<>''"),
        QStringLiteral("SELECT brand FROM materials WHERE trim(brand)<>''")
    };
    for (const QString &statement : historyQueries) {
        QSqlQuery query(m_database);
        // 旧数据库缺少列时只跳过该项历史，不影响手工输入。
        if (!query.exec(statement)) continue;
        collect(query);
    }
    std::stable_sort(suppliers.begin(), suppliers.end(),
                     [](const QString &left, const QString &right) {
                         return QString::compare(left, right, Qt::CaseInsensitive) < 0;
                     });
    for (const QString &name : std::as_const(suppliers)) m_supplierCombo->addItem(name);

    // 已填写的供应商即使不在历史里也要保留，并且仍然允许直接输入新值。
    const int index = current.isEmpty()
        ? 0
        : m_supplierCombo->findText(current, Qt::MatchFixedString);
    if (index >= 0) m_supplierCombo->setCurrentIndex(index);
    else m_supplierCombo->setEditText(current);
}

QString InspectionDialog::supplierText() const
{
    return m_supplierCombo->currentText().trimmed();
}

void InspectionDialog::chooseAttachment()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择检验合格附件"), {},
        QStringLiteral("常用文件 (*.pdf *.jpg *.jpeg *.png *.doc *.docx *.xls *.xlsx);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("附件读取失败"), file.errorString());
        return;
    }
    if (file.size() > MaximumAttachmentBytes) {
        QMessageBox::warning(this, QStringLiteral("附件过大"),
                             QStringLiteral("单个送检附件不能超过50 MB。"));
        return;
    }
    const QByteArray data = file.readAll();
    if (data.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("附件无效"), QStringLiteral("不能上传空文件。"));
        return;
    }
    m_inspection.attachmentFileName = QFileInfo(path).fileName();
    m_inspection.attachmentMimeType = QMimeDatabase().mimeTypeForFile(path).name();
    m_inspection.attachmentData = data;
    m_attachmentLabel->setText(QStringLiteral("已选择：%1（%2 KB）")
                                   .arg(m_inspection.attachmentFileName)
                                   .arg((data.size() + 1023) / 1024));
}

QString InspectionDialog::resultText() const
{
    return m_resultCombo->currentText();
}

OfficeTemplateDocument InspectionDialog::templateDocument() const
{
    OfficeTemplateDocument document;
    document.kind = OfficeFormKind::Inspection;
    document.documentNumber = m_numberEdit->text().trimmed();
    document.documentDate = m_dateEdit->date();
    document.fields.insert(QStringLiteral("arrivalDate"),
                           m_arrivalDateEdit->date().toString(QStringLiteral("yyyy-MM-dd")));
    document.fields.insert(QStringLiteral("inspectionDate"),
                           m_dateEdit->date().toString(QStringLiteral("yyyy-MM-dd")));
    document.fields.insert(QStringLiteral("entrustedBy"), m_entrustedEdit->text().trimmed());
    document.fields.insert(QStringLiteral("notificationDepartment"),
                           m_notificationDepartmentEdit->text().trimmed());
    document.fields.insert(QStringLiteral("purchaseOrderNumber"),
                           m_purchaseOrderEdit->text().trimmed());
    document.fields.insert(QStringLiteral("supplier"), supplierText());
    document.fields.insert(QStringLiteral("inspectionResult"), resultText());
    document.fields.insert(QStringLiteral("conclusion"),
                           m_conclusionEdit->toPlainText().trimmed());
    QString error;
    document.lines = OfficeTemplateService::materialLines(m_database, m_lines, &error);
    for (OfficeTemplateLine &line : document.lines) {
        line.orderNumber = m_purchaseOrderEdit->text().trimmed();
        line.supplier = supplierText();
    }
    return document;
}

void InspectionDialog::printInspectionForm()
{
    if (m_numberEdit->text().trimmed().isEmpty()
        || m_inspectorEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("送检单不完整"),
                             QStringLiteral("请先填写送检单号和检验员。"));
        return;
    }
    if (m_lineTable->rowCount() == 0) {
        QMessageBox::warning(this, QStringLiteral("没有送检产品"),
                             QStringLiteral("请先返回入库单填写产品明细，再重新打开送检单打印。"));
        return;
    }
    OfficeTemplateService::openPreview(templateDocument(), this);
}

InboundInspectionRequest InspectionDialog::inspection() const
{
    InboundInspectionRequest result = m_inspection;
    result.required = true;
    result.inspectionNumber = m_numberEdit->text().trimmed();
    result.inspectionDate = m_dateEdit->date();
    result.inspectorName = m_inspectorEdit->text().trimmed();
    result.result = m_resultCombo->currentData().toString();
    result.conclusion = m_conclusionEdit->toPlainText().trimmed();
    result.templateFields.insert(QStringLiteral("arrivalDate"),
                                 m_arrivalDateEdit->date().toString(QStringLiteral("yyyy-MM-dd")));
    result.templateFields.insert(QStringLiteral("entrustedBy"), m_entrustedEdit->text().trimmed());
    result.templateFields.insert(QStringLiteral("notificationDepartment"),
                                 m_notificationDepartmentEdit->text().trimmed());
    result.templateFields.insert(QStringLiteral("purchaseOrderNumber"),
                                 m_purchaseOrderEdit->text().trimmed());
    result.templateFields.insert(QStringLiteral("supplier"), supplierText());
    return result;
}

void InspectionDialog::acceptInspection()
{
    InboundInspectionRequest value = inspection();
    if (!value.inspectionDate.isValid()
        || value.inspectorName.isEmpty()
        || value.templateFields.value(QStringLiteral("entrustedBy")).isEmpty()
        || value.templateFields.value(QStringLiteral("notificationDepartment")).isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("送检单不完整"),
                             QStringLiteral("送检日期、委托人员、通知单位和检验员不能为空。"));
        return;
    }
    // 窗口打开后单号可能已被其他送检单占用：提交前重新取号，避免撞唯一约束后才失败。
    QString numberError;
    const QString availableNumber = resolveInspectionNumber(
        m_database, value.inspectionNumber, value.inspectionDate, &numberError);
    if (availableNumber.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("生成送检单号失败"),
                             numberError.isEmpty()
                                 ? QStringLiteral("无法生成可用的送检单号，请稍后重试。")
                                 : numberError);
        return;
    }
    if (availableNumber != value.inspectionNumber) {
        value.inspectionNumber = availableNumber;
        m_inspection.inspectionNumber = availableNumber;
        m_numberEdit->setText(availableNumber);
    }
    if (value.result == QStringLiteral("QUALIFIED")
        && (value.attachmentFileName.isEmpty() || value.attachmentData.isEmpty())) {
        QMessageBox::warning(this, QStringLiteral("缺少合格附件"),
                             QStringLiteral("检验合格后必须上传检验报告或合格证明附件。"));
        return;
    }

    // 单号已最终确定：业务单据尚未生效也先把送检单正本按单号归档，
    // 生成失败时保持窗口打开，避免用户以为已经保存成功。
    OfficeTemplateDocument document = templateDocument();
    document.documentNumber = value.inspectionNumber;
    QString saveError;
    if (!OfficeTemplateService::saveToDocumentsArchive(document, &m_archivedPath, &saveError)) {
        QMessageBox::warning(this, QStringLiteral("送检单保存失败"),
                             saveError.isEmpty()
                                 ? QStringLiteral("无法保存送检单，请稍后重试。")
                                 : saveError);
        return;
    }
    m_inspection = inspection();
    accept();
}
