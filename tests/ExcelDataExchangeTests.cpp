#include "import/LegacyInventoryImporter.h"
#include "import/XlsxExporter.h"

#include <QTemporaryDir>
#include <QtTest>

class ExcelDataExchangeTests final : public QObject
{
    Q_OBJECT
private slots:
    void generatedInitialTemplateCanBeParsed();
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

QTEST_GUILESS_MAIN(ExcelDataExchangeTests)
#include "ExcelDataExchangeTests.moc"
