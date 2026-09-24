#include "ui/pages/FormManagementPage.h"

#include "services/AttachmentService.h"
#include "services/OfficeTemplateService.h"
#include "ui/widgets/TableExcelExport.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QStringList>

#include <utility>

namespace {
constexpr int AttachmentIdRole = Qt::UserRole + 1;
constexpr int DocumentIdRole = Qt::UserRole + 2;
constexpr int BusinessStatusRole = Qt::UserRole + 3;

QString formTitle(const QString &code)
{
    if (code == QStringLiteral("inspection")) return QStringLiteral("材料检验通知单");
    if (code == QStringLiteral("rawInbound")) return QStringLiteral("原材料入库单");
    if (code == QStringLiteral("productionIssue")) return QStringLiteral("领料单");
    if (code == QStringLiteral("finishedInbound")) return QStringLiteral("成品入库单");
    if (code == QStringLiteral("stockOutbound")) return QStringLiteral("出库单");
    if (code == QStringLiteral("deliveryConfirmation")) return QStringLiteral("送货确认单");
    return code.isEmpty() ? QStringLiteral("其他表单") : code;
}

QString businessStatusText(const QString &status)
{
    if (status == QStringLiteral("POSTED")) return QStringLiteral("已生效");
    if (status == QStringLiteral("PARTIALLY_REVERSED")) return QStringLiteral("部分撤销");
    if (status == QStringLiteral("REVERSED")) return QStringLiteral("已撤销");
    if (status == QStringLiteral("DRAFT")) return QStringLiteral("草稿");
    if (status == QStringLiteral("PENDING")) return QStringLiteral("待检验");
    if (status == QStringLiteral("QUALIFIED")) return QStringLiteral("检验合格");
    if (status == QStringLiteral("UNQUALIFIED")) return QStringLiteral("检验不合格");
    if (status == QStringLiteral("USED")) return QStringLiteral("已入库");
    if (status == QStringLiteral("CANCELLED")) return QStringLiteral("已撤销");
    return status;
}

QString generationStatusText(const QString &status)
{
    if (status == QStringLiteral("COMPLETED")) return QStringLiteral("已生成");
    if (status == QStringLiteral("FAILED")) return QStringLiteral("生成失败");
    if (status == QStringLiteral("PENDING")) return QStringLiteral("待生成");
    return status;
}

QString documentNumberFromPayload(const QString &payload, const QString &fallback)
{
    const QJsonDocument json = QJsonDocument::fromJson(payload.toUtf8());
    if (!json.isObject()) return fallback;
    const QString number = json.object().value(QStringLiteral("documentNumber")).toString().trimmed();
    return number.isEmpty() ? fallback : number;
}

bool matchesKeyword(const QString &keyword, const QStringList &values)
{
    if (keyword.isEmpty()) return true;
    for (const QString &value : values) {
        if (value.contains(keyword, Qt::CaseInsensitive)) return true;
    }
    return false;
}
}

