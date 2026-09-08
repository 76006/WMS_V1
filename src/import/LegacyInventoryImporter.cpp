#include "import/LegacyInventoryImporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QXmlStreamReader>

namespace {
using SheetCells = QMap<int, QMap<int, QString>>;

void setImportError(QString *target, const QString &message)
{
    if (target) *target = message;
}

int columnNumber(const QString &cellReference)
{
    int value = 0;
    for (const QChar character : cellReference) {
        if (!character.isLetter()) break;
        value = value * 26 + character.toUpper().unicode() - QLatin1Char('A').unicode() + 1;
    }
    return value;
}

int rowNumber(const QString &cellReference)
{
    static const QRegularExpression expression(QStringLiteral("(\\d+)$"));
    const QRegularExpressionMatch match = expression.match(cellReference);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QString attributeValue(const QXmlStreamAttributes &attributes, const QString &name)
{
    for (const QXmlStreamAttribute &attribute : attributes) {
        if (attribute.name() == name || attribute.qualifiedName() == name) {
            return attribute.value().toString();
        }
    }
    return {};
}

bool loadSharedStrings(const QString &path, QStringList *values, QString *errorMessage)
{
    QFile file(path);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) {
        setImportError(errorMessage, QStringLiteral("无法读取Excel共享文本。"));
        return false;
    }
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QStringLiteral("si")) continue;
        QString text;
        while (!(xml.isEndElement() && xml.name() == QStringLiteral("si")) && !xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QStringLiteral("t")) {
                text += xml.readElementText();
            }
        }
        values->append(text);
    }
    if (xml.hasError()) {
        setImportError(errorMessage, QStringLiteral("Excel共享文本格式错误：%1").arg(xml.errorString()));
        return false;
    }
    return true;
}

bool loadSheet(const QString &path, const QStringList &sharedStrings,
               SheetCells *cells, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setImportError(errorMessage, QStringLiteral("无法读取Excel工作表：%1").arg(path));
        return false;
    }
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QStringLiteral("c")) continue;
        const QString reference = attributeValue(xml.attributes(), QStringLiteral("r"));
        const QString type = attributeValue(xml.attributes(), QStringLiteral("t"));
        QString rawValue;
        QString inlineValue;
        while (!(xml.isEndElement() && xml.name() == QStringLiteral("c")) && !xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QStringLiteral("v")) {
                rawValue = xml.readElementText();
            } else if (xml.isStartElement() && xml.name() == QStringLiteral("t")) {
                inlineValue += xml.readElementText();
            }
        }
        QString value = rawValue;
        if (type == QStringLiteral("s")) {
            bool ok = false;
            const int index = rawValue.toInt(&ok);
            value = ok && index >= 0 && index < sharedStrings.size()
                ? sharedStrings.at(index) : QString();
        } else if (type == QStringLiteral("inlineStr")) {
            value = inlineValue;
        }
        const int row = rowNumber(reference);
        const int column = columnNumber(reference);
        if (row > 0 && column > 0) (*cells)[row][column] = value.trimmed();
    }
    if (xml.hasError()) {
        setImportError(errorMessage, QStringLiteral("Excel工作表格式错误：%1").arg(xml.errorString()));
        return false;
    }
    return true;
}

QString cell(const SheetCells &cells, int row, int column)
{
    return cells.value(row).value(column).trimmed();
}

