#include "ui/pages/InspectionPage.h"

#include "services/InspectionService.h"
#include "services/OfficeTemplateService.h"
#include "ui/widgets/ComboBoxSearch.h"
#include "ui/widgets/TableExcelExport.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
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
#include <QMimeDatabase>
#include <QPushButton>
#include <QSet>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {
enum NoticeColumn {
    NoticeRowColumn,
    NoticeMaterialColumn,
    NoticeSpecificationColumn,
    NoticeQuantityColumn,
    NoticeOrderColumn,
    NoticeBatchColumn,
    NoticeSupplierColumn,
    NoticeDeleteColumn,
    NoticeColumnCount
};

QString statusText(const QString &status)
{
    if (status == QStringLiteral("PENDING")) return QStringLiteral("在检");
    if (status == QStringLiteral("QUALIFIED")) return QStringLiteral("合格");
    if (status == QStringLiteral("UNQUALIFIED")) return QStringLiteral("不合格");
    if (status == QStringLiteral("USED")) return QStringLiteral("已入库");
    if (status == QStringLiteral("CANCELLED")) return QStringLiteral("已取消");
    return status;
}

OfficeTemplateDocument noticeTemplate(const InspectionNotice &notice)
{
    OfficeTemplateDocument document;
    document.kind = OfficeFormKind::Inspection;
    document.documentNumber = notice.inspectionNumber;
    document.documentDate = notice.notificationDate;
    document.fields.insert(QStringLiteral("entrustedBy"), notice.entrustedBy);
    document.fields.insert(QStringLiteral("notificationDepartment"), notice.notificationDepartment);
    document.fields.insert(QStringLiteral("arrivalDate"), notice.arrivalDate.toString(Qt::ISODate));
    document.fields.insert(QStringLiteral("urgency"), notice.urgency);
    document.fields.insert(QStringLiteral("purchaseOrderNumber"), notice.purchaseOrderNumber);
    document.fields.insert(QStringLiteral("supplier"), notice.supplier);
    for (const InspectionNoticeLine &source : notice.lines) {
        OfficeTemplateLine line;
        line.materialCode = source.materialCode;
        line.materialName = source.materialName;
        line.specification = source.specification;
        line.quantity = source.quantity;
        line.orderNumber = source.purchaseOrderNumber;
        line.batchNo = source.batchNumber;
        line.supplier = source.supplier;
        document.lines.append(line);
    }
    return document;
}

