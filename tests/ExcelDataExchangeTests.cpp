#include "core/DatabaseConfig.h"
#include "database/DatabaseManager.h"
#include "database/SchemaMigrator.h"
#include "import/LegacyInventoryImporter.h"
#include "import/XlsxExporter.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

class ExcelDataExchangeTests final : public QObject
{
    Q_OBJECT
private slots:
    void generatedInitialTemplateCanBeParsed();
    void materialTemplateCanBeImportedAndUpdated();
    void modernWordDocumentCanBePreviewed();
};

void ExcelDataExchangeTests::generatedInitialTemplateCanBeParsed()
{
#ifndef Q_OS_WIN
    QSKIP("OOXML压缩当前使用Windows内置Zip能力。");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("期初库存模板.xlsx"));
    QString error;
    const QList<QList<QVariant>> data = {{QStringLiteral("TEST-001"), QStringLiteral("测试物料"),
        QStringLiteral("中文规格"), QStringLiteral("RAW"), QStringLiteral("盒"),
        QStringLiteral("BATCH-01"), 12.5}};
    QVERIFY2(XlsxExporter::writeSingleSheet(path, QStringLiteral("期初库存"),
        {QStringLiteral("物料编码"), QStringLiteral("物料名称"), QStringLiteral("规格"),
         QStringLiteral("分类编码"), QStringLiteral("单位"), QStringLiteral("批次号"),
         QStringLiteral("库存数量")}, data, &error), qPrintable(error));
    QList<LegacyImportRow> rows;
    QVERIFY2(LegacyInventoryImporter::parseFile(path, &rows, &error), qPrintable(error));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().status, LegacyImportStatus::Ready);
    QCOMPARE(rows.first().materialCode, QStringLiteral("TEST-001"));
    QCOMPARE(rows.first().materialName, QStringLiteral("测试物料"));
    QCOMPARE(rows.first().unit, QStringLiteral("盒"));
    QCOMPARE(rows.first().batchNo, QStringLiteral("BATCH-01"));
    QCOMPARE(rows.first().quantity, 12.5);
#endif
}

