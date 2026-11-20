#include "import/XlsxExporter.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QSaveFile>
#include <functional>
#include <QTemporaryDir>
#include <QTime>
#include <QXmlStreamWriter>

namespace {
void setError(QString *target, const QString &message)
{
    if (target) *target = message;
}

bool writeTextFile(const QString &path, const QByteArray &data, QString *errorMessage)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(data) != data.size()) {
        setError(errorMessage, QStringLiteral("无法生成Excel内部文件：%1").arg(path));
        return false;
    }
    return true;
}

QString columnName(int column)
{
    QString result;
    while (column > 0) {
        const int remainder = (column - 1) % 26;
        result.prepend(QChar(QLatin1Char('A').unicode() + remainder));
        column = (column - 1) / 26;
    }
    return result;
}

QString safeSheetName(QString name, int index)
{
    static const QString invalid = QStringLiteral("[]:*?/\\");
    name = name.trimmed();
    for (const QChar character : invalid) name.replace(character, QLatin1Char('_'));
    if (name.isEmpty()) name = QStringLiteral("工作表%1").arg(index + 1);
    return name.left(31);
}

bool isNumeric(const QVariant &value)
{
    switch (value.metaType().id()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Float:
    case QMetaType::Double:
        return true;
    default:
        return false;
    }
}

int displayWidth(const QString &value)
{
    int width = 0;
    for (const QChar character : value) width += character.unicode() > 0x7f ? 2 : 1;
    return width;
}

QList<int> columnWidths(const XlsxExporter::SheetData &sheet)
{
    QList<int> widths;
    for (const QString &header : sheet.headers)
        widths.append(qBound(12, displayWidth(header) + 3, 32));
    for (const QList<QVariant> &row : sheet.rows) {
        for (int column = 0; column < qMin(row.size(), widths.size()); ++column) {
            QString text = row.at(column).toString();
            if (row.at(column).metaType().id() == QMetaType::QDate)
                text = row.at(column).toDate().toString(QStringLiteral("yyyy-MM-dd"));
            widths[column] = qMax(widths.at(column), qBound(12, displayWidth(text) + 2, 32));
        }
    }
    return widths;
}

void writeCell(QXmlStreamWriter &xml, const QString &reference,
               const QVariant &value, bool header)
{
    xml.writeStartElement(QStringLiteral("c"));
    xml.writeAttribute(QStringLiteral("r"), reference);
    if (header) xml.writeAttribute(QStringLiteral("s"), QStringLiteral("1"));
    if (!header && value.metaType().id() == QMetaType::QDate) {
        xml.writeAttribute(QStringLiteral("s"), QStringLiteral("2"));
        xml.writeTextElement(QStringLiteral("v"),
                             QString::number(QDate(1899, 12, 30).daysTo(value.toDate())));
    } else if (!header && value.metaType().id() == QMetaType::QDateTime) {
        xml.writeAttribute(QStringLiteral("s"), QStringLiteral("3"));
        const QDateTime dateTime = value.toDateTime();
        const double serial = QDate(1899, 12, 30).daysTo(dateTime.date())
            + QTime(0, 0).msecsTo(dateTime.time()) / 86400000.0;
        xml.writeTextElement(QStringLiteral("v"), QString::number(serial, 'g', 15));
    } else if (!header && isNumeric(value)) {
        xml.writeTextElement(QStringLiteral("v"), QString::number(value.toDouble(), 'g', 15));
    } else {
        xml.writeAttribute(QStringLiteral("t"), QStringLiteral("inlineStr"));
        xml.writeStartElement(QStringLiteral("is"));
        xml.writeTextElement(QStringLiteral("t"), value.toString());
        xml.writeEndElement();
    }
    xml.writeEndElement();
}