class NoticeEditDialog final : public QDialog
{
public:
    NoticeEditDialog(QSqlDatabase database, Session session, qlonglong noticeId,
                     QWidget *parent = nullptr)
        : QDialog(parent), m_database(std::move(database)), m_session(std::move(session)),
          m_noticeId(noticeId)
    {
        setWindowTitle(noticeId > 0 ? QStringLiteral("修改材料检验通知单")
                                    : QStringLiteral("在线填写材料检验通知单"));
        resize(1280, 800);
        setMinimumSize(980, 620);
        auto *root = new QVBoxLayout(this);
        auto *heading = new QLabel(QStringLiteral("材料检验通知单"), this);
        heading->setAlignment(Qt::AlignCenter);
        heading->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;"));
        root->addWidget(heading);
        auto *form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        m_numberLabel = new QLabel(QStringLiteral("保存时生成"), this);
        m_notificationDate = new QDateEdit(QDate::currentDate(), this);
        m_notificationDate->setCalendarPopup(true);
        m_notificationDate->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        m_arrivalDate = new QDateEdit(QDate::currentDate(), this);
        m_arrivalDate->setCalendarPopup(true);
        m_arrivalDate->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        m_entrusted = new QLineEdit(m_session.displayName, this);
        m_department = new QLineEdit(QStringLiteral("检验部"), this);
        m_urgency = new QComboBox(this);
        m_urgency->addItem(QStringLiteral("加急（1-3天）"), QStringLiteral("EXPEDITED"));
        m_urgency->addItem(QStringLiteral("急（7天内）"), QStringLiteral("URGENT"));
        m_urgency->addItem(QStringLiteral("正常（7-15天）"), QStringLiteral("NORMAL"));
        m_urgency->setCurrentIndex(2);
        m_purchaseOrder = new QLineEdit(this);
        m_supplier = new QComboBox(this);
        populateSuppliers();
        ComboBoxSearch::enableContainsSearch(m_supplier, QStringLiteral("选择或输入供应商"));
        form->addRow(QStringLiteral("通知单号"), m_numberLabel);
        form->addRow(QStringLiteral("通知日期 *"), m_notificationDate);
        form->addRow(QStringLiteral("到货日期 *"), m_arrivalDate);
        form->addRow(QStringLiteral("委托人员 *"), m_entrusted);
        form->addRow(QStringLiteral("通知单位 *"), m_department);
        form->addRow(QStringLiteral("待检状态 *"), m_urgency);
        form->addRow(QStringLiteral("默认采购单号"), m_purchaseOrder);
        form->addRow(QStringLiteral("默认供应商"), m_supplier);
        root->addLayout(form);

        auto *toolbar = new QHBoxLayout;
        toolbar->addWidget(new QLabel(QStringLiteral("送检物料明细"), this));
        toolbar->addStretch();
        auto *fullScreen = new QPushButton(QStringLiteral("全屏显示"), this);
        auto *add = new QPushButton(QStringLiteral("添加物料"), this);
        toolbar->addWidget(fullScreen);
        toolbar->addWidget(add);
        root->addLayout(toolbar);
        m_lines = new QTableWidget(0, NoticeColumnCount, this);
        m_lines->setHorizontalHeaderLabels({
            QStringLiteral("序号"), QStringLiteral("物料"), QStringLiteral("规格型号"),
            QStringLiteral("送检数量"), QStringLiteral("采购单号"), QStringLiteral("批号"),
            QStringLiteral("供应商"), QStringLiteral("操作")});
        m_lines->verticalHeader()->hide();
        m_lines->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_lines->horizontalHeader()->setStretchLastSection(false);
        m_lines->setColumnWidth(NoticeMaterialColumn, 280);
        m_lines->setColumnWidth(NoticeSpecificationColumn, 210);
        m_lines->setColumnWidth(NoticeOrderColumn, 150);
        m_lines->setColumnWidth(NoticeSupplierColumn, 220);
        root->addWidget(m_lines, 1);
        auto *buttons = new QDialogButtonBox(this);
        auto *preview = buttons->addButton(QStringLiteral("预览当前通知单"), QDialogButtonBox::ActionRole);
        auto *cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
        auto *saveButton = buttons->addButton(QStringLiteral("保存通知单"), QDialogButtonBox::AcceptRole);
        saveButton->setProperty("primary", true);
        root->addWidget(buttons);
        connect(add, &QPushButton::clicked, this, [this] { addLine({}); });
        connect(fullScreen, &QPushButton::clicked, this, [this] {
            if (isMaximized()) showNormal(); else showMaximized();
        });
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(saveButton, &QPushButton::clicked, this, [this] { this->save(); });
        connect(preview, &QPushButton::clicked, this, [this] {
            InspectionNotice notice;
            QString error;
            if (!draftNotice(&notice, &error)) {
                QMessageBox::warning(this, QStringLiteral("通知单不完整"), error);
                return;
            }
            OfficeTemplateService::openPreview(noticeTemplate(notice), this);
        });
        connect(m_notificationDate, &QDateEdit::dateChanged, this, [this](const QDate &date) {
            if (m_noticeId > 0) return;
            InspectionService service(m_database, m_session.userId);
            QString error;
            const QString number = service.previewNextInspectionNumber(date, &error);
            m_numberLabel->setText(number.isEmpty() ? QStringLiteral("保存时生成") : number);
        });
        connect(m_purchaseOrder, &QLineEdit::textChanged, this, [this](const QString &text) {
            for (int row = 0; row < m_lines->rowCount(); ++row) {
                auto *edit = qobject_cast<QLineEdit *>(m_lines->cellWidget(row, NoticeOrderColumn));
                if (edit && edit->text().trimmed().isEmpty()) edit->setPlaceholderText(text);
            }
        });
        if (m_noticeId > 0) load();
        else {
            InspectionService service(m_database, m_session.userId);
            m_numberLabel->setText(service.previewNextInspectionNumber(QDate::currentDate()));
            addLine({});
        }
    }

    bool saved() const { return m_saved; }

private:
    void populateSuppliers()
    {
        m_supplier->setEditable(true);
        m_supplier->addItem(QString());
        QSet<QString> seen;
        const QStringList sql = {
            QStringLiteral("SELECT supplier FROM batches WHERE trim(supplier)<>''"),
            QStringLiteral("SELECT supplier FROM business_documents WHERE trim(supplier)<>''"),
            QStringLiteral("SELECT brand FROM materials WHERE trim(brand)<>''")};
        QStringList values;
        for (const QString &statement : sql) {
            QSqlQuery query(m_database);
            if (!query.exec(statement)) continue;
            while (query.next()) {
                const QString value = query.value(0).toString().trimmed();
                const QString key = value.toCaseFolded();
                if (value.isEmpty() || seen.contains(key)) continue;
                seen.insert(key);
                values.append(value);
            }
        }
        values.sort(Qt::CaseInsensitive);
        m_supplier->addItems(values);
    }

