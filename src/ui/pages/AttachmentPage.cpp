#include "ui/pages/AttachmentPage.h"

#include "import/LegacyInventoryImporter.h"
#include "services/AttachmentService.h"
#include "services/OfficeTemplateService.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPushButton>
#include <QPixmap>
#include <QQuickWidget>
#include <QQmlContext>
#include <QScrollArea>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTabWidget>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

namespace {
constexpr int IdRole = Qt::UserRole + 1;
constexpr int DeletedRole = Qt::UserRole + 2;
}

AttachmentPage::AttachmentPage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *documentPanel = new QFrame(this);
    documentPanel->setObjectName(QStringLiteral("panel"));
    auto *documentLayout = new QVBoxLayout(documentPanel);
    documentLayout->addWidget(new QLabel(QStringLiteral("选择业务单据"), documentPanel));
    auto *searchRow = new QHBoxLayout;
    m_keywordEdit = new QLineEdit(documentPanel);
    m_keywordEdit->setPlaceholderText(QStringLiteral("单据号、业务类型、经办人或备注"));
    auto *search = new QPushButton(QStringLiteral("查询"), documentPanel);
    searchRow->addWidget(m_keywordEdit, 1);
    searchRow->addWidget(search);
    documentLayout->addLayout(searchRow);
    m_documentTable = new QTableWidget(0, 5, documentPanel);
    m_documentTable->setProperty("businessDocumentTable", true);
    m_documentTable->setHorizontalHeaderLabels({QStringLiteral("单据号"), QStringLiteral("日期"),
                                                QStringLiteral("类型"), QStringLiteral("状态"),
                                                QStringLiteral("经办人")});
    m_documentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_documentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_documentTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_documentTable->verticalHeader()->setVisible(false);
    m_documentTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_documentTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    documentLayout->addWidget(m_documentTable);
    root->addWidget(documentPanel, 1);

    auto *attachmentPanel = new QFrame(this);
    attachmentPanel->setObjectName(QStringLiteral("panel"));
    auto *attachmentLayout = new QVBoxLayout(attachmentPanel);
    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(new QLabel(QStringLiteral("所选单据附件"), attachmentPanel));
    toolbar->addStretch();
    m_showDeleted = new QCheckBox(QStringLiteral("显示已删除"), attachmentPanel);
    m_retryFormsButton = new QPushButton(QStringLiteral("重新生成未完成表单"), attachmentPanel);
    m_uploadButton = new QPushButton(QStringLiteral("上传附件"), attachmentPanel);
    m_uploadButton->setProperty("primary", true);
    m_downloadButton = new QPushButton(QStringLiteral("下载"), attachmentPanel);
    m_openButton = new QPushButton(QStringLiteral("预览/打开"), attachmentPanel);
    m_deleteButton = new QPushButton(QStringLiteral("删除"), attachmentPanel);
    m_deleteButton->setProperty("danger", true);
    toolbar->addWidget(m_showDeleted);
    toolbar->addWidget(m_retryFormsButton);
    toolbar->addWidget(m_uploadButton);
    toolbar->addWidget(m_openButton);
    toolbar->addWidget(m_downloadButton);
    toolbar->addWidget(m_deleteButton);
    attachmentLayout->addLayout(toolbar);
    m_attachmentTable = new QTableWidget(0, 7, attachmentPanel);
    m_attachmentTable->setHorizontalHeaderLabels({QStringLiteral("文件名"), QStringLiteral("类型"),
                                                  QStringLiteral("大小"), QStringLiteral("SHA-256"),
                                                  QStringLiteral("上传人"), QStringLiteral("上传时间"),
                                                  QStringLiteral("状态")});
    m_attachmentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_attachmentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_attachmentTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_attachmentTable->verticalHeader()->setVisible(false);
    m_attachmentTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_attachmentTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_attachmentTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    attachmentLayout->addWidget(m_attachmentTable);
    root->addWidget(attachmentPanel, 1);

    connect(search, &QPushButton::clicked, this, &AttachmentPage::loadDocuments);
    connect(m_keywordEdit, &QLineEdit::returnPressed, this, &AttachmentPage::loadDocuments);
    connect(m_documentTable, &QTableWidget::itemSelectionChanged,
            this, &AttachmentPage::loadAttachments);
    connect(m_attachmentTable, &QTableWidget::itemSelectionChanged,
            this, &AttachmentPage::updateActions);
    connect(m_showDeleted, &QCheckBox::toggled, this, &AttachmentPage::loadAttachments);
    connect(m_retryFormsButton, &QPushButton::clicked, this, &AttachmentPage::retryIncompleteForms);
    connect(m_uploadButton, &QPushButton::clicked, this, &AttachmentPage::upload);
    connect(m_downloadButton, &QPushButton::clicked, this, &AttachmentPage::download);
    connect(m_openButton, &QPushButton::clicked, this, &AttachmentPage::openAttachment);
    connect(m_attachmentTable, &QTableWidget::doubleClicked, this, &AttachmentPage::openAttachment);
    connect(m_deleteButton, &QPushButton::clicked, this, &AttachmentPage::deleteOrRestore);
    refresh();
}