bool writeWorksheet(const QString &path, const XlsxExporter::SheetData &sheet,
                    QString *errorMessage)
{
    QByteArray data;
    QXmlStreamWriter xml(&data);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("worksheet"));
    xml.writeDefaultNamespace(QStringLiteral(
        "http://schemas.openxmlformats.org/spreadsheetml/2006/main"));
    const int lastRow = sheet.rows.size() + 1;
    const QString lastColumn = columnName(sheet.headers.size());
    xml.writeStartElement(QStringLiteral("dimension"));
    xml.writeAttribute(QStringLiteral("ref"),
                       QStringLiteral("A1:%1%2").arg(lastColumn).arg(lastRow));
    xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("sheetViews"));
    xml.writeStartElement(QStringLiteral("sheetView"));
    xml.writeAttribute(QStringLiteral("workbookViewId"), QStringLiteral("0"));
    xml.writeStartElement(QStringLiteral("pane"));
    xml.writeAttribute(QStringLiteral("ySplit"), QStringLiteral("1"));
    xml.writeAttribute(QStringLiteral("topLeftCell"), QStringLiteral("A2"));
    xml.writeAttribute(QStringLiteral("state"), QStringLiteral("frozen"));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("cols"));
    const QList<int> widths = columnWidths(sheet);
    for (int column = 0; column < widths.size(); ++column) {
        xml.writeStartElement(QStringLiteral("col"));
        xml.writeAttribute(QStringLiteral("min"), QString::number(column + 1));
        xml.writeAttribute(QStringLiteral("max"), QString::number(column + 1));
        xml.writeAttribute(QStringLiteral("width"), QString::number(widths.at(column)));
        xml.writeAttribute(QStringLiteral("customWidth"), QStringLiteral("1"));
        xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("sheetData"));
    auto writeRow = [&](int rowNumber, const QList<QVariant> &values, bool header) {
        xml.writeStartElement(QStringLiteral("row"));
        xml.writeAttribute(QStringLiteral("r"), QString::number(rowNumber));
        for (int index = 0; index < values.size(); ++index)
            writeCell(xml, columnName(index + 1) + QString::number(rowNumber),
                      values.at(index), header);
        xml.writeEndElement();
    };
    QList<QVariant> headerValues;
    for (const QString &header : sheet.headers) headerValues.append(header);
    writeRow(1, headerValues, true);
    for (int index = 0; index < sheet.rows.size(); ++index)
        writeRow(index + 2, sheet.rows.at(index), false);
    xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("autoFilter"));
    xml.writeAttribute(QStringLiteral("ref"),
                       QStringLiteral("A1:%1%2").arg(lastColumn).arg(lastRow));
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return writeTextFile(path, data, errorMessage);
}