    QComboBox *materialCombo(qlonglong selectedId)
    {
        auto *combo = new QComboBox(m_lines);
        combo->addItem(QStringLiteral("请选择物料"), qlonglong(0));
        QSqlQuery query(m_database);
        query.exec(QStringLiteral(
            "SELECT id,code,name,specification FROM materials ORDER BY code COLLATE NOCASE"));
        while (query.next()) {
            combo->addItem(QStringLiteral("%1 - %2").arg(query.value(1).toString(), query.value(2).toString()),
                           query.value(0));
            combo->setItemData(combo->count() - 1, query.value(3), Qt::UserRole + 1);
        }
        ComboBoxSearch::enableContainsSearch(combo, QStringLiteral("输入物料号或名称"));
        const int selected = combo->findData(selectedId);
        if (selected >= 0) combo->setCurrentIndex(selected);
        return combo;
    }

    void addLine(const InspectionNoticeLine &line)
    {
        const int row = m_lines->rowCount();
        m_lines->insertRow(row);
        m_lines->setItem(row, NoticeRowColumn, new QTableWidgetItem(QString::number(row + 1)));
        auto *material = materialCombo(line.materialId);
        m_lines->setCellWidget(row, NoticeMaterialColumn, material);
        auto *specification = new QTableWidgetItem(line.specification);
        specification->setFlags(specification->flags() & ~Qt::ItemIsEditable);
        m_lines->setItem(row, NoticeSpecificationColumn, specification);
        auto *quantity = new QDoubleSpinBox(m_lines);
        quantity->setDecimals(6);
        quantity->setRange(0.000001, 999999999.0);
        quantity->setValue(line.quantity > 0 ? line.quantity : 1.0);
        m_lines->setCellWidget(row, NoticeQuantityColumn, quantity);
        auto *order = new QLineEdit(line.purchaseOrderNumber, m_lines);
        order->setPlaceholderText(m_purchaseOrder->text());
        m_lines->setCellWidget(row, NoticeOrderColumn, order);
        m_lines->setCellWidget(row, NoticeBatchColumn, new QLineEdit(line.batchNumber, m_lines));
        auto *supplier = new QLineEdit(line.supplier, m_lines);
        supplier->setPlaceholderText(m_supplier->currentText());
        m_lines->setCellWidget(row, NoticeSupplierColumn, supplier);
        auto *remove = new QPushButton(QStringLiteral("删除"), m_lines);
        m_lines->setCellWidget(row, NoticeDeleteColumn, remove);
        connect(material, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, material](int index) {
            for (int row = 0; row < m_lines->rowCount(); ++row) {
                if (m_lines->cellWidget(row, NoticeMaterialColumn) != material) continue;
                m_lines->item(row, NoticeSpecificationColumn)->setText(
                    material->itemData(index, Qt::UserRole + 1).toString());
                break;
            }
        });
        connect(remove, &QPushButton::clicked, this, [this, remove] {
            for (int row = 0; row < m_lines->rowCount(); ++row) {
                if (m_lines->cellWidget(row, NoticeDeleteColumn) != remove) continue;
                m_lines->removeRow(row);
                for (int next = row; next < m_lines->rowCount(); ++next)
                    m_lines->item(next, NoticeRowColumn)->setText(QString::number(next + 1));
                break;
            }
        });
    }

    InspectionNoticeDraft draft(QString *errorMessage) const
    {
        InspectionNoticeDraft value;
        value.notificationDate = m_notificationDate->date();
        value.arrivalDate = m_arrivalDate->date();
        value.entrustedBy = m_entrusted->text().trimmed();
        value.notificationDepartment = m_department->text().trimmed();
        value.urgency = m_urgency->currentData().toString();
        value.purchaseOrderNumber = m_purchaseOrder->text().trimmed();
        value.supplier = m_supplier->currentText().trimmed();
        if (value.entrustedBy.isEmpty() || value.notificationDepartment.isEmpty()) {
            if (errorMessage) *errorMessage = QStringLiteral("委托人员和通知单位不能为空。");
            return {};
        }
        for (int row = 0; row < m_lines->rowCount(); ++row) {
            auto *material = qobject_cast<QComboBox *>(m_lines->cellWidget(row, NoticeMaterialColumn));
            auto *quantity = qobject_cast<QDoubleSpinBox *>(m_lines->cellWidget(row, NoticeQuantityColumn));
            auto *order = qobject_cast<QLineEdit *>(m_lines->cellWidget(row, NoticeOrderColumn));
            auto *batch = qobject_cast<QLineEdit *>(m_lines->cellWidget(row, NoticeBatchColumn));
            auto *supplier = qobject_cast<QLineEdit *>(m_lines->cellWidget(row, NoticeSupplierColumn));
            if (!material || material->currentData().toLongLong() <= 0 || !quantity) {
                if (errorMessage) *errorMessage = QStringLiteral("第 %1 行物料或数量无效。").arg(row + 1);
                return {};
            }
            InspectionNoticeLine line;
            line.lineNumber = row + 1;
            line.materialId = material->currentData().toLongLong();
            line.materialCode = material->currentText().section(QStringLiteral(" - "), 0, 0);
            line.materialName = material->currentText().section(QStringLiteral(" - "), 1);
            line.specification = m_lines->item(row, NoticeSpecificationColumn)->text();
            line.quantity = quantity->value();
            line.purchaseOrderNumber = order && !order->text().trimmed().isEmpty()
                ? order->text().trimmed() : value.purchaseOrderNumber;
            line.batchNumber = batch ? batch->text().trimmed() : QString();
            line.supplier = supplier && !supplier->text().trimmed().isEmpty()
                ? supplier->text().trimmed() : value.supplier;
            value.lines.append(line);
        }
        if (value.lines.isEmpty() && errorMessage)
            *errorMessage = QStringLiteral("至少添加一行送检物料。");
        return value;
    }

    bool draftNotice(InspectionNotice *notice, QString *errorMessage) const
    {
        const InspectionNoticeDraft value = draft(errorMessage);
        if (value.lines.isEmpty()) return false;
        InspectionNotice result;
        result.id = m_noticeId;
        result.inspectionNumber = m_numberLabel->text();
        if (result.inspectionNumber.isEmpty() || result.inspectionNumber == QStringLiteral("保存时生成"))
            result.inspectionNumber = QStringLiteral("预览号-保存时生成");
        result.notificationDate = value.notificationDate;
        result.arrivalDate = value.arrivalDate;
        result.entrustedBy = value.entrustedBy;
        result.notificationDepartment = value.notificationDepartment;
        result.urgency = value.urgency;
        result.purchaseOrderNumber = value.purchaseOrderNumber;
        result.supplier = value.supplier;
        result.lines = value.lines;
        *notice = result;
        return true;
    }

    void load()
    {
        InspectionService service(m_database, m_session.userId);
        InspectionNotice notice;
        QString error;
        if (!service.readNotice(m_noticeId, &notice, &error)) {
            QMessageBox::warning(this, QStringLiteral("读取失败"), error);
            return;
        }
        m_numberLabel->setText(notice.inspectionNumber);
        m_notificationDate->setDate(notice.notificationDate);
        m_arrivalDate->setDate(notice.arrivalDate);
        m_entrusted->setText(notice.entrustedBy);
        m_department->setText(notice.notificationDepartment);
        int index = m_urgency->findData(notice.urgency);
        if (index >= 0) m_urgency->setCurrentIndex(index);
        m_purchaseOrder->setText(notice.purchaseOrderNumber);
        index = m_supplier->findText(notice.supplier, Qt::MatchFixedString);
        if (index >= 0) m_supplier->setCurrentIndex(index);
        else m_supplier->setEditText(notice.supplier);
        for (const InspectionNoticeLine &line : std::as_const(notice.lines)) addLine(line);
    }

    void save()
    {
        QString error;
        const InspectionNoticeDraft value = draft(&error);
        if (value.lines.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("通知单不完整"), error);
            return;
        }
        InspectionService service(m_database, m_session.userId);
        qlonglong id = m_noticeId;
        QString number;
        const bool ok = id > 0
            ? service.updateNotice(id, value, &number, &error)
            : service.createNotice(value, &id, &number, &error);
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("保存失败"), error);
            return;
        }
        m_noticeId = id;
        m_numberLabel->setText(number);
        InspectionNotice notice;
        if (!service.readNotice(id, &notice, &error)) {
            QMessageBox::warning(this, QStringLiteral("通知单已保存"),
                                 QStringLiteral("通知数据已保存，但读取模板数据失败：%1").arg(error));
            m_saved = true;
            accept();
            return;
        }
        QString savedPath;
        if (!OfficeTemplateService::attachToInspectionNotice(
                noticeTemplate(notice), m_database, m_session.userId, id, &savedPath, &error)) {
            QMessageBox::warning(
                this, QStringLiteral("通知单已保存，Excel未完成"),
                QStringLiteral("通知数据已经保存，但Excel通知单未能归档或写入附件：\n%1")
                    .arg(error));
        } else {
            QMessageBox::information(
                this, QStringLiteral("保存完成"),
                QStringLiteral("通知单 %1 已保存到数据库和“我的文档”归档目录。")
                    .arg(number));
        }
        m_saved = true;
        accept();
    }

    QSqlDatabase m_database;
    Session m_session;
    qlonglong m_noticeId = 0;
    bool m_saved = false;
    QLabel *m_numberLabel = nullptr;
    QDateEdit *m_notificationDate = nullptr;
    QDateEdit *m_arrivalDate = nullptr;
    QLineEdit *m_entrusted = nullptr;
    QLineEdit *m_department = nullptr;
    QComboBox *m_urgency = nullptr;
    QLineEdit *m_purchaseOrder = nullptr;
    QComboBox *m_supplier = nullptr;
    QTableWidget *m_lines = nullptr;
};

