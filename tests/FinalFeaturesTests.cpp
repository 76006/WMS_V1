#include "core/DatabaseConfig.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"

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

class FinalFeaturesTests final : public QObject
{
    Q_OBJECT
private slots:
    void migrationPermissionsImagesAndRules();
};

void FinalFeaturesTests::migrationPermissionsImagesAndRules()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DatabaseConfig config;
    config.filePath = directory.filePath(QStringLiteral("final-features.db"));
    DatabaseManager manager;
    QString error;
    QVERIFY2(manager.open(config, &error), qPrintable(error));
    QVERIFY2(SchemaMigrator::migrate(manager.database(), &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT MAX(version) FROM schema_migrations")).toInt(), 5);
    QVERIFY(scalar(manager.database(), QStringLiteral(
        "SELECT COUNT(*) FROM role_permissions rp JOIN roles r ON r.id=rp.role_id "
        "WHERE r.code='PRODUCTION' AND rp.permission_code='POST_PRODUCTION' AND rp.is_allowed=1")).toInt() == 1);
    QVERIFY(scalar(manager.database(), QStringLiteral(
        "SELECT COUNT(*) FROM role_permissions rp JOIN roles r ON r.id=rp.role_id "
        "WHERE r.code='QUERY' AND rp.permission_code='POST_INVENTORY' AND rp.is_allowed=1")).toInt() == 0);

    QSqlQuery material(manager.database());
    QVERIFY(material.exec(QStringLiteral(
        "INSERT INTO materials(code,name,category_id,unit) VALUES('IMG-001','图片物料',"
        "(SELECT id FROM material_categories WHERE code='RAW'),'个')")));
    const qlonglong materialId = material.lastInsertId().toLongLong();
    const QByteArray imageData("test-image-bytes");
    QSqlQuery image(manager.database());
    image.prepare(QStringLiteral(
        "INSERT INTO material_images(material_id,original_file_name,mime_type,sha256,image_data,updated_by) "
        "VALUES(?,?,?,?,?,(SELECT id FROM users WHERE username='admin'))"));
    image.addBindValue(materialId);
    image.addBindValue(QStringLiteral("物料.png"));
    image.addBindValue(QStringLiteral("image/png"));
    image.addBindValue(QString::fromLatin1(QCryptographicHash::hash(imageData, QCryptographicHash::Sha256).toHex()));
    image.addBindValue(imageData);
    QVERIFY(image.exec());
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT length(image_data) FROM material_images WHERE material_id=%1").arg(materialId)).toInt(),
        imageData.size());

    QSqlQuery rule(manager.database());
    QVERIFY(rule.exec(QStringLiteral(
        "UPDATE number_rules SET prefix='TEST',sequence_width=6 WHERE document_type='CGRK'")));
    QCOMPARE(scalar(manager.database(), QStringLiteral(
        "SELECT prefix||':'||sequence_width FROM number_rules WHERE document_type='CGRK'")).toString(),
        QStringLiteral("TEST:6"));

    QVERIFY2(SchemaMigrator::migrate(manager.database(), &error), qPrintable(error));
    QCOMPARE(scalar(manager.database(), QStringLiteral("SELECT COUNT(*) FROM schema_migrations WHERE version=3")).toInt(), 1);
}

QTEST_GUILESS_MAIN(FinalFeaturesTests)
#include "FinalFeaturesTests.moc"
