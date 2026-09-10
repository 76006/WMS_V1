#pragma once

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

struct InspectionNoticeLine
{
    qlonglong id = 0;
    int lineNumber = 0;
    qlonglong materialId = 0;
    QString materialCode;
    QString materialName;
    QString specification;
    double quantity = 0.0;
    QString purchaseOrderNumber;
    QString batchNumber;
    QString supplier;
};

struct InspectionNoticeDraft
{
    QDate notificationDate;
    QDate arrivalDate;
    QString entrustedBy;
    QString notificationDepartment;
    QString urgency = QStringLiteral("NORMAL");
    QString purchaseOrderNumber;
    QString supplier;
    QList<InspectionNoticeLine> lines;
};

struct InspectionNoticeResult
{
    QDate inspectionDate;
    QString inspectorName;
    // 仅允许 QUALIFIED 或 UNQUALIFIED。
    QString result;
    QString conclusion;
    QString attachmentFileName;
    QString attachmentMimeType;
    QByteArray attachmentData;
};

struct InspectionTemplateArtifact
{
    QString payload;
    QString archivePath;
    QString fileName;
    QString mimeType;
    QByteArray data;
};

struct InspectionNotice
{
    qlonglong id = 0;
    QString inspectionNumber;
    QString status;
    QDate notificationDate;
    QDate arrivalDate;
    QString entrustedBy;
    QString notificationDepartment;
    QString urgency;
    QString purchaseOrderNumber;
    QString supplier;
    QString inspectorName;
    QDate inspectionDate;
    QString inspectionResult;
    QString conclusion;
    qlonglong inspectionAttachmentId = 0;
    qlonglong templateFileAttachmentId = 0;
    QString templatePayload;
    QString archivePath;
    qlonglong linkedDocumentId = 0;
    qlonglong createdBy = 0;
    QString creatorName;
    QDateTime createdAt;
    QDateTime updatedAt;
    QDateTime completedAt;
    int lineCount = 0;
    QList<InspectionNoticeLine> lines;
};

class InspectionService
{
public:
    static constexpr qint64 MaximumAttachmentBytes = 50LL * 1024LL * 1024LL;

    InspectionService(QSqlDatabase database, qlonglong operatorId);

    // 仅供界面显示候选号；createNotice 会在写事务内再次取最终号码。
    QString previewNextInspectionNumber(const QDate &date,
                                        QString *errorMessage = nullptr) const;

    bool createNotice(const InspectionNoticeDraft &draft,
                      qlonglong *noticeId,
                      QString *inspectionNumber,
                      QString *errorMessage = nullptr);
    // 通知单已检验或已关联入库后仍可完整修改；关联入库中的检验摘要会同步更新。
    bool updateNotice(qlonglong noticeId,
                      const InspectionNoticeDraft &draft,
                      QString *inspectionNumber = nullptr,
                      QString *errorMessage = nullptr);
    bool readNotice(qlonglong noticeId,
                    InspectionNotice *notice,
                    QString *errorMessage = nullptr) const;
    bool listNotices(const QStringList &statuses,
                     const QString &keyword,
                     QList<InspectionNotice> *notices,
                     QString *errorMessage = nullptr) const;
    bool listQualifiedUnused(QList<InspectionNotice> *notices,
                             QString *errorMessage = nullptr) const;
    bool recordResult(qlonglong noticeId,
                      const InspectionNoticeResult &result,
                      QString *errorMessage = nullptr);
    // UI生成通知单Excel后回写载荷、归档路径及可选的数据库文件附件。
    bool recordTemplateArtifact(qlonglong noticeId,
                                const InspectionTemplateArtifact &artifact,
                                QString *errorMessage = nullptr);
    bool cancelNotice(qlonglong noticeId, QString *errorMessage = nullptr);

private:
    bool validateDraft(const InspectionNoticeDraft &draft, QString *errorMessage) const;
    QString nextInspectionNumber(const QDate &date, QString *errorMessage) const;
    bool replaceLines(qlonglong noticeId,
                      const QList<InspectionNoticeLine> &lines,
                      QString *errorMessage);
    qlonglong storeAttachment(qlonglong noticeId,
                              const QString &fileName,
                              const QString &mimeType,
                              const QByteArray &data,
                              QString *errorMessage);
    bool markAttachmentDeleted(qlonglong attachmentId, QString *errorMessage);
    bool writeAudit(const QString &action,
                    qlonglong noticeId,
                    const QString &detail,
                    QString *errorMessage);
    bool beginImmediate(QString *errorMessage);
    bool commit(QString *errorMessage);
    void rollback();

    QSqlDatabase m_database;
    qlonglong m_operatorId = 0;
};