class ResultDialog final : public QDialog
{
public:
    ResultDialog(QSqlDatabase database, Session session, qlonglong noticeId, QWidget *parent = nullptr)
        : QDialog(parent), m_database(std::move(database)), m_session(std::move(session)),
          m_noticeId(noticeId)
    {
        setWindowTitle(QStringLiteral("录入/修改检验结果"));
        resize(680, 430);
        auto *root = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        m_date = new QDateEdit(QDate::currentDate(), this);
        m_date->setCalendarPopup(true);
        m_date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        m_inspector = new QLineEdit(m_session.displayName, this);
        m_result = new QComboBox(this);
        m_result->addItem(QStringLiteral("合格"), QStringLiteral("QUALIFIED"));
        m_result->addItem(QStringLiteral("不合格"), QStringLiteral("UNQUALIFIED"));
        m_conclusion = new QTextEdit(this);
        m_attachment = new QLabel(QStringLiteral("尚未选择新附件"), this);
        auto *choose = new QPushButton(QStringLiteral("选择检验附件"), this);
        auto *attachmentRow = new QWidget(this);
        auto *attachmentLayout = new QHBoxLayout(attachmentRow);
        attachmentLayout->setContentsMargins(0, 0, 0, 0);
        attachmentLayout->addWidget(choose);
        attachmentLayout->addWidget(m_attachment, 1);
        form->addRow(QStringLiteral("检验日期 *"), m_date);
        form->addRow(QStringLiteral("检验员 *"), m_inspector);
        form->addRow(QStringLiteral("检验结果 *"), m_result);
        form->addRow(QStringLiteral("检验说明"), m_conclusion);
        form->addRow(QStringLiteral("检验附件"), attachmentRow);
        root->addLayout(form);
        auto *hint = new QLabel(
            QStringLiteral("检验结果只保存在系统和检验附件中，不写入材料检验通知单。合格时必须已有或重新上传附件。"), this);
        hint->setWordWrap(true);
        hint->setObjectName(QStringLiteral("mutedText"));
        root->addWidget(hint);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save, this);
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存检验结果"));
        root->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { save(); });
        connect(choose, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择检验附件"), {},
                QStringLiteral("常用文件 (*.pdf *.jpg *.jpeg *.png *.doc *.docx *.xls *.xlsx);;所有文件 (*.*)"));
            if (path.isEmpty()) return;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                QMessageBox::warning(this, QStringLiteral("附件读取失败"), file.errorString());
                return;
            }
            if (file.size() > InspectionService::MaximumAttachmentBytes) {
                QMessageBox::warning(this, QStringLiteral("附件过大"), QStringLiteral("附件不能超过50 MB。"));
                return;
            }
            m_data = file.readAll();
            m_fileName = QFileInfo(path).fileName();
            m_mimeType = QMimeDatabase().mimeTypeForFile(path).name();
            m_attachment->setText(QStringLiteral("已选择：%1").arg(m_fileName));
        });
        InspectionService service(m_database, m_session.userId);
        InspectionNotice notice;
        QString error;
        if (service.readNotice(m_noticeId, &notice, &error)) {
            if (notice.inspectionDate.isValid()) m_date->setDate(notice.inspectionDate);
            if (!notice.inspectorName.isEmpty()) m_inspector->setText(notice.inspectorName);
            const int index = m_result->findData(notice.inspectionResult);
            if (index >= 0) m_result->setCurrentIndex(index);
            m_conclusion->setPlainText(notice.conclusion);
            if (notice.inspectionAttachmentId > 0)
                m_attachment->setText(QStringLiteral("已存在检验附件；不选择新文件则保留原附件"));
        }
    }

    bool saved() const { return m_saved; }

