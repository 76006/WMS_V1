#pragma once

#include "services/InventoryService.h"

#include <QDate>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

class QWidget;

enum class OfficeFormKind
{
    Inspection,
    RawMaterialInbound,
    ProductionIssue,
    FinishedGoodsInbound,
    StockOutbound,
    DeliveryConfirmation
};

struct OfficeTemplateLine
{
    QString materialCode;
    QString materialName;
    QString specification;
    QString unit;
    QString batchNo;
    QString serialNumbers;
    QString notes;
    QString orderNumber;
    QString supplier;
    double quantity = 0.0;
    double unitUsage = 0.0;
    double externalQuantity = 0.0;
    double reworkQuantity = 0.0;
    double lossQuantity = 0.0;
    double returnQuantity = 0.0;
};

struct OfficeTemplateDocument
{
    OfficeFormKind kind = OfficeFormKind::StockOutbound;
    QString documentNumber;
    QDate documentDate;
    QMap<QString, QString> fields;
    QList<OfficeTemplateLine> lines;
};

class OfficeTemplateService
{
public:
    static QString formTitle(OfficeFormKind kind);
    static QString templateFileName(OfficeFormKind kind);
    static QString outputFileName(const OfficeTemplateDocument &document);
    static QString archiveRootPath();

    static QList<OfficeTemplateLine> materialLines(
        QSqlDatabase database,
        const QList<StockMovementRequest> &lines,
        QString *errorMessage = nullptr);

    static bool renderToFile(const OfficeTemplateDocument &document,
                             const QString &outputPath,
                             QString *errorMessage = nullptr);
    // 仅生成并归档表单文件，不写数据库附件和 document_forms 记录；
    // 供业务单据尚未生效时（如送检单先于入库单）提前保存正本。
    static bool saveToDocumentsArchive(const OfficeTemplateDocument &document,
                                       QString *savedPath = nullptr,
                                       QString *errorMessage = nullptr);
    static bool openPreview(const OfficeTemplateDocument &document,
                            QWidget *parent = nullptr);
    // Excel 类文件打开前由用户选择 WPS 表格或 Microsoft Excel；
    // 其他文件仍交给 Windows 默认程序。用户取消选择不视为错误。
    static bool openFileWithApplicationChoice(const QString &filePath,
                                              QWidget *parent = nullptr);
    static bool attachToDocument(const OfficeTemplateDocument &document,
                                 QSqlDatabase database,
                                 qlonglong operatorId,
                                 qlonglong businessDocumentId,
                                 QString *errorMessage = nullptr,
                                 bool openArchivedFile = true);
    // 独立材料检验通知单尚未形成入库业务单据，附件按 inspection_notice 归档。
    // 同一通知单再次保存时原位替换数据库附件和“我的文档”中的正本。
    static bool attachToInspectionNotice(const OfficeTemplateDocument &document,
                                         QSqlDatabase database,
                                         qlonglong operatorId,
                                         qlonglong inspectionNoticeId,
                                         QString *savedPath = nullptr,
                                         QString *errorMessage = nullptr);

    // 表单记录可重试：业务单据已生效后，只重新生成表单，不涉及任何库存变动。
    static bool hasIncompleteDocumentForms(QSqlDatabase database, qlonglong documentId);
    static bool retryIncompleteDocumentForms(QSqlDatabase database,
                                             qlonglong operatorId,
                                             qlonglong documentId,
                                             QStringList *completedTitles = nullptr,
                                             QStringList *errors = nullptr,
                                             bool openArchivedFiles = true);
    // 业务单据被事务修订后，从正式业务表重新同步表单载荷、数据库附件和本地归档。
    static bool synchronizeDocumentForms(QSqlDatabase database,
                                         qlonglong operatorId,
                                         qlonglong documentId,
                                         QStringList *errors = nullptr,
                                         bool openArchivedFiles = false);

private:
    static QString findTemplateFile(OfficeFormKind kind);

    static QJsonObject lineToJson(const OfficeTemplateLine &line);
    static OfficeTemplateLine lineFromJson(const QJsonObject &value);
    static QJsonObject documentToJson(const OfficeTemplateDocument &document);
    static bool documentFromJson(const QJsonObject &value,
                                 OfficeTemplateDocument *document,
                                 QString *errorMessage);
    static bool documentFromPayload(const QString &payload,
                                    OfficeTemplateDocument *document,
                                    QString *errorMessage);

    static bool upsertDocumentForm(QSqlDatabase database,
                                   qlonglong documentId,
                                   const OfficeTemplateDocument &document,
                                   qlonglong *formRecordId,
                                   QString *errorMessage);
    static void markDocumentFormFailed(QSqlDatabase database,
                                       qlonglong formRecordId,
                                       const QString &message,
                                       QString *errorMessage);
    static bool markDocumentFormCompleted(QSqlDatabase database,
                                          qlonglong formRecordId,
                                          qlonglong attachmentId,
                                          QString *errorMessage);
    static bool storeDocumentFormAttachment(QSqlDatabase database,
                                            qlonglong formRecordId,
                                            qlonglong attachmentId,
                                            QString *errorMessage);
    static qlonglong documentFormAttachmentId(QSqlDatabase database, qlonglong formRecordId);
    static bool isReusableAttachment(QSqlDatabase database,
                                     qlonglong documentId,
                                     qlonglong attachmentId);
    static bool persistDocumentForm(QSqlDatabase database,
                                    qlonglong operatorId,
                                    qlonglong documentId,
                                    qlonglong formRecordId,
                                    const OfficeTemplateDocument &document,
                                    bool *formCompleted,
                                    QString *errorMessage,
                                    bool openArchivedFile = true);
    static QString incompleteFormHint(bool formCompleted);
};
