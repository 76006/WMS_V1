#include "core/DatabaseConfig.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"
#include "services/AttachmentService.h"

#include <QCryptographicHash>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QVariant scalar(QSqlDatabase database, const QString &sql)
{
    QSqlQuery query(database);
    return query.exec(sql) && query.next() ? query.value(0) : QVariant();
}
}

class TraceabilityAttachmentTests final : public QObject
{
    Q_OBJECT
private slots:
    void attachmentLifecycle();
};

void TraceabilityAttachmentTests::attachmentLifecycle()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DatabaseConfig config;
    config.filePath = directory.filePath(QStringLiteral("attachments.db"));
    DatabaseManager manager;
    QString error;
    QVERIFY2(manager.open(config, &error), qPrintable(error));
    QVERIFY2(SchemaMigrator::migrate(manager.database(), &error), qPrintable(error));
    const qlonglong userId = scalar(manager.database(),
        QStringLiteral("SELECT id FROM users WHERE username='admin'")).toLongLong();

    QSqlQuery warehouse(manager.database());
    QVERIFY(warehouse.exec(QStringLiteral("INSERT INTO warehouses(code,name) VALUES('AW','附件仓')")));
    const qlonglong warehouseId = warehouse.lastInsertId().toLongLong();
    QSqlQuery location(manager.database());
    location.prepare(QStringLiteral("INSERT INTO locations(warehouse_id,code,name) VALUES(?,'A1','附件库位')"));
    location.addBindValue(warehouseId);
    QVERIFY(location.exec());
    QSqlQuery document(manager.database());
    document.prepare(QStringLiteral(
        "INSERT INTO business_documents(document_no,document_type,stock_direction,document_date,"
        "status,created_by) VALUES('DOC-A-1','QTRK','IN','2026-09-08','POSTED',?)"));
    document.addBindValue(userId);
    QVERIFY(document.exec());
    const qlonglong documentId = document.lastInsertId().toLongLong();

    const QByteArray content("attachment-content-123");
    AttachmentService service(manager.database(), userId);
    qlonglong attachmentId = 0;
    QVERIFY2(service.uploadDocumentAttachment(documentId, QStringLiteral("检验报告.txt"),
                                               QStringLiteral("text/plain"), content,
                                               &attachmentId, &error), qPrintable(error));
    QVERIFY(attachmentId > 0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT file_size FROM attachments WHERE id=%1").arg(attachmentId)).toLongLong(),
             content.size());
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT sha256 FROM attachments WHERE id=%1").arg(attachmentId)).toString(),
             QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex()));

    AttachmentPayload payload;
    QVERIFY2(service.loadAttachment(attachmentId, &payload, &error), qPrintable(error));
    QCOMPARE(payload.fileName, QStringLiteral("检验报告.txt"));
    QCOMPARE(payload.data, content);
    QVERIFY(!payload.deleted);

    QVERIFY2(service.setDeleted(attachmentId, true, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT is_deleted FROM attachments WHERE id=%1").arg(attachmentId)).toInt(), 1);
    QVERIFY2(service.setDeleted(attachmentId, false, &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT is_deleted FROM attachments WHERE id=%1").arg(attachmentId)).toInt(), 0);
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT COUNT(*) FROM audit_logs WHERE entity_type='attachment' AND entity_id=%1")
        .arg(attachmentId)).toInt(), 3);

    QByteArray oversized;
    oversized.resize(AttachmentService::MaximumAttachmentBytes + 1);
    QVERIFY(!service.uploadDocumentAttachment(documentId, QStringLiteral("too-large.bin"),
                                               QStringLiteral("application/octet-stream"),
                                               oversized, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("50 MB")));
}

QTEST_GUILESS_MAIN(TraceabilityAttachmentTests)
#include "TraceabilityAttachmentTests.moc"