FormManagementPage::FormManagementPage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(14);

    auto *filterPanel = new QFrame(this);
    filterPanel->setObjectName(QStringLiteral("panel"));
    auto *filterLayout = new QHBoxLayout(filterPanel);
    filterLayout->setContentsMargins(18, 16, 18, 16);
    auto *title = new QLabel(QStringLiteral("全部表单（包含已撤销）"), filterPanel);
    title->setObjectName(QStringLiteral("sectionTitle"));
    m_keywordEdit = new QLineEdit(filterPanel);
    m_keywordEdit->setPlaceholderText(QStringLiteral("检索表单号、业务单号、表单类型、经办人或文件名"));
    m_keywordEdit->setClearButtonEnabled(true);
    m_statusCombo = new QComboBox(filterPanel);
    m_statusCombo->addItem(QStringLiteral("全部状态（含已撤销）"), QStringLiteral("ALL"));
    m_statusCombo->addItem(QStringLiteral("有效表单"), QStringLiteral("ACTIVE"));
    m_statusCombo->addItem(QStringLiteral("已撤销表单"), QStringLiteral("REVERSED"));
    m_statusCombo->addItem(QStringLiteral("未完成表单"), QStringLiteral("INCOMPLETE"));
    auto *searchButton = new QPushButton(QStringLiteral("查询"), filterPanel);
    searchButton->setProperty("primary", true);
    auto *resetButton = new QPushButton(QStringLiteral("重置"), filterPanel);
    filterLayout->addWidget(title);
    filterLayout->addSpacing(12);
    filterLayout->addWidget(m_keywordEdit, 1);
    filterLayout->addWidget(m_statusCombo);
    filterLayout->addWidget(searchButton);
    filterLayout->addWidget(resetButton);
    root->addWidget(filterPanel);

    auto *tablePanel = new QFrame(this);
    tablePanel->setObjectName(QStringLiteral("panel"));
    auto *tableLayout = new QVBoxLayout(tablePanel);
    tableLayout->setContentsMargins(18, 16, 18, 18);
    auto *toolbar = new QHBoxLayout;
    m_summaryLabel = new QLabel(tablePanel);
    m_summaryLabel->setObjectName(QStringLiteral("mutedText"));
    m_openButton = new QPushButton(QStringLiteral("打开表单"), tablePanel);
    m_openButton->setProperty("primary", true);
    m_downloadButton = new QPushButton(QStringLiteral("另存表单"), tablePanel);
    m_retryButton = new QPushButton(QStringLiteral("重新生成未完成表单"), tablePanel);
    auto *fullScreenButton = new QPushButton(QStringLiteral("全屏显示"), tablePanel);
    toolbar->addWidget(m_summaryLabel);
    toolbar->addStretch();
    toolbar->addWidget(m_retryButton);
    toolbar->addWidget(m_openButton);
    toolbar->addWidget(m_downloadButton);
    toolbar->addWidget(fullScreenButton);
    tableLayout->addLayout(toolbar);

    m_table = new QTableWidget(0, 12, tablePanel);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("序号"), QStringLiteral("表单号"), QStringLiteral("表单类型"),
        QStringLiteral("来源业务单号"), QStringLiteral("单据日期"), QStringLiteral("业务状态"),
        QStringLiteral("表单状态"), QStringLiteral("经办人"), QStringLiteral("文件名"),
        QStringLiteral("文件状态"), QStringLiteral("更新时间"), QStringLiteral("说明")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(11, QHeaderView::Stretch);
    m_table->setProperty("excelExportTitle", QStringLiteral("全部表单（含已撤销）"));
    tableLayout->addWidget(m_table, 1);
    root->addWidget(tablePanel, 1);

    connect(searchButton, &QPushButton::clicked, this, &FormManagementPage::refresh);
    connect(resetButton, &QPushButton::clicked, this, &FormManagementPage::resetFilters);
    connect(m_keywordEdit, &QLineEdit::returnPressed, this, &FormManagementPage::refresh);
    connect(m_statusCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { refresh(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &FormManagementPage::updateActions);
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { openSelectedForm(); });
    connect(m_openButton, &QPushButton::clicked, this, &FormManagementPage::openSelectedForm);
    connect(m_downloadButton, &QPushButton::clicked, this, &FormManagementPage::downloadSelectedForm);
    connect(m_retryButton, &QPushButton::clicked, this, &FormManagementPage::retrySelectedForm);
    connect(fullScreenButton, &QPushButton::clicked, this, &FormManagementPage::showFullScreen);
    refresh();
}