private:
    void save()
    {
        InspectionNoticeResult value;
        value.inspectionDate = m_date->date();
        value.inspectorName = m_inspector->text().trimmed();
        value.result = m_result->currentData().toString();
        value.conclusion = m_conclusion->toPlainText().trimmed();
        value.attachmentFileName = m_fileName;
        value.attachmentMimeType = m_mimeType;
        value.attachmentData = m_data;
        InspectionService service(m_database, m_session.userId);
        QString error;
        if (!service.recordResult(m_noticeId, value, &error)) {
            QMessageBox::warning(this, QStringLiteral("保存检验结果失败"), error);
            return;
        }
        m_saved = true;
        accept();
    }

    QSqlDatabase m_database;
    Session m_session;
    qlonglong m_noticeId = 0;
    bool m_saved = false;
    QDateEdit *m_date = nullptr;
    QLineEdit *m_inspector = nullptr;
    QComboBox *m_result = nullptr;
    QTextEdit *m_conclusion = nullptr;
    QLabel *m_attachment = nullptr;
    QString m_fileName;
    QString m_mimeType;
    QByteArray m_data;
};
}

InspectionPage::InspectionPage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);
    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QVBoxLayout(panel);
    auto *toolbar = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("材料检验通知与在检管理"), panel);
    title->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    m_searchEdit = new QLineEdit(panel);
    m_searchEdit->setPlaceholderText(QStringLiteral("按通知单号、采购单号、供应商或委托人检索"));
    auto *search = new QPushButton(QStringLiteral("查询"), panel);
    auto *create = new QPushButton(QStringLiteral("在线填写检验通知单"), panel);
    create->setProperty("primary", true);
    toolbar->addWidget(title);
    toolbar->addStretch();
    toolbar->addWidget(m_searchEdit, 1);
    toolbar->addWidget(search);
    toolbar->addWidget(create);
    layout->addLayout(toolbar);

    auto *pendingToolbar = new QHBoxLayout;
    pendingToolbar->addWidget(new QLabel(QStringLiteral("在检列表"), panel));
    pendingToolbar->addStretch();
    m_editPendingButton = new QPushButton(QStringLiteral("修改通知单"), panel);
    m_recordResultButton = new QPushButton(QStringLiteral("录入/修改检验结果"), panel);
    auto *openPendingNotice = new QPushButton(QStringLiteral("打开通知单Excel"), panel);
    auto *fullScreenPending = new QPushButton(QStringLiteral("全屏显示"), panel);
    pendingToolbar->addWidget(m_editPendingButton);
    pendingToolbar->addWidget(m_recordResultButton);
    pendingToolbar->addWidget(openPendingNotice);
    pendingToolbar->addWidget(fullScreenPending);
    layout->addLayout(pendingToolbar);
    m_pendingTable = new QTableWidget(0, 9, panel);
    m_pendingTable->setProperty("excelExportTitle", QStringLiteral("在检列表"));
    m_pendingTable->setHorizontalHeaderLabels({
        QStringLiteral("序号"), QStringLiteral("通知单号"), QStringLiteral("通知日期"),
        QStringLiteral("到货日期"), QStringLiteral("委托人员"), QStringLiteral("通知单位"),
        QStringLiteral("供应商"), QStringLiteral("明细数"), QStringLiteral("状态")});
    m_pendingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pendingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pendingTable->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_pendingTable, 1);

    auto *historyToolbar = new QHBoxLayout;
    historyToolbar->addWidget(new QLabel(QStringLiteral("已检及历史通知单"), panel));
    historyToolbar->addStretch();
    m_viewHistoryButton = new QPushButton(QStringLiteral("查看/修改通知单"), panel);
    auto *historyResult = new QPushButton(QStringLiteral("修改检验结果"), panel);
    auto *openHistoryNotice = new QPushButton(QStringLiteral("打开通知单Excel"), panel);
    auto *openInspectionAttachment = new QPushButton(QStringLiteral("打开检验附件"), panel);
    auto *fullScreenHistory = new QPushButton(QStringLiteral("全屏显示"), panel);
    historyToolbar->addWidget(m_viewHistoryButton);
    historyToolbar->addWidget(historyResult);
    historyToolbar->addWidget(openHistoryNotice);
    historyToolbar->addWidget(openInspectionAttachment);
    historyToolbar->addWidget(fullScreenHistory);
    layout->addLayout(historyToolbar);
    m_historyTable = new QTableWidget(0, 10, panel);
    m_historyTable->setProperty("excelExportTitle", QStringLiteral("已检及历史通知单"));
    m_historyTable->setHorizontalHeaderLabels({
        QStringLiteral("序号"), QStringLiteral("通知单号"), QStringLiteral("通知日期"),
        QStringLiteral("采购单号"), QStringLiteral("供应商"), QStringLiteral("检验日期"),
        QStringLiteral("检验员"), QStringLiteral("检验结果"), QStringLiteral("关联入库单ID"),
        QStringLiteral("明细数")});
    m_historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_historyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_historyTable->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_historyTable, 1);
    root->addWidget(panel, 1);

    connect(create, &QPushButton::clicked, this, &InspectionPage::createNotice);
    connect(search, &QPushButton::clicked, this, &InspectionPage::refreshNotices);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &InspectionPage::refreshNotices);
    connect(m_editPendingButton, &QPushButton::clicked, this, &InspectionPage::editPendingNotice);
    connect(m_recordResultButton, &QPushButton::clicked, this, &InspectionPage::recordInspectionResult);
    connect(m_viewHistoryButton, &QPushButton::clicked, this, &InspectionPage::viewHistoryNotice);
    connect(historyResult, &QPushButton::clicked, this, &InspectionPage::editHistoryResult);
    const auto openNoticeFile = [this](QTableWidget *table, bool templateFile) {
        const qlonglong id = selectedNoticeId(table);
        if (id <= 0) {
            QMessageBox::information(activeDialogParent(), QStringLiteral("请选择通知单"),
                                     QStringLiteral("请先选择一条送检通知单。"));
            return;
        }
        QSqlQuery query(m_database);
        query.prepare(templateFile
            ? QStringLiteral(
                "SELECT a.original_file_name,a.file_data FROM inspection_notices n "
                "JOIN attachments a ON a.id=n.template_file_attachment_id "
                "WHERE n.id=? AND a.is_deleted=0")
            : QStringLiteral(
                "SELECT a.original_file_name,a.file_data FROM inspection_notices n "
                "JOIN attachments a ON a.id=n.inspection_attachment_id "
                "WHERE n.id=? AND a.is_deleted=0"));
        query.addBindValue(id);
        if (!query.exec() || !query.next()) {
            QMessageBox::information(activeDialogParent(), QStringLiteral("没有可打开的文件"),
                                     templateFile ? QStringLiteral("该通知单Excel尚未成功保存。")
                                                  : QStringLiteral("该通知单尚未上传检验附件。"));
            return;
        }
        const QString name = QFileInfo(query.value(0).toString()).fileName();
        const QByteArray data = query.value(1).toByteArray();
        const QString directory = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                      .filePath(QStringLiteral("IceBeautyWms/inspection-files"));
        QDir().mkpath(directory);
        const QString path = QDir(directory).filePath(
            QStringLiteral("%1_%2").arg(QUuid::createUuid().toString(QUuid::WithoutBraces), name));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
            QMessageBox::warning(activeDialogParent(), QStringLiteral("打开失败"), file.errorString());
            return;
        }
        file.close();
        OfficeTemplateService::openFileWithApplicationChoice(path, activeDialogParent());
    };
    connect(openPendingNotice, &QPushButton::clicked, this,
            [openNoticeFile, this] { openNoticeFile(m_pendingTable, true); });
    connect(openHistoryNotice, &QPushButton::clicked, this,
            [openNoticeFile, this] { openNoticeFile(m_historyTable, true); });
    connect(openInspectionAttachment, &QPushButton::clicked, this,
            [openNoticeFile, this] { openNoticeFile(m_historyTable, false); });
    connect(fullScreenPending, &QPushButton::clicked, this, [this, openNoticeFile] {
        TableExcelExport::fullScreenTable(m_pendingTable, QStringLiteral("全部在检通知单"), this,
            [this] { refreshNotices(); }, {
                {QStringLiteral("修改通知单"), [this] { editPendingNotice(); }},
                {QStringLiteral("录入/修改检验结果"), [this] { recordInspectionResult(); }},
                {QStringLiteral("打开通知单Excel"), [this, openNoticeFile] { openNoticeFile(m_pendingTable, true); }}
            });
    });
    connect(fullScreenHistory, &QPushButton::clicked, this, [this, openNoticeFile] {
        TableExcelExport::fullScreenTable(m_historyTable, QStringLiteral("全部已检及历史通知单"), this,
            [this] { refreshNotices(); }, {
                {QStringLiteral("查看/修改通知单"), [this] { viewHistoryNotice(); }},
                {QStringLiteral("修改检验结果"), [this] { editHistoryResult(); }},
                {QStringLiteral("打开通知单Excel"), [this, openNoticeFile] { openNoticeFile(m_historyTable, true); }},
                {QStringLiteral("打开检验附件"), [this, openNoticeFile] { openNoticeFile(m_historyTable, false); }}
            });
    });
    connect(m_pendingTable, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { editPendingNotice(); });
    connect(m_historyTable, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { viewHistoryNotice(); });
    refreshNotices();
}