LegacyImportRow makeRow(const QString &sheet, int row, const QString &code,
                        const QString &name, const QString &specification,
                        const QString &category, const QString &batch,
                        const QString &rawQuantity)
{
    LegacyImportRow result;
    result.sourceSheet = sheet;
    result.sourceRow = row;
    result.materialCode = code.trimmed().toUpper();
    result.materialName = name.trimmed();
    result.specification = specification.trimmed();
    result.categoryCode = category;
    result.batchNo = batch.trimmed();
    result.rawQuantity = rawQuantity.trimmed();

    if (result.materialCode.isEmpty() || result.materialCode == QStringLiteral("/")) {
        result.status = LegacyImportStatus::Error;
        result.message = QStringLiteral("缺少有效物料编码，不能导入");
        return result;
    }
    if (result.materialName.isEmpty()) {
        result.status = LegacyImportStatus::Error;
        result.message = QStringLiteral("缺少物料名称，不能导入");
        return result;
    }
    if (result.rawQuantity.isEmpty() || result.rawQuantity == QStringLiteral("/")) {
        result.status = LegacyImportStatus::Warning;
        result.message = QStringLiteral("库存数量为空或为斜杠，已跳过");
        return result;
    }
    bool ok = false;
    result.quantity = result.rawQuantity.toDouble(&ok);
    if (!ok) {
        result.status = LegacyImportStatus::Error;
        result.message = QStringLiteral("库存数量不是有效数字");
    } else if (result.quantity < 0.0) {
        result.status = LegacyImportStatus::Error;
        result.message = QStringLiteral("系统不允许负库存，请先核对历史欠料");
    } else if (qFuzzyIsNull(result.quantity)) {
        result.status = LegacyImportStatus::Skipped;
        result.message = QStringLiteral("零库存，无需导入");
    } else {
        result.status = LegacyImportStatus::Ready;
        result.message = QStringLiteral("可导入");
    }
    return result;
}

void parseStandardSheet(const QString &name, const SheetCells &cells,
                        int quantityColumn, const QString &category,
                        QList<LegacyImportRow> *rows)
{
    if (cells.isEmpty()) return;
    const int lastRow = cells.lastKey();
    for (int row = 3; row <= lastRow; ++row) {
        const QString code = cell(cells, row, 2);
        const QString materialName = cell(cells, row, 3);
        const QString quantity = cell(cells, row, quantityColumn);
        if (code.isEmpty() && materialName.isEmpty() && quantity.isEmpty()) continue;
        rows->append(makeRow(name, row, code, materialName, cell(cells, row, 4),
                             category, QString(), quantity));
    }
}

void parseInitialTemplate(const QString &name, const SheetCells &cells,
                          QList<LegacyImportRow> *rows)
{
    const int lastRow = cells.isEmpty() ? 0 : cells.lastKey();
    for (int row = 2; row <= lastRow; ++row) {
        if (cell(cells, row, 1).isEmpty() && cell(cells, row, 2).isEmpty()) continue;
        LegacyImportRow item = makeRow(name, row, cell(cells, row, 1), cell(cells, row, 2),
                                       cell(cells, row, 3),
                                       cell(cells, row, 4).isEmpty() ? QStringLiteral("RAW")
                                                                    : cell(cells, row, 4),
                                       cell(cells, row, 6), cell(cells, row, 7));
        if (!cell(cells, row, 5).isEmpty()) item.unit = cell(cells, row, 5);
        rows->append(item);
    }
}