void FormManagementPage::refresh()
{
    const qlonglong previousAttachmentId = selectedAttachmentId();
    const qlonglong previousDocumentId = selectedDocumentId();
    m_table->setRowCount(0);
    const QString keyword = m_keywordEdit->text().trimmed();
    const QString filter = m_statusCombo->currentData().toString();
    int reversedCount = 0;
    int incompleteCount = 0;
    int targetRow = -1;

    const auto appendRow = [&](const QString &formNumber, const QString &formType,
                               const QString &sourceNumber, const QString &documentDate,
                               const QString &businessStatus, const QString &formStatus,
                               const QString &handlerName, const QString &fileName,
                               bool attachmentDeleted, qlonglong attachmentId,
                               qlonglong documentId, const QString &updatedAt,
                               const QString &description) {
        const bool reversed = businessStatus == QStringLiteral("REVERSED")
            || businessStatus == QStringLiteral("CANCELLED");
        const bool incomplete = formStatus == QStringLiteral("PENDING")
            || formStatus == QStringLiteral("FAILED");
        if (filter == QStringLiteral("ACTIVE") && reversed) return;
        if (filter == QStringLiteral("REVERSED") && !reversed) return;
        if (filter == QStringLiteral("INCOMPLETE") && !incomplete) return;
        const QString formTypeText = formTitle(formType);
        if (!matchesKeyword(keyword, {formNumber, formTypeText, sourceNumber, documentDate,
                                      businessStatusText(businessStatus), handlerName,
                                      fileName, description})) return;

        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QString fileState = attachmentId <= 0
            ? QStringLiteral("无文件")
            : attachmentDeleted ? (reversed ? QStringLiteral("已撤销归档") : QStringLiteral("已删除"))
                                : QStringLiteral("正常");
        const QStringList values = {
            QString::number(row + 1), formNumber, formTypeText, sourceNumber, documentDate,
            businessStatusText(businessStatus), generationStatusText(formStatus), handlerName,
            fileName, fileState, updatedAt, description};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setData(AttachmentIdRole, attachmentId);
            item->setData(DocumentIdRole, documentId);
            item->setData(BusinessStatusRole, businessStatus);
            if (reversed) item->setBackground(QBrush(QColor(QStringLiteral("#f1f3f5"))));
            else if (formStatus == QStringLiteral("FAILED"))
                item->setBackground(QBrush(QColor(QStringLiteral("#ffe3e3"))));
            else if (formStatus == QStringLiteral("PENDING"))
                item->setBackground(QBrush(QColor(QStringLiteral("#fff3bf"))));
            m_table->setItem(row, column, item);
        }
        if (reversed) ++reversedCount;
        if (incomplete) ++incompleteCount;
        if ((attachmentId > 0 && attachmentId == previousAttachmentId)
            || (attachmentId <= 0 && documentId > 0 && documentId == previousDocumentId)) {
            targetRow = row;
        }
    };

    QSqlQuery forms(m_database);
    forms.prepare(QStringLiteral(
        "SELECT f.document_id,f.form_kind,f.payload,f.status,f.last_error,f.attachment_id,"
        "f.updated_at,d.document_no,d.document_date,d.status,d.handler_name,"
        "COALESCE(a.original_file_name,''),COALESCE(a.is_deleted,0) "
        "FROM document_forms f JOIN business_documents d ON d.id=f.document_id "
        "LEFT JOIN attachments a ON a.id=f.attachment_id ORDER BY f.updated_at DESC,f.id DESC"));
    if (!forms.exec()) {
        m_summaryLabel->setText(QStringLiteral("表单查询失败：%1").arg(forms.lastError().text()));
        updateActions();
        return;
    }
    while (forms.next()) {
        const QString sourceNumber = forms.value(7).toString();
        appendRow(documentNumberFromPayload(forms.value(2).toString(), sourceNumber),
                  forms.value(1).toString(), sourceNumber, forms.value(8).toString(),
                  forms.value(9).toString(), forms.value(3).toString(),
                  forms.value(10).toString(), forms.value(11).toString(),
                  forms.value(12).toBool(), forms.value(5).toLongLong(),
                  forms.value(0).toLongLong(), forms.value(6).toString(),
                  forms.value(4).toString());
    }

    QSqlQuery inspections(m_database);
    inspections.prepare(QStringLiteral(
        "SELECT n.inspection_no,n.status,n.notification_date,n.entrusted_by,"
        "n.template_file_attachment_id,n.updated_at,COALESCE(a.original_file_name,''),"
        "COALESCE(a.is_deleted,0),n.linked_document_id,n.conclusion "
        "FROM inspection_notices n "
        "LEFT JOIN attachments a ON a.id=n.template_file_attachment_id "
        "WHERE (n.template_file_attachment_id IS NOT NULL OR n.template_payload<>'' OR n.archive_path<>'') "
        "AND (n.template_file_attachment_id IS NULL OR NOT EXISTS("
        "SELECT 1 FROM document_forms f WHERE f.attachment_id=n.template_file_attachment_id)) "
        "ORDER BY n.updated_at DESC,n.id DESC"));
    if (inspections.exec()) {
        while (inspections.next()) {
            const QString noticeStatus = inspections.value(1).toString();
            appendRow(inspections.value(0).toString(), QStringLiteral("inspection"),
                      inspections.value(0).toString(), inspections.value(2).toString(),
                      noticeStatus, inspections.value(4).toLongLong() > 0
                          ? QStringLiteral("COMPLETED") : QStringLiteral("PENDING"),
                      inspections.value(3).toString(), inspections.value(6).toString(),
                      inspections.value(7).toBool(), inspections.value(4).toLongLong(),
                      0, inspections.value(5).toString(),
                      inspections.value(9).toString());
        }
    }

    m_summaryLabel->setText(QStringLiteral("共 %1 份表单，其中已撤销 %2 份、未完成 %3 份")
                                .arg(m_table->rowCount()).arg(reversedCount).arg(incompleteCount));
    if (targetRow < 0 && m_table->rowCount() > 0) targetRow = 0;
    if (targetRow >= 0) m_table->selectRow(targetRow);
    updateActions();
}

void FormManagementPage::resetFilters()
{
    m_keywordEdit->clear();
    m_statusCombo->setCurrentIndex(0);
    refresh();
}

qlonglong FormManagementPage::selectedAttachmentId() const
{
    const int row = m_table->currentRow();
    return row < 0 || !m_table->item(row, 0)
        ? 0 : m_table->item(row, 0)->data(AttachmentIdRole).toLongLong();
}