void InspectionPage::refreshReferenceData()
{
    refreshNotices();
}

qlonglong InspectionPage::selectedNoticeId(QTableWidget *table) const
{
    if (!table || table->currentRow() < 0 || !table->item(table->currentRow(), 0)) return 0;
    if (table->isRowHidden(table->currentRow()) || table->selectedItems().isEmpty()) return 0;
    return table->item(table->currentRow(), 0)->data(Qt::UserRole).toLongLong();
}

QWidget *InspectionPage::activeDialogParent()
{
    for (QTableWidget *table : {m_pendingTable, m_historyTable}) {
        if (table && table->property("tableFullScreenActive").toBool()) return table->window();
    }
    return this;
}

bool InspectionPage::editNotice(qlonglong noticeId)
{
    NoticeEditDialog dialog(m_database, m_session, noticeId, activeDialogParent());
    dialog.exec();
    if (!dialog.saved()) return false;
    refreshNotices();
    emit inspectionChanged();
    return true;
}

void InspectionPage::createNotice()
{
    editNotice();
}

void InspectionPage::editPendingNotice()
{
    const qlonglong id = selectedNoticeId(m_pendingTable);
    if (id <= 0) {
        QMessageBox::information(activeDialogParent(), QStringLiteral("请选择通知单"), QStringLiteral("请先选择一条在检通知单。"));
        return;
    }
    editNotice(id);
}