void parseFinishedSheet(const QString &name, const SheetCells &cells,
                        QList<LegacyImportRow> *rows)
{
    QMap<QString, QList<LegacyImportRow>> latestGroups;
    QString group = QStringLiteral("头端");
    QString previousCode;
    QString previousName;
    QString previousSpec;
    bool inBlock = false;
    const int lastRow = cells.isEmpty() ? 0 : cells.lastKey();
    for (int row = 1; row <= lastRow; ++row) {
        const QString title = cell(cells, row, 1);
        if (title.contains(QStringLiteral("头端"))) group = QStringLiteral("头端");
        else if (title.contains(QStringLiteral("主机"))) group = QStringLiteral("主机");
        else if (title.contains(QStringLiteral("耗材"))) group = QStringLiteral("耗材");

        if (cell(cells, row, 2).contains(QStringLiteral("物料编码"))) {
            latestGroups[group].clear();
            previousCode.clear();
            previousName.clear();
            previousSpec.clear();
            inBlock = true;
            continue;
        }
        if (!inBlock) continue;
        QString code = cell(cells, row, 2);
        QString materialName = cell(cells, row, 3);
        QString specification = cell(cells, row, 4);
        const QString batch = cell(cells, row, 5);
        const QString quantity = cell(cells, row, 6);
        if (batch.contains(QStringLiteral("库存合计"))
            || materialName.contains(QStringLiteral("库存合计"))) continue;
        if (!code.isEmpty() && code != QStringLiteral("/")) {
            previousCode = code;
            previousName = materialName;
            previousSpec = specification;
        } else if (code.isEmpty() && (!batch.isEmpty() || !quantity.isEmpty())) {
            code = previousCode;
            materialName = previousName;
            specification = previousSpec;
        }
        if (code.isEmpty() && materialName.isEmpty() && batch.isEmpty() && quantity.isEmpty()) continue;
        const QString category = group == QStringLiteral("耗材")
            ? QStringLiteral("CONSUMABLE") : QStringLiteral("FINISHED");
        latestGroups[group].append(makeRow(name, row, code, materialName, specification,
                                            category, batch, quantity));
    }
    for (const QString &key : {QStringLiteral("头端"), QStringLiteral("主机"),
                               QStringLiteral("耗材")}) {
        rows->append(latestGroups.value(key));
    }
}

void aggregateReadyRows(QList<LegacyImportRow> *rows)
{
    QMap<QString, int> positions;
    QList<LegacyImportRow> result;
    for (const LegacyImportRow &row : std::as_const(*rows)) {
        if (row.status != LegacyImportStatus::Ready) {
            result.append(row);
            continue;
        }
        const QString key = row.materialCode.toUpper() + QLatin1Char('|') + row.batchNo.toUpper();
        if (!positions.contains(key)) {
            positions.insert(key, result.size());
            result.append(row);
        } else {
            LegacyImportRow &existing = result[positions.value(key)];
            existing.quantity += row.quantity;
            existing.rawQuantity = QString::number(existing.quantity, 'g', 15);
            existing.message = QStringLiteral("同物料同批次已合并");
        }
    }
    *rows = result;
}
}

