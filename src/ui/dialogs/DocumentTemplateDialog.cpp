#include "ui/dialogs/DocumentTemplateDialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <utility>

namespace {
struct FieldDefinition
{
    QString key;
    QString label;
    QString defaultValue;
    bool required = false;
};

QList<FieldDefinition> fieldDefinitions(const OfficeTemplateDocument &document)
{
    switch (document.kind) {
    case OfficeFormKind::RawMaterialInbound:
    case OfficeFormKind::FinishedGoodsInbound:
        return {};
    case OfficeFormKind::ProductionIssue:
        return {
            {QStringLiteral("receivingDepartment"), QStringLiteral("领用部门"),
             QStringLiteral("生产部"), true}
        };
    case OfficeFormKind::StockOutbound:
        return {};
    case OfficeFormKind::DeliveryConfirmation:
        return {};
    case OfficeFormKind::Inspection:
        return {};
    }
    return {};
}

QString quantityText(double value)
{
    return QString::number(value, 'g', 12);
}
}

DocumentTemplateDialog::DocumentTemplateDialog(OfficeTemplateDocument document, QWidget *parent)
    : QDialog(parent), m_document(std::move(document))
{
    const QString title = OfficeTemplateService::formTitle(m_document.kind);
    setWindowTitle(QStringLiteral("在线填写%1").arg(title));
    setModal(true);
    resize(1080, 720);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);

    auto *heading = new QLabel(title, this);
    heading->setAlignment(Qt::AlignCenter);
    heading->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;"));
    root->addWidget(heading);

    auto *description = new QLabel(
        QStringLiteral("以下内容会写入“仓库系统表单模板”中的原始 Excel 版式。正式提交后同时保存到数据库附件和“我的文档\\冰美肌仓库系统表单”分类文件夹，并自动打开正式表单。所有签字、批准和签署日期位置保持空白，打印后手写。"),
        this);
    description->setObjectName(QStringLiteral("mutedText"));
    description->setWordWrap(true);
    root->addWidget(description);

    auto *fixedForm = new QFormLayout;
    fixedForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto *number = new QLabel(m_document.documentNumber.trimmed().isEmpty()
                                  ? QStringLiteral("提交后自动生成正式单号")
                                  : m_document.documentNumber,
                              this);
    number->setObjectName(QStringLiteral("mutedText"));
    fixedForm->addRow(QStringLiteral("业务单号"), number);
    fixedForm->addRow(QStringLiteral("业务日期"),
                      new QLabel(m_document.documentDate.toString(QStringLiteral("yyyy-MM-dd")), this));
    root->addLayout(fixedForm);

    addEditableFields();

    auto *lineTitle = new QLabel(QStringLiteral("模板明细（来源于当前业务页面）"), this);
    lineTitle->setStyleSheet(QStringLiteral("font-weight:600;"));
    root->addWidget(lineTitle);
    m_lineTable = new QTableWidget(0, 8, this);
    m_lineTable->setHorizontalHeaderLabels({
        QStringLiteral("序号"), QStringLiteral("物料编码"), QStringLiteral("名称"),
        QStringLiteral("规格型号"), QStringLiteral("单位"), QStringLiteral("数量"),
        QStringLiteral("批次/SN"), QStringLiteral("备注")});
    m_lineTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_lineTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_lineTable->verticalHeader()->hide();
    m_lineTable->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(m_lineTable, 1);
    populateLines();

    auto *buttons = new QDialogButtonBox(this);
    auto *preview = buttons->addButton(QStringLiteral("打开模板预览"), QDialogButtonBox::ActionRole);
    auto *cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    auto *save = buttons->addButton(QStringLiteral("保存表单并继续"), QDialogButtonBox::AcceptRole);
    save->setProperty("primary", true);
    root->addWidget(buttons);
    connect(preview, &QPushButton::clicked, this, &DocumentTemplateDialog::previewTemplate);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &DocumentTemplateDialog::acceptForm);
}

void DocumentTemplateDialog::addEditableFields()
{
    const QList<FieldDefinition> definitions = fieldDefinitions(m_document);
    if (definitions.isEmpty()) return;
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    for (const FieldDefinition &definition : definitions) {
        auto *edit = new QLineEdit(this);
        edit->setText(m_document.fields.value(definition.key, definition.defaultValue));
        edit->setProperty("required", definition.required);
        m_fieldEdits.insert(definition.key, edit);
        form->addRow(definition.label + (definition.required ? QStringLiteral(" *") : QString()), edit);
    }
    if (auto *root = qobject_cast<QVBoxLayout *>(layout())) root->addLayout(form);
}

void DocumentTemplateDialog::populateLines()
{
    for (const OfficeTemplateLine &line : std::as_const(m_document.lines)) {
        const int row = m_lineTable->rowCount();
        m_lineTable->insertRow(row);
        QString batchAndSerial = line.batchNo;
        if (!line.serialNumbers.isEmpty()) {
            if (!batchAndSerial.isEmpty()) batchAndSerial += QStringLiteral(" / ");
            batchAndSerial += line.serialNumbers;
        }
        const QStringList values = {
            QString::number(row + 1), line.materialCode, line.materialName, line.specification,
            line.unit, quantityText(line.quantity), batchAndSerial, line.notes};
        for (int column = 0; column < values.size(); ++column)
            m_lineTable->setItem(row, column, new QTableWidgetItem(values.at(column)));
    }
    m_lineTable->resizeColumnsToContents();
    m_lineTable->setColumnWidth(2, qMax(150, m_lineTable->columnWidth(2)));
    m_lineTable->setColumnWidth(3, qMax(150, m_lineTable->columnWidth(3)));
}

OfficeTemplateDocument DocumentTemplateDialog::document() const
{
    OfficeTemplateDocument result = m_document;
    for (auto it = m_fieldEdits.cbegin(); it != m_fieldEdits.cend(); ++it)
        result.fields.insert(it.key(), it.value()->text().trimmed());
    return result;
}

void DocumentTemplateDialog::previewTemplate()
{
    OfficeTemplateService::openPreview(document(), this);
}

void DocumentTemplateDialog::acceptForm()
{
    for (auto it = m_fieldEdits.cbegin(); it != m_fieldEdits.cend(); ++it) {
        if (it.value()->property("required").toBool() && it.value()->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("表单资料不完整"),
                                 QStringLiteral("带 * 的模板字段不能为空。"));
            it.value()->setFocus();
            return;
        }
    }
    m_document = document();
    accept();
}