qlonglong AttachmentPage::selectedDocumentId() const
{
    const int row = m_documentTable->currentRow();
    return row < 0 ? 0 : m_documentTable->item(row, 0)->data(IdRole).toLongLong();
}

qlonglong AttachmentPage::selectedAttachmentId() const
{
    const int row = m_attachmentTable->currentRow();
    return row < 0 ? 0 : m_attachmentTable->item(row, 0)->data(IdRole).toLongLong();
}

void AttachmentPage::refresh()
{
    loadDocuments();
}

void AttachmentPage::loadDocuments()
{
    const qlonglong previous = selectedDocumentId();
    m_documentTable->setRowCount(0);
    const QString keyword = QStringLiteral("%%1%").arg(m_keywordEdit->text().trimmed());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id,document_no,document_date,document_type,status,handler_name "
        "FROM business_documents WHERE document_no LIKE ? OR document_type LIKE ? "
        "OR handler_name LIKE ? OR notes LIKE ? ORDER BY id DESC LIMIT 300"));
    for (int i = 0; i < 4; ++i) query.addBindValue(keyword);
    query.exec();
    int targetRow = -1;
    while (query.next()) {
        const int row = m_documentTable->rowCount();
        m_documentTable->insertRow(row);
        auto *number = new QTableWidgetItem(query.value(1).toString());
        number->setData(IdRole, query.value(0));
        number->setData(Qt::UserRole, query.value(0));
        m_documentTable->setItem(row, 0, number);
        for (int column = 1; column < 5; ++column)
            m_documentTable->setItem(row, column,
                                     new QTableWidgetItem(query.value(column + 1).toString()));
        if (query.value(0).toLongLong() == previous) targetRow = row;
    }
    if (targetRow < 0 && m_documentTable->rowCount() > 0) targetRow = 0;
    if (targetRow >= 0) m_documentTable->selectRow(targetRow);
    else loadAttachments();
}

void AttachmentPage::loadAttachments()
{
    m_attachmentTable->setRowCount(0);
    const qlonglong documentId = selectedDocumentId();
    if (documentId <= 0) {
        updateActions();
        return;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT a.id,a.original_file_name,a.mime_type,a.file_size,a.sha256,u.display_name,"
        "a.uploaded_at,a.is_deleted FROM attachments a JOIN users u ON u.id=a.uploaded_by "
        "WHERE a.business_type='business_document' AND a.business_id=? AND (?=1 OR a.is_deleted=0) "
        "ORDER BY a.id DESC"));
    query.addBindValue(documentId);
    query.addBindValue(m_showDeleted->isChecked());
    query.exec();
    while (query.next()) {
        const int row = m_attachmentTable->rowCount();
        m_attachmentTable->insertRow(row);
        auto *name = new QTableWidgetItem(query.value(1).toString());
        name->setData(IdRole, query.value(0));
        name->setData(DeletedRole, query.value(7));
        m_attachmentTable->setItem(row, 0, name);
        m_attachmentTable->setItem(row, 1, new QTableWidgetItem(query.value(2).toString()));
        m_attachmentTable->setItem(row, 2, new QTableWidgetItem(
            QStringLiteral("%1 KB").arg(query.value(3).toLongLong() / 1024.0, 0, 'f', 1)));
        m_attachmentTable->setItem(row, 3, new QTableWidgetItem(query.value(4).toString()));
        m_attachmentTable->setItem(row, 4, new QTableWidgetItem(query.value(5).toString()));
        m_attachmentTable->setItem(row, 5, new QTableWidgetItem(query.value(6).toString()));
        m_attachmentTable->setItem(row, 6, new QTableWidgetItem(query.value(7).toBool()
                                                                   ? QStringLiteral("已删除")
                                                                   : QStringLiteral("正常")));
    }
    if (m_attachmentTable->rowCount() > 0) m_attachmentTable->selectRow(0);
    updateActions();
}

void AttachmentPage::updateActions()
{
    const bool canManage = m_session.canManageAttachments();
    const qlonglong documentId = selectedDocumentId();
    m_uploadButton->setEnabled(canManage && documentId > 0);
    m_retryFormsButton->setEnabled(
        canManage && documentId > 0
        && OfficeTemplateService::hasIncompleteDocumentForms(m_database, documentId));
    const int row = m_attachmentTable->currentRow();
    const bool selected = row >= 0;
    m_downloadButton->setEnabled(selected);
    m_openButton->setEnabled(selected);
    m_deleteButton->setEnabled(canManage && selected);
    const bool deleted = selected && m_attachmentTable->item(row, 0)->data(DeletedRole).toBool();
    m_deleteButton->setText(deleted ? QStringLiteral("恢复") : QStringLiteral("删除"));
    m_deleteButton->setProperty("danger", !deleted);
    m_deleteButton->style()->unpolish(m_deleteButton);
    m_deleteButton->style()->polish(m_deleteButton);
}