bool LegacyInventoryImporter::parseFile(const QString &filePath,
                                        QList<LegacyImportRow> *rows,
                                        QString *errorMessage)
{
    if (!rows) {
        setImportError(errorMessage, QStringLiteral("导入结果容器无效。"));
        return false;
    }
    rows->clear();
    const QFileInfo info(filePath);
    if (!info.isFile() || info.suffix().compare(QStringLiteral("xlsx"), Qt::CaseInsensitive) != 0) {
        setImportError(errorMessage, QStringLiteral("请选择有效的 .xlsx 文件。"));
        return false;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        setImportError(errorMessage, QStringLiteral("无法创建Excel解析临时目录。"));
        return false;
    }
    QProcess unzip;
    auto powerShellLiteral = [](QString value) {
        value.replace(QLatin1Char('\''), QStringLiteral("''"));
        return QLatin1Char('\'') + value + QLatin1Char('\'');
    };
    const QString script = QStringLiteral(
        "Add-Type -AssemblyName System.IO.Compression.FileSystem; "
        "[IO.Compression.ZipFile]::ExtractToDirectory(%1,%2)")
                               .arg(powerShellLiteral(info.absoluteFilePath()),
                                    powerShellLiteral(temporary.path()));
    unzip.start(QStringLiteral("powershell.exe"),
                {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                 QStringLiteral("-Command"), script});
    if (!unzip.waitForFinished(30000) || unzip.exitCode() != 0) {
        setImportError(errorMessage, QStringLiteral("无法解压Excel文件：%1")
                                       .arg(QString::fromLocal8Bit(unzip.readAllStandardError())));
        return false;
    }

    QStringList sharedStrings;
    if (!loadSharedStrings(temporary.filePath(QStringLiteral("xl/sharedStrings.xml")),
                           &sharedStrings, errorMessage)) return false;

    QMap<QString, QString> relationshipTargets;
    QFile relationships(temporary.filePath(QStringLiteral("xl/_rels/workbook.xml.rels")));
    if (!relationships.open(QIODevice::ReadOnly)) {
        setImportError(errorMessage, QStringLiteral("Excel缺少工作簿关系定义。"));
        return false;
    }
    QXmlStreamReader relXml(&relationships);
    while (!relXml.atEnd()) {
        relXml.readNext();
        if (relXml.isStartElement() && relXml.name() == QStringLiteral("Relationship")) {
            relationshipTargets.insert(attributeValue(relXml.attributes(), QStringLiteral("Id")),
                                       attributeValue(relXml.attributes(), QStringLiteral("Target")));
        }
    }

    QList<QPair<QString, QString>> sheets;
    QFile workbook(temporary.filePath(QStringLiteral("xl/workbook.xml")));
    if (!workbook.open(QIODevice::ReadOnly)) {
        setImportError(errorMessage, QStringLiteral("Excel缺少工作簿定义。"));
        return false;
    }
    QXmlStreamReader workbookXml(&workbook);
    while (!workbookXml.atEnd()) {
        workbookXml.readNext();
        if (!workbookXml.isStartElement() || workbookXml.name() != QStringLiteral("sheet")) continue;
        const QString sheetName = attributeValue(workbookXml.attributes(), QStringLiteral("name"));
        const QString relationId = attributeValue(workbookXml.attributes(), QStringLiteral("r:id"));
        QString target = relationshipTargets.value(relationId).replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (target.startsWith(QLatin1Char('/'))) target.remove(0, 1);
        else if (!target.startsWith(QStringLiteral("xl/"))) target.prepend(QStringLiteral("xl/"));
        sheets.append({sheetName, temporary.filePath(target)});
    }
    if (workbookXml.hasError() || sheets.isEmpty()) {
        setImportError(errorMessage, QStringLiteral("Excel工作簿中没有可读取的工作表。"));
        return false;
    }

    for (const auto &sheet : std::as_const(sheets)) {
        SheetCells cells;
        if (!loadSheet(sheet.second, sharedStrings, &cells, errorMessage)) return false;
        if (sheet.first == QStringLiteral("主机原材料库存")) {
            parseStandardSheet(sheet.first, cells, 7, QStringLiteral("RAW"), rows);
        } else if (sheet.first == QStringLiteral("主机耗材库存")) {
            parseStandardSheet(sheet.first, cells, 6, QStringLiteral("CONSUMABLE"), rows);
        } else if (sheet.first == QStringLiteral("头端原材料库存")) {
            parseStandardSheet(sheet.first, cells, 6, QStringLiteral("RAW"), rows);
        } else if (sheet.first == QStringLiteral("核心件")) {
            parseStandardSheet(sheet.first, cells, 7, QStringLiteral("SEMI"), rows);
        } else if (sheet.first == QStringLiteral("成品")) {
            parseFinishedSheet(sheet.first, cells, rows);
        } else if (sheet.first == QStringLiteral("期初库存")) {
            parseInitialTemplate(sheet.first, cells, rows);
        } else if (sheet.first == QStringLiteral("Sheet1")) {
            LegacyImportRow warning;
            warning.status = LegacyImportStatus::Warning;
            warning.sourceSheet = sheet.first;
            warning.message = QStringLiteral("此表为外借/异地记录且没有标准物料编码，未纳入期初库存");
            rows->append(warning);
        }
    }
    aggregateReadyRows(rows);
    return true;
}

QString LegacyInventoryImporter::statusText(LegacyImportStatus status)
{
    switch (status) {
    case LegacyImportStatus::Ready: return QStringLiteral("可导入");
    case LegacyImportStatus::Warning: return QStringLiteral("警告");
    case LegacyImportStatus::Error: return QStringLiteral("错误");
    case LegacyImportStatus::Skipped: return QStringLiteral("跳过");
    }
    return {};
}