qlonglong FormManagementPage::selectedDocumentId() const
{
    const int row = m_table->currentRow();
    return row < 0 || !m_table->item(row, 0)
        ? 0 : m_table->item(row, 0)->data(DocumentIdRole).toLongLong();
}

QString FormManagementPage::selectedBusinessStatus() const
{
    const int row = m_table->currentRow();
    return row < 0 || !m_table->item(row, 0)
        ? QString() : m_table->item(row, 0)->data(BusinessStatusRole).toString();
}

bool FormManagementPage::saveAttachmentToTemporaryFile(qlonglong attachmentId, QString *path,
                                                        QString *errorMessage) const
{
    AttachmentService service(m_database, m_session.userId);
    AttachmentPayload payload;
    if (!service.loadAttachment(attachmentId, &payload, errorMessage)) return false;
    QString safeName = payload.fileName.trimmed();
    safeName.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    if (safeName.isEmpty()) safeName = QStringLiteral("表单文件");
    const QString directory = QDir::cleanPath(
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/IceBeautyWms/forms"));
    if (!QDir().mkpath(directory)) {
        if (errorMessage) *errorMessage = QStringLiteral("无法创建表单临时目录。");
        return false;
    }
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(payload.data, QCryptographicHash::Sha256).toHex().left(12));
    const QString outputPath = QDir(directory).filePath(
        QStringLiteral("%1-%2-%3").arg(payload.id).arg(digest).arg(safeName));
    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(payload.data) != payload.data.size()) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    file.close();
    if (path) *path = outputPath;
    return true;
}

void FormManagementPage::openSelectedForm()
{
    const qlonglong attachmentId = selectedAttachmentId();
    if (attachmentId <= 0) {
        QMessageBox::information(this, QStringLiteral("没有表单文件"),
                                 QStringLiteral("该表单尚未生成文件，可选择后点击“重新生成未完成表单”。"));
        return;
    }
    QString path;
    QString error;
    if (!saveAttachmentToTemporaryFile(attachmentId, &path, &error)) {
        QMessageBox::warning(this, QStringLiteral("打开失败"), error);
        return;
    }
    OfficeTemplateService::openFileWithApplicationChoice(path, this);
}

void FormManagementPage::downloadSelectedForm()
{
    const qlonglong attachmentId = selectedAttachmentId();
    if (attachmentId <= 0) return;
    AttachmentService service(m_database, m_session.userId);
    AttachmentPayload payload;
    QString error;
    if (!service.loadAttachment(attachmentId, &payload, &error)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), error);
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("另存表单"), payload.fileName);
    if (path.isEmpty()) return;
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || output.write(payload.data) != payload.data.size()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), output.errorString());
        return;
    }
    QMessageBox::information(this, QStringLiteral("保存完成"),
                             QStringLiteral("表单已保存到：\n%1")
                                 .arg(QDir::toNativeSeparators(path)));
}

void FormManagementPage::retrySelectedForm()
{
    const qlonglong documentId = selectedDocumentId();
    if (documentId <= 0 || selectedBusinessStatus() == QStringLiteral("REVERSED")) return;
    QStringList completed;
    QStringList errors;
    OfficeTemplateService::retryIncompleteDocumentForms(
        m_database, m_session.userId, documentId, &completed, &errors, false);
    refresh();
    if (errors.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("重新生成完成"),
            completed.isEmpty() ? QStringLiteral("该单据没有需要重新生成的表单。")
                                : completed.join(QLatin1Char('\n')));
    } else {
        QMessageBox::warning(this, QStringLiteral("部分表单未完成"), errors.join(QLatin1Char('\n')));
    }
}

void FormManagementPage::showFullScreen()
{
    TableExcelExport::fullScreenTable(
        m_table, QStringLiteral("全部表单（包含已撤销）"), this, [this] { refresh(); },
        {{QStringLiteral("打开表单"), [this] { openSelectedForm(); }},
         {QStringLiteral("另存表单"), [this] { downloadSelectedForm(); }},
         {QStringLiteral("重新生成未完成表单"), [this] { retrySelectedForm(); }}});
}

void FormManagementPage::updateActions()
{
    const bool selected = m_table->currentRow() >= 0;
    const bool hasFile = selectedAttachmentId() > 0;
    m_openButton->setEnabled(hasFile);
    m_downloadButton->setEnabled(hasFile);
    m_retryButton->setEnabled(
        selected && m_session.canManageAttachments() && selectedDocumentId() > 0
        && selectedBusinessStatus() != QStringLiteral("REVERSED")
        && OfficeTemplateService::hasIncompleteDocumentForms(m_database, selectedDocumentId()));
}
