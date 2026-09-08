#include "import/XlsxExporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QXmlStreamWriter>

namespace {
void setError(QString *target, const QString &message) { if (target) *target = message; }

bool writeTextFile(const QString &path, const QByteArray &data, QString *errorMessage)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(data) != data.size()) {
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

QString psLiteral(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}
}

bool XlsxExporter::writeSingleSheet(const QString &filePath, const QString &sheetName,
                                    const QStringList &headers,
                                    const QList<QList<QVariant>> &rows,
                                    QString *errorMessage)
{
    if (filePath.trimmed().isEmpty() || headers.isEmpty()) {
        setError(errorMessage, QStringLiteral("Excel文件路径或表头不能为空。"));
        return false;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        setError(errorMessage, QStringLiteral("无法创建Excel导出临时目录。"));
        return false;
    }
    const QByteArray contentTypes = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/></Types>)";
    const QByteArray rootRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)";
    const QByteArray workbookRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>)";
    const QByteArray styles = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><fonts count="2"><font><sz val="11"/><name val="Microsoft YaHei"/></font><font><b/><color rgb="FFFFFFFF"/><sz val="11"/><name val="Microsoft YaHei"/></font></fonts><fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill><fill><patternFill patternType="solid"><fgColor rgb="FF1F4E78"/><bgColor indexed="64"/></patternFill></fill></fills><borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders><cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs><cellXfs count="2"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1"/></cellXfs></styleSheet>)";
    if (!writeTextFile(temporary.filePath(QStringLiteral("[Content_Types].xml")), contentTypes, errorMessage)
        || !writeTextFile(temporary.filePath(QStringLiteral("_rels/.rels")), rootRels, errorMessage)
        || !writeTextFile(temporary.filePath(QStringLiteral("xl/_rels/workbook.xml.rels")), workbookRels, errorMessage)
        || !writeTextFile(temporary.filePath(QStringLiteral("xl/styles.xml")), styles, errorMessage)) return false;

    QByteArray workbookData;
    QXmlStreamWriter workbook(&workbookData);
    workbook.writeStartDocument();
    workbook.writeStartElement(QStringLiteral("workbook"));
    workbook.writeDefaultNamespace(QStringLiteral("http://schemas.openxmlformats.org/spreadsheetml/2006/main"));
    workbook.writeNamespace(QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"), QStringLiteral("r"));
    workbook.writeStartElement(QStringLiteral("sheets"));
    workbook.writeStartElement(QStringLiteral("sheet"));
    workbook.writeAttribute(QStringLiteral("name"), sheetName.left(31));
    workbook.writeAttribute(QStringLiteral("sheetId"), QStringLiteral("1"));
    workbook.writeAttribute(QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"), QStringLiteral("id"), QStringLiteral("rId1"));
    workbook.writeEndElement(); workbook.writeEndElement(); workbook.writeEndElement(); workbook.writeEndDocument();
    if (!writeTextFile(temporary.filePath(QStringLiteral("xl/workbook.xml")), workbookData, errorMessage)) return false;

    QByteArray sheetData;
    QXmlStreamWriter xml(&sheetData);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("worksheet"));
    xml.writeDefaultNamespace(QStringLiteral("http://schemas.openxmlformats.org/spreadsheetml/2006/main"));
    xml.writeStartElement(QStringLiteral("sheetViews"));
    xml.writeStartElement(QStringLiteral("sheetView")); xml.writeAttribute(QStringLiteral("workbookViewId"), QStringLiteral("0"));
    xml.writeStartElement(QStringLiteral("pane")); xml.writeAttribute(QStringLiteral("ySplit"), QStringLiteral("1")); xml.writeAttribute(QStringLiteral("topLeftCell"), QStringLiteral("A2")); xml.writeAttribute(QStringLiteral("state"), QStringLiteral("frozen")); xml.writeEndElement();
    xml.writeEndElement(); xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("cols"));
    for (int column = 1; column <= headers.size(); ++column) {
        xml.writeStartElement(QStringLiteral("col")); xml.writeAttribute(QStringLiteral("min"), QString::number(column)); xml.writeAttribute(QStringLiteral("max"), QString::number(column)); xml.writeAttribute(QStringLiteral("width"), column <= 2 ? QStringLiteral("20") : QStringLiteral("16")); xml.writeAttribute(QStringLiteral("customWidth"), QStringLiteral("1")); xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("sheetData"));
    auto writeRow = [&](int rowNumber, const QList<QVariant> &values, bool header) {
        xml.writeStartElement(QStringLiteral("row")); xml.writeAttribute(QStringLiteral("r"), QString::number(rowNumber));
        for (int index = 0; index < values.size(); ++index) {
            const QVariant &value = values.at(index);
            xml.writeStartElement(QStringLiteral("c")); xml.writeAttribute(QStringLiteral("r"), columnName(index + 1) + QString::number(rowNumber));
            if (header) xml.writeAttribute(QStringLiteral("s"), QStringLiteral("1"));
            const bool numeric = !header && (value.metaType().id() == QMetaType::Double || value.metaType().id() == QMetaType::Int || value.metaType().id() == QMetaType::LongLong);
            if (numeric) {
                xml.writeTextElement(QStringLiteral("v"), QString::number(value.toDouble(), 'g', 15));
            } else {
                xml.writeAttribute(QStringLiteral("t"), QStringLiteral("inlineStr"));
                xml.writeStartElement(QStringLiteral("is")); xml.writeTextElement(QStringLiteral("t"), value.toString()); xml.writeEndElement();
            }
            xml.writeEndElement();
        }
        xml.writeEndElement();
    };
    QList<QVariant> headerValues; for (const QString &header : headers) headerValues.append(header);
    writeRow(1, headerValues, true);
    for (int index = 0; index < rows.size(); ++index) writeRow(index + 2, rows.at(index), false);
    xml.writeEndElement(); xml.writeEndElement(); xml.writeEndDocument();
    if (!writeTextFile(temporary.filePath(QStringLiteral("xl/worksheets/sheet1.xml")), sheetData, errorMessage)) return false;

    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile::remove(filePath);
    QProcess zip;
    const QString script = QStringLiteral("Add-Type -AssemblyName System.IO.Compression.FileSystem; [IO.Compression.ZipFile]::CreateFromDirectory(%1,%2)")
                               .arg(psLiteral(temporary.path()), psLiteral(QFileInfo(filePath).absoluteFilePath()));
    zip.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"), script});
    if (!zip.waitForFinished(30000) || zip.exitCode() != 0) {
        setError(errorMessage, QStringLiteral("生成Excel文件失败：%1").arg(QString::fromLocal8Bit(zip.readAllStandardError())));
        return false;
    }
    return QFileInfo::exists(filePath);
}