void AttachmentPage::openAttachment()
{
    AttachmentService service(m_database, m_session.userId);
    AttachmentPayload payload;
    QString error;
    if (!service.loadAttachment(selectedAttachmentId(), &payload, &error)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), error);
        return;
    }
    if (payload.mimeType.startsWith(QStringLiteral("image/"))) {
        QPixmap pixmap;
        if (pixmap.loadFromData(payload.data)) {
            QDialog preview(this);
            preview.setWindowTitle(QStringLiteral("图片预览 - %1").arg(payload.fileName));
            preview.resize(900, 680);
            auto *layout = new QVBoxLayout(&preview);
            auto *scroll = new QScrollArea(&preview);
            scroll->setWidgetResizable(true);
            auto *image = new QLabel(scroll);
            image->setAlignment(Qt::AlignCenter);
            image->setPixmap(pixmap);
            image->setMinimumSize(pixmap.size().boundedTo(QSize(1200, 900)));
            scroll->setWidget(image);
            layout->addWidget(scroll, 1);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &preview);
            buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
            layout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
            preview.exec();
            return;
        }
    }
    QString safeName = payload.fileName;
    safeName.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/IceBeautyWms/attachments");
    QDir().mkpath(directory);
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(payload.data, QCryptographicHash::Sha256).toHex().left(12));
    const QString path = directory + QStringLiteral("/%1-%2-%3")
        .arg(payload.id).arg(digest).arg(safeName);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(payload.data) != payload.data.size()) {
        QMessageBox::warning(this, QStringLiteral("打开失败"), file.errorString());
        return;
    }
    file.close();
    if (payload.fileName.endsWith(QStringLiteral(".xlsx"), Qt::CaseInsensitive)) {
        QList<SpreadsheetPreviewSheet> sheets;
        QString previewError;
        if (OfficePreviewExtractor::previewXlsx(path, &sheets, &previewError)) {
            QDialog preview(this);
            preview.setWindowTitle(QStringLiteral("Excel 预览 - %1").arg(payload.fileName));
            preview.resize(1100, 720);
            auto *layout = new QVBoxLayout(&preview);
            auto *tabs = new QTabWidget(&preview);
            for (const SpreadsheetPreviewSheet &sheet : std::as_const(sheets)) {
                int columnCount = 0;
                for (const QStringList &row : sheet.rows) columnCount = qMax(columnCount, row.size());
                auto *table = new QTableWidget(qMax(0, sheet.rows.size() - 1), columnCount, tabs);
                table->setEditTriggers(QAbstractItemView::NoEditTriggers);
                table->verticalHeader()->setVisible(false);
                if (!sheet.rows.isEmpty()) table->setHorizontalHeaderLabels(sheet.rows.first());
                for (int row = 1; row < sheet.rows.size(); ++row) {
                    for (int column = 0; column < sheet.rows.at(row).size(); ++column)
                        table->setItem(row - 1, column,
                                       new QTableWidgetItem(sheet.rows.at(row).at(column)));
                }
                table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
                tabs->addTab(table, sheet.name);
            }
            layout->addWidget(tabs, 1);
            auto *hint = new QLabel(QStringLiteral("预览最多显示每个工作表前200行、30列；下载后可查看完整内容。"),
                                    &preview);
            hint->setObjectName(QStringLiteral("mutedText"));
            layout->addWidget(hint);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &preview);
            buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
            connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
            layout->addWidget(buttons);
            preview.exec();
            return;
        }
    }
    if (payload.fileName.endsWith(QStringLiteral(".docx"), Qt::CaseInsensitive)) {
        QString documentText;
        QString previewError;
        if (OfficePreviewExtractor::previewDocx(path, &documentText, &previewError)) {
            QDialog preview(this);
            preview.setWindowTitle(QStringLiteral("Word 预览 - %1").arg(payload.fileName));
            preview.resize(900, 700);
            auto *layout = new QVBoxLayout(&preview);
            auto *text = new QTextBrowser(&preview);
            text->setPlainText(documentText.isEmpty() ? QStringLiteral("文档没有可提取的文字内容。")
                                                       : documentText);
            layout->addWidget(text, 1);
            auto *hint = new QLabel(QStringLiteral("此处显示文档文字预览；复杂排版、表格和图片请下载后查看。"),
                                    &preview);
            hint->setObjectName(QStringLiteral("mutedText"));
            layout->addWidget(hint);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &preview);
            buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
            connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
            layout->addWidget(buttons);
            preview.exec();
            return;
        }
    }
    if (payload.mimeType == QStringLiteral("application/pdf")
        || payload.fileName.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        QDialog preview(this);
        preview.setWindowTitle(QStringLiteral("PDF 预览 - %1").arg(payload.fileName));
        preview.resize(1000, 760);
        auto *layout = new QVBoxLayout(&preview);
        auto *viewer = new QQuickWidget(&preview);
        viewer->setResizeMode(QQuickWidget::SizeRootObjectToView);
        viewer->rootContext()->setContextProperty(QStringLiteral("pdfPreviewUrl"),
                                                  QUrl::fromLocalFile(path));
        viewer->setSource(QUrl(QStringLiteral("qrc:/resources/PdfPreview.qml")));
        layout->addWidget(viewer, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &preview);
        buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
        if (viewer->status() == QQuickWidget::Ready) {
            preview.exec();
            return;
        }
    }
    OfficeTemplateService::openFileWithApplicationChoice(path, this);
}

