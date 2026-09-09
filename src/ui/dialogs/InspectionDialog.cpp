#include "ui/dialogs/InspectionDialog.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
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
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTextDocument>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

namespace {
constexpr qint64 MaximumAttachmentBytes = 50LL * 1024LL * 1024LL;

QString displayQuantity(double value)
{
    return QString::number(value, 'g', 12);
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
    m_numberEdit = new QLineEdit(this);
    if (m_inspection.inspectionNumber.trimmed().isEmpty()) {
        m_inspection.inspectionNumber = QStringLiteral("SJ%1-%2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss")),
                 QUuid::createUuid().toString(QUuid::WithoutBraces).left(6).toUpper());
    }
    m_numberEdit->setText(m_inspection.inspectionNumber);
    m_dateEdit = new QDateEdit(m_inspection.inspectionDate.isValid()
                                   ? m_inspection.inspectionDate : QDate::currentDate(), this);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_inspectorEdit = new QLineEdit(m_inspection.inspectorName, this);
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
    auto *printButton = new QPushButton(QStringLiteral("打印送检单"), this);
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

QString InspectionDialog::inspectionHtml() const
{
    QString rows;
    for (int row = 0; row < m_lineTable->rowCount(); ++row) {
        rows += QStringLiteral("<tr>");
        for (int column = 0; column < m_lineTable->columnCount(); ++column) {
            const QTableWidgetItem *item = m_lineTable->item(row, column);
            rows += QStringLiteral("<td>%1</td>")
                        .arg(item ? item->text().toHtmlEscaped() : QString());
        }
        rows += QStringLiteral("</tr>");
    }
    return QStringLiteral(
        "<html><head><style>body{font-family:'Microsoft YaHei';font-size:10pt;}"
        "h1{text-align:center;font-size:18pt;}table{border-collapse:collapse;width:100%;}"
        "th,td{border:1px solid #333;padding:6px;}th{background:#eee;}"
        ".meta td{border:0;padding:5px;}</style></head><body>"
        "<h1>入库送检单</h1><table class='meta'>"
        "<tr><td>送检单号：%1</td><td>送检日期：%2</td></tr>"
        "<tr><td>检验员：%3</td><td>检验结果：%4</td></tr></table>"
        "<table><tr><th>物料编码</th><th>产品名称</th><th>规格型号</th>"
        "<th>送检数量</th><th>批次</th><th>入库仓库</th><th>入库库位</th></tr>%5</table>"
        "<p><b>检验说明：</b>%6</p><p><b>附件：</b>%7</p>"
        "<p style='margin-top:35px'>检验签字：________________　日期：________________</p>"
        "</body></html>")
        .arg(m_numberEdit->text().trimmed().toHtmlEscaped(),
             m_dateEdit->date().toString(QStringLiteral("yyyy-MM-dd")),
             m_inspectorEdit->text().trimmed().toHtmlEscaped(), resultText().toHtmlEscaped(),
             rows, m_conclusionEdit->toPlainText().trimmed().toHtmlEscaped().replace('\n', "<br>"),
             m_inspection.attachmentFileName.trimmed().isEmpty()
                 ? QStringLiteral("未上传") : m_inspection.attachmentFileName.toHtmlEscaped());
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
    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog printDialog(&printer, this);
    printDialog.setWindowTitle(QStringLiteral("打印送检单"));
    if (printDialog.exec() != QDialog::Accepted) return;
    QTextDocument document;
    document.setHtml(inspectionHtml());
    document.print(&printer);
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
    return result;
}

void InspectionDialog::acceptInspection()
{
    const InboundInspectionRequest value = inspection();
    if (value.inspectionNumber.isEmpty() || !value.inspectionDate.isValid()
        || value.inspectorName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("送检单不完整"),
                             QStringLiteral("送检单号、送检日期和检验员不能为空。"));
        return;
    }
    if (value.result == QStringLiteral("QUALIFIED")
        && (value.attachmentFileName.isEmpty() || value.attachmentData.isEmpty())) {
        QMessageBox::warning(this, QStringLiteral("缺少合格附件"),
                             QStringLiteral("检验合格后必须上传检验报告或合格证明附件。"));
        return;
    }
    accept();
}