void ExcelDataExchangeTests::materialTemplateCanBeImportedAndUpdated()
{
#ifndef Q_OS_WIN
    QSKIP("OOXML压缩当前使用Windows内置Zip能力。");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DatabaseConfig config;
    config.filePath = directory.filePath(QStringLiteral("material-import.db"));
    DatabaseManager manager;
    QString error;
    QVERIFY2(manager.open(config, &error), qPrintable(error));
    QVERIFY2(SchemaMigrator::migrate(manager.database(), &error), qPrintable(error));
    QSqlQuery setup(manager.database());
    QVERIFY(setup.exec(QStringLiteral("INSERT INTO warehouses(code,name) VALUES('WH-A','物料仓')")));
    const qlonglong warehouseId = setup.lastInsertId().toLongLong();
    setup.prepare(QStringLiteral(
        "INSERT INTO locations(warehouse_id,code,name) VALUES(?,'A01','默认库位')"));
    setup.addBindValue(warehouseId);
    QVERIFY(setup.exec());

    const QString path = directory.filePath(QStringLiteral("物料导入模板.xlsx"));
    const QList<QList<QVariant>> data = {
        {QStringLiteral("MAT-100"), QStringLiteral("批量物料"), QStringLiteral("S-100"),
         QStringLiteral("RAW"), QStringLiteral("盒"), 5.5, QStringLiteral("WH-A"),
         QStringLiteral("A01"), QStringLiteral("是"), QStringLiteral("否"),
         QStringLiteral("测试品牌"), QStringLiteral("测试备注")},
        {QStringLiteral("MAT-ERR"), QString(), QStringLiteral("S-ERR"),
         QStringLiteral("RAW"), QStringLiteral("个"), 0, QString(), QString(),
         QStringLiteral("否"), QStringLiteral("否"), QString(), QString()}
    };
    const QStringList headers = {QStringLiteral("物料编码"), QStringLiteral("物料名称"),
        QStringLiteral("规格"), QStringLiteral("分类编码"), QStringLiteral("单位"),
        QStringLiteral("最低库存"), QStringLiteral("默认仓库编码"), QStringLiteral("默认库位编码"),
        QStringLiteral("批次管理"), QStringLiteral("SN管理"), QStringLiteral("品牌"),
        QStringLiteral("备注")};
    QVERIFY2(XlsxExporter::writeSingleSheet(path, QStringLiteral("物料导入"), headers, data, &error),
             qPrintable(error));
    QList<SpreadsheetPreviewSheet> previewSheets;
    QVERIFY2(OfficePreviewExtractor::previewXlsx(path, &previewSheets, &error), qPrintable(error));
    QCOMPARE(previewSheets.size(), 1);
    QCOMPARE(previewSheets.first().name, QStringLiteral("物料导入"));
    QCOMPARE(previewSheets.first().rows.at(1).at(0), QStringLiteral("MAT-100"));
    QList<MaterialImportRow> rows;
    QVERIFY2(MaterialExcelImporter::parseFile(path, &rows, &error), qPrintable(error));
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).status, MaterialImportStatus::Ready);
    QCOMPARE(rows.at(1).status, MaterialImportStatus::Error);
    QCOMPARE(rows.at(0).brand, QStringLiteral("测试品牌"));
    QCOMPARE(rows.at(0).minimumStock, 5.5);
    MaterialExcelImporter::validateReferences(manager.database(), &rows);
    QCOMPARE(rows.at(0).status, MaterialImportStatus::Ready);
    const qlonglong userId = [&] {
        QSqlQuery query(manager.database());
        return query.exec(QStringLiteral("SELECT id FROM users WHERE username='admin'")) && query.next()
            ? query.value(0).toLongLong() : 0;
    }();
    int created = 0;
    int updated = 0;
    QVERIFY2(MaterialExcelImporter::importRows(manager.database(), userId, rows,
                                               &created, &updated, &error), qPrintable(error));
    QCOMPARE(created, 1);
    QCOMPARE(updated, 0);
    QSqlQuery saved(manager.database());
    QVERIFY(saved.exec(QStringLiteral(
        "SELECT m.name,m.specification,m.brand,m.unit,m.minimum_stock,w.code,l.code,m.require_batch,"
        "m.require_serial,m.notes FROM materials m LEFT JOIN warehouses w ON w.id=m.default_warehouse_id "
        "LEFT JOIN locations l ON l.id=m.default_location_id WHERE m.code='MAT-100'")));
    QVERIFY(saved.next());
    QCOMPARE(saved.value(0).toString(), QStringLiteral("批量物料"));
    QCOMPARE(saved.value(2).toString(), QStringLiteral("测试品牌"));
    QCOMPARE(saved.value(4).toDouble(), 5.5);
    QCOMPARE(saved.value(5).toString(), QStringLiteral("WH-A"));
    QCOMPARE(saved.value(6).toString(), QStringLiteral("A01"));
    QVERIFY(saved.value(7).toBool());
    QVERIFY(!saved.value(8).toBool());

    rows[0].materialName = QStringLiteral("批量物料（更新）");
    rows[0].status = MaterialImportStatus::Ready;
    rows[0].message.clear();
    MaterialExcelImporter::validateReferences(manager.database(), &rows);
    QCOMPARE(rows.at(0).status, MaterialImportStatus::Warning);
    QVERIFY2(MaterialExcelImporter::importRows(manager.database(), userId, rows,
                                               &created, &updated, &error), qPrintable(error));
    QCOMPARE(created, 0);
    QCOMPARE(updated, 1);
    QVERIFY(saved.exec(QStringLiteral("SELECT name FROM materials WHERE code='MAT-100'")));
    QVERIFY(saved.next());
    QCOMPARE(saved.value(0).toString(), QStringLiteral("批量物料（更新）"));
#endif
}

void ExcelDataExchangeTests::modernWordDocumentCanBePreviewed()
{
#ifndef Q_OS_WIN
    QSKIP("OOXML压缩当前使用Windows内置Zip能力。");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("docx-source"));
    QVERIFY(QDir().mkpath(sourcePath + QStringLiteral("/word")));
    QFile document(sourcePath + QStringLiteral("/word/document.xml"));
    QVERIFY(document.open(QIODevice::WriteOnly));
    const QByteArray xml = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body><w:p><w:r><w:t>第一段</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>第二段</w:t></w:r></w:p></w:body></w:document>");
    QCOMPARE(document.write(xml), xml.size());
    document.close();
    QString escapedSource = sourcePath;
    escapedSource.replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString archivePath = directory.filePath(QStringLiteral("preview.zip"));
    QString escapedArchive = archivePath;
    escapedArchive.replace(QLatin1Char('\''), QStringLiteral("''"));
    QProcess compression;
    compression.start(QStringLiteral("powershell.exe"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
         QStringLiteral("-Command"),
         QStringLiteral("Add-Type -AssemblyName System.IO.Compression.FileSystem; "
                        "[IO.Compression.ZipFile]::CreateFromDirectory('%1','%2')")
             .arg(escapedSource, escapedArchive)});
    QVERIFY(compression.waitForFinished(30000));
    QVERIFY2(compression.exitCode() == 0,
             compression.readAllStandardError().constData());
    const QString docxPath = directory.filePath(QStringLiteral("preview.docx"));
    QVERIFY(QFile::rename(archivePath, docxPath));
    QString text;
    QString error;
    QVERIFY2(OfficePreviewExtractor::previewDocx(docxPath, &text, &error), qPrintable(error));
    QCOMPARE(text, QStringLiteral("第一段\n第二段"));
#endif
}

QTEST_GUILESS_MAIN(ExcelDataExchangeTests)
#include "ExcelDataExchangeTests.moc"