void InspectionPage::viewHistoryNotice()
{
    const qlonglong id = selectedNoticeId(m_historyTable);
    if (id <= 0) {
        QMessageBox::information(activeDialogParent(), QStringLiteral("请选择通知单"), QStringLiteral("请先选择一条历史通知单。"));
        return;
    }
    editNotice(id);
}

void InspectionPage::recordInspectionResult()
{
    const qlonglong id = selectedNoticeId(m_pendingTable);
    if (id <= 0) {
        QMessageBox::information(activeDialogParent(), QStringLiteral("请选择通知单"), QStringLiteral("请先选择一条在检通知单。"));
        return;
    }
    ResultDialog dialog(m_database, m_session, id, activeDialogParent());
    dialog.exec();
    if (dialog.saved()) {
        refreshNotices();
        emit inspectionChanged();
    }
}

void InspectionPage::editHistoryResult()
{
    const qlonglong id = selectedNoticeId(m_historyTable);
    if (id <= 0) {
        QMessageBox::information(activeDialogParent(), QStringLiteral("请选择通知单"),
                                 QStringLiteral("请先选择一条历史通知单。"));
        return;
    }
    ResultDialog dialog(m_database, m_session, id, activeDialogParent());
    dialog.exec();
    if (dialog.saved()) {
        refreshNotices();
        emit inspectionChanged();
    }
}