void AttachmentPage::upload()
{
    const qlonglong documentId = selectedDocumentId();
    if (documentId <= 0) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择附件"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (file.size() > AttachmentService::MaximumAttachmentBytes) {
        QMessageBox::warning(this, QStringLiteral("文件过大"), QStringLiteral("单个附件不能超过50 MB。"));
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), file.errorString());
        return;
    }
    AttachmentService service(m_database, m_session.userId);
    QString error;
    if (!service.uploadDocumentAttachment(documentId, QFileInfo(path).fileName(),
                                          QMimeDatabase().mimeTypeForFile(path).name(),
                                          file.readAll(), nullptr, &error)) {
        QMessageBox::warning(this, QStringLiteral("上传失败"), error);
        return;
    }
    loadAttachments();
}

void AttachmentPage::download()
{
    AttachmentService service(m_database, m_session.userId);
    AttachmentPayload payload;
    QString error;
    if (!service.loadAttachment(selectedAttachmentId(), &payload, &error)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), error);
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存附件"), payload.fileName);
    if (path.isEmpty()) return;
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(payload.data) != payload.data.size()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), output.errorString());
        return;
    }
    QMessageBox::information(this, QStringLiteral("下载完成"), QStringLiteral("附件已保存。"));
}

void AttachmentPage::deleteOrRestore()
{
    const int row = m_attachmentTable->currentRow();
    if (row < 0) return;
    const bool currentlyDeleted = m_attachmentTable->item(row, 0)->data(DeletedRole).toBool();
    if (!currentlyDeleted
        && QMessageBox::question(this, QStringLiteral("确认删除"),
                                 QStringLiteral("附件将标记为已删除，之后仍可恢复。"))
               != QMessageBox::Yes) return;
    AttachmentService service(m_database, m_session.userId);
    QString error;
    if (!service.setDeleted(selectedAttachmentId(), !currentlyDeleted, &error)) {
        QMessageBox::warning(this, QStringLiteral("操作失败"), error);
        return;
    }
    loadAttachments();
}

void AttachmentPage::retryIncompleteForms()
{
    const qlonglong documentId = selectedDocumentId();
    if (documentId <= 0) return;
    if (QMessageBox::question(
            this, QStringLiteral("重新生成未完成表单"),
            QStringLiteral("将重新生成该业务单据尚未完成的 Excel 表单，只重新生成并保存表单文件。\n\n"
                           "本操作不会重新过账、不会改变任何库存数量、库存流水和单据状态。\n\n"
                           "是否继续？"))
        != QMessageBox::Yes) {
        return;
    }
    QStringList completedTitles;
    QStringList errors;
    OfficeTemplateService::retryIncompleteDocumentForms(m_database, m_session.userId, documentId,
                                                        &completedTitles, &errors);
    loadAttachments();
    if (errors.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("表单重新生成完成"),
            completedTitles.isEmpty()
                ? QStringLiteral("该单据没有需要重新生成的表单。")
                : QStringLiteral("以下表单已重新生成并保存到数据库附件和“我的文档\\冰美肌仓库系统表单”分类文件夹：\n\n%1")
                      .arg(completedTitles.join(QStringLiteral("\n"))));
        return;
    }
    QString message;
    if (!completedTitles.isEmpty()) {
        message = QStringLiteral("已重新生成并保存：\n%1\n\n")
                      .arg(completedTitles.join(QStringLiteral("\n")));
    }
    message += QStringLiteral("以下表单或步骤未完成：\n\n%1")
                   .arg(errors.join(QStringLiteral("\n")));
    QMessageBox::warning(this, QStringLiteral("部分表单未完成"), message);
}