bool packageWorkbook(const QString &filePath, const QStringList &sheetNames,
                     const QByteArray &styles, const QMap<QString, QString> &printNames,
                     const std::function<bool(int, const QString &, QString *)> &writeSheet,
                     QString *errorMessage)
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        setError(errorMessage, QStringLiteral("无法创建Excel导出临时目录。"));
        return false;
    }

    QByteArray contentTypesData;
    QXmlStreamWriter contentTypes(&contentTypesData);
    contentTypes.writeStartDocument();
    contentTypes.writeStartElement(QStringLiteral("Types"));
    contentTypes.writeDefaultNamespace(QStringLiteral(
        "http://schemas.openxmlformats.org/package/2006/content-types"));
    auto writeDefault = [&](const QString &extension, const QString &contentType) {
        contentTypes.writeStartElement(QStringLiteral("Default"));
        contentTypes.writeAttribute(QStringLiteral("Extension"), extension);
        contentTypes.writeAttribute(QStringLiteral("ContentType"), contentType);
        contentTypes.writeEndElement();
    };
    auto writeOverride = [&](const QString &part, const QString &contentType) {
        contentTypes.writeStartElement(QStringLiteral("Override"));
        contentTypes.writeAttribute(QStringLiteral("PartName"), part);
        contentTypes.writeAttribute(QStringLiteral("ContentType"), contentType);
        contentTypes.writeEndElement();
    };
    writeDefault(QStringLiteral("rels"), QStringLiteral(
        "application/vnd.openxmlformats-package.relationships+xml"));
    writeDefault(QStringLiteral("xml"), QStringLiteral("application/xml"));
    writeOverride(QStringLiteral("/xl/workbook.xml"), QStringLiteral(
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"));
    for (int index = 0; index < sheetNames.size(); ++index)
        writeOverride(QStringLiteral("/xl/worksheets/sheet%1.xml").arg(index + 1),
                      QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"));
    writeOverride(QStringLiteral("/xl/styles.xml"), QStringLiteral(
        "application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"));
    contentTypes.writeEndElement();
    contentTypes.writeEndDocument();

    const QByteArray rootRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)";

    if (!writeTextFile(temporary.filePath(QStringLiteral("[Content_Types].xml")),
                       contentTypesData, errorMessage)
        || !writeTextFile(temporary.filePath(QStringLiteral("_rels/.rels")), rootRels, errorMessage)
        || !writeTextFile(temporary.filePath(QStringLiteral("xl/styles.xml")), styles, errorMessage))
        return false;

    QByteArray workbookRelsData;
    QXmlStreamWriter workbookRels(&workbookRelsData);
    workbookRels.writeStartDocument();
    workbookRels.writeStartElement(QStringLiteral("Relationships"));
    workbookRels.writeDefaultNamespace(QStringLiteral(
        "http://schemas.openxmlformats.org/package/2006/relationships"));
    for (int index = 0; index < sheetNames.size(); ++index) {
        workbookRels.writeStartElement(QStringLiteral("Relationship"));
        workbookRels.writeAttribute(QStringLiteral("Id"), QStringLiteral("rId%1").arg(index + 1));
        workbookRels.writeAttribute(QStringLiteral("Type"), QStringLiteral(
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet"));
        workbookRels.writeAttribute(QStringLiteral("Target"),
                                    QStringLiteral("worksheets/sheet%1.xml").arg(index + 1));
        workbookRels.writeEndElement();
    }
    workbookRels.writeStartElement(QStringLiteral("Relationship"));
    workbookRels.writeAttribute(QStringLiteral("Id"),
                                QStringLiteral("rId%1").arg(sheetNames.size() + 1));
    workbookRels.writeAttribute(QStringLiteral("Type"), QStringLiteral(
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles"));
    workbookRels.writeAttribute(QStringLiteral("Target"), QStringLiteral("styles.xml"));
    workbookRels.writeEndElement();
    workbookRels.writeEndElement();
    workbookRels.writeEndDocument();
    if (!writeTextFile(temporary.filePath(QStringLiteral("xl/_rels/workbook.xml.rels")),
                       workbookRelsData, errorMessage)) return false;

    QByteArray workbookData;
    QXmlStreamWriter workbook(&workbookData);
    workbook.writeStartDocument();
    workbook.writeStartElement(QStringLiteral("workbook"));
    workbook.writeDefaultNamespace(QStringLiteral(
        "http://schemas.openxmlformats.org/spreadsheetml/2006/main"));
    workbook.writeNamespace(QStringLiteral(
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
        QStringLiteral("r"));
    workbook.writeStartElement(QStringLiteral("sheets"));
    for (int index = 0; index < sheetNames.size(); ++index) {
        workbook.writeStartElement(QStringLiteral("sheet"));
        workbook.writeAttribute(QStringLiteral("name"), sheetNames.at(index));
        workbook.writeAttribute(QStringLiteral("sheetId"), QString::number(index + 1));
        workbook.writeAttribute(QStringLiteral(
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
            QStringLiteral("id"), QStringLiteral("rId%1").arg(index + 1));
        workbook.writeEndElement();
    }
    workbook.writeEndElement();
    if (!printNames.isEmpty()) {
        workbook.writeStartElement(QStringLiteral("definedNames"));
        for (auto it = printNames.cbegin(); it != printNames.cend(); ++it) {
            workbook.writeStartElement(QStringLiteral("definedName"));
            workbook.writeAttribute(QStringLiteral("name"), it.key());
            workbook.writeAttribute(QStringLiteral("localSheetId"), QStringLiteral("0"));
            workbook.writeCharacters(it.value());
            workbook.writeEndElement();
        }
        workbook.writeEndElement();
    }
    workbook.writeEndElement();
    workbook.writeEndDocument();
    if (!writeTextFile(temporary.filePath(QStringLiteral("xl/workbook.xml")),
                       workbookData, errorMessage)) return false;

    for (int index = 0; index < sheetNames.size(); ++index) {
        if (!writeSheet(index, temporary.filePath(
                QStringLiteral("xl/worksheets/sheet%1.xml").arg(index + 1)), errorMessage)) return false;
    }

    QTemporaryDir archiveDirectory;
    const QString archive = archiveDirectory.filePath(QStringLiteral("output.zip"));
    if (!archiveDirectory.isValid() || !QDir().mkpath(QFileInfo(filePath).absolutePath())) {
        setError(errorMessage, QStringLiteral("无法建立 Excel 输出目录。"));
        return false;
    }
    // Explicit entry names keep OOXML paths portable on Windows PowerShell 5.
    QProcess zip;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("WMS_XLSX_PARTS"), QDir::toNativeSeparators(temporary.path()));
    environment.insert(QStringLiteral("WMS_XLSX_ARCHIVE"), archive);
    zip.setProcessEnvironment(environment);
    const QString script = QStringLiteral(
        "$ErrorActionPreference='Stop';Add-Type -AssemblyName System.IO.Compression.FileSystem;"
        "$root=$env:WMS_XLSX_PARTS;$z=[IO.Compression.ZipFile]::Open($env:WMS_XLSX_ARCHIVE,'Create');"
        "try{foreach($p in [IO.Directory]::EnumerateFiles($root,'*','AllDirectories')){"
        "$name=$p.Substring($root.Length+1).Replace([char]92,[char]47);"
        "$null=[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z,$p,$name)}}finally{$z.Dispose()}");
    zip.start(QStringLiteral("powershell.exe"),
              {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
               QStringLiteral("-Command"), script});
    if (!zip.waitForFinished(30000) || zip.exitStatus() != QProcess::NormalExit || zip.exitCode() != 0) {
        setError(errorMessage, QStringLiteral("生成 Excel 压缩包失败：%1")
                                   .arg(QString::fromLocal8Bit(zip.readAllStandardError())));
        return false;
    }
    QFile input(archive);
    QSaveFile output(filePath);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        setError(errorMessage, QStringLiteral("无法保存 Excel 文件：%1").arg(filePath));
        return false;
    }
    while (!input.atEnd()) {
        const auto block = input.read(65536);
        if (block.isEmpty() || output.write(block) != block.size()) {
            setError(errorMessage, QStringLiteral("写入 Excel 文件失败：%1").arg(filePath));
            return false;
        }
    }
    if (output.commit()) return true;
    setError(errorMessage, output.errorString());
    return false;
}

}

bool XlsxExporter::writeSingleSheet(const QString &filePath, const QString &sheetName,
                                    const QStringList &headers,
                                    const QList<QList<QVariant>> &rows,
                                    QString *errorMessage)
{
    return writeWorkbook(filePath, {{sheetName, headers, rows}}, errorMessage);
}

bool XlsxExporter::writeWorkbook(const QString &filePath,
                                 const QList<SheetData> &sheets,
                                 QString *errorMessage)
{
    if (filePath.trimmed().isEmpty() || sheets.isEmpty()) {
        setError(errorMessage, QStringLiteral("Excel文件路径或工作表不能为空。"));
        return false;
    }
    QList<SheetData> normalizedSheets = sheets;
    QSet<QString> names;
    for (int index = 0; index < normalizedSheets.size(); ++index) {
        SheetData &sheet = normalizedSheets[index];
        if (sheet.headers.isEmpty()) {
            setError(errorMessage, QStringLiteral("第%1个工作表的表头不能为空。").arg(index + 1));
            return false;
        }
        sheet.name = safeSheetName(sheet.name, index);
        QString uniqueName = sheet.name;
        int suffix = 2;
        while (names.contains(uniqueName.toLower())) {
            const QString tail = QStringLiteral("-%1").arg(suffix++);
            uniqueName = sheet.name.left(31 - tail.size()) + tail;
        }
        sheet.name = uniqueName;
        names.insert(uniqueName.toLower());
    }

    const QByteArray styles = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><numFmts count="1"><numFmt numFmtId="164" formatCode="yyyy-mm-dd hh:mm:ss"/></numFmts><fonts count="2"><font><sz val="10"/><name val="Microsoft YaHei"/></font><font><b/><color rgb="FFFFFFFF"/><sz val="10"/><name val="Microsoft YaHei"/></font></fonts><fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill><fill><patternFill patternType="solid"><fgColor rgb="FF1F4E78"/><bgColor indexed="64"/></patternFill></fill></fills><borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders><cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs><cellXfs count="4"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf><xf numFmtId="14" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/><xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/></cellXfs></styleSheet>)";
    QStringList sheetNames;
    for (const auto &sheet : normalizedSheets) sheetNames.append(sheet.name);
    return packageWorkbook(filePath, sheetNames, styles, {},
        [&](int index, const QString &path, QString *error) {
            return writeWorksheet(path, normalizedSheets.at(index), error);
        }, errorMessage);
}

bool XlsxExporter::writeForm(const QString &filePath, const QString &sheetName,
                             const QByteArray &worksheet, const QByteArray &styles,
                             const QMap<QString, QString> &printNames, QString *errorMessage)
{
    return packageWorkbook(filePath, {sheetName}, styles, printNames,
        [&](int, const QString &path, QString *error) {
            return writeTextFile(path, worksheet, error);
        }, errorMessage);
}