void InspectionPage::populateTable(QTableWidget *table, const QStringList &statuses)
{
    table->setRowCount(0);
    InspectionService service(m_database, m_session.userId);
    QList<InspectionNotice> notices;
    QString error;
    const QString keyword = table->property("tableFullScreenActive").toBool() ? QString() : m_searchEdit->text();
    if (!service.listNotices(statuses, keyword, &notices, &error)) {
        QMessageBox::warning(activeDialogParent(), QStringLiteral("查询送检通知失败"), error);
        return;
    }
    const bool pending = table == m_pendingTable;
    for (int index = 0; index < notices.size(); ++index) {
        const InspectionNotice &notice = notices.at(index);
        const int row = table->rowCount();
        table->insertRow(row);
        QStringList values;
        if (pending) {
            values = {QString::number(index + 1), notice.inspectionNumber,
                      notice.notificationDate.toString(Qt::ISODate),
                      notice.arrivalDate.toString(Qt::ISODate), notice.entrustedBy,
                      notice.notificationDepartment, notice.supplier,
                      QString::number(notice.lineCount), statusText(notice.status)};
        } else {
            values = {QString::number(index + 1), notice.inspectionNumber,
                      notice.notificationDate.toString(Qt::ISODate), notice.purchaseOrderNumber,
                      notice.supplier, notice.inspectionDate.toString(Qt::ISODate),
                      notice.inspectorName, statusText(notice.inspectionResult),
                      notice.linkedDocumentId > 0 ? QString::number(notice.linkedDocumentId) : QString(),
                      QString::number(notice.lineCount)};
        }
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setData(Qt::UserRole, notice.id);
            table->setItem(row, column, item);
        }
    }
    table->resizeColumnsToContents();
    if (table->rowCount() > 0) table->selectRow(0);
}

void InspectionPage::refreshNotices()
{
    populateTable(m_pendingTable, {QStringLiteral("PENDING")});
    populateTable(m_historyTable,
                  {QStringLiteral("QUALIFIED"), QStringLiteral("UNQUALIFIED"),
                   QStringLiteral("USED"), QStringLiteral("CANCELLED")});
}
