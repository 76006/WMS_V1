#include "import/LegacyInventoryImporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QDateTime>
#include <QXmlStreamReader>

namespace {
using SheetCells = QMap<int, QMap<int, QString>>;

struct WorkbookSheet
{
    QString name;
    SheetCells cells;
};

void setImportError(QString *target, const QString &message)
{
    if (target) *target = message;
}

QString databaseText(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isNull() ? QString::fromLatin1("", 0) : trimmed;
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

QString powerShellLiteral(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

bool extractArchive(const QString &filePath, const QString &destination,
                    QString *errorMessage)
{
    const QString readableArchive = QDir(destination).filePath(QStringLiteral("__source.xlsx"));
    if (!QFile::copy(filePath, readableArchive)) {
        setImportError(errorMessage, QStringLiteral("无法读取Office文件，请检查文件是否存在或有读取权限。"));
        return false;
    }
    QProcess unzip;
    const QString script = QStringLiteral(
        "Add-Type -AssemblyName System.IO.Compression.FileSystem; "
        "[IO.Compression.ZipFile]::ExtractToDirectory(%1,%2)")
                               .arg(powerShellLiteral(readableArchive),
                                    powerShellLiteral(destination));
    unzip.start(QStringLiteral("powershell.exe"),
                {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                 QStringLiteral("-Command"), script});
    if (!unzip.waitForFinished(30000) || unzip.exitCode() != 0) {
        setImportError(errorMessage, QStringLiteral("无法解压Office文件：%1")
                                       .arg(QString::fromLocal8Bit(unzip.readAllStandardError())));
        return false;
    }
    return true;
}

bool loadWorkbook(const QString &filePath, QList<WorkbookSheet> *sheets,
                  QString *errorMessage)
{
    if (!sheets) {
        setImportError(errorMessage, QStringLiteral("工作表容器无效。"));
        return false;
    }
    sheets->clear();
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
    if (!extractArchive(info.absoluteFilePath(), temporary.path(), errorMessage)) return false;

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
    if (relXml.hasError()) {
        setImportError(errorMessage, QStringLiteral("Excel工作簿关系格式错误。"));
        return false;
    }

    QList<QPair<QString, QString>> sheetFiles;
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
        sheetFiles.append({sheetName, temporary.filePath(target)});
    }
    if (workbookXml.hasError() || sheetFiles.isEmpty()) {
        setImportError(errorMessage, QStringLiteral("Excel工作簿中没有可读取的工作表。"));
        return false;
    }
    for (const auto &sheetFile : std::as_const(sheetFiles)) {
        WorkbookSheet sheet;
        sheet.name = sheetFile.first;
        if (!loadSheet(sheetFile.second, sharedStrings, &sheet.cells, errorMessage)) return false;
        sheets->append(sheet);
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

void appendMaterialMessage(MaterialImportRow *row, MaterialImportStatus status,
                           const QString &message)
{
    if (!row) return;
    if (row->message == QStringLiteral("可导入")) row->message.clear();
    if (status == MaterialImportStatus::Error || row->status != MaterialImportStatus::Error)
        row->status = status;
    if (!row->message.isEmpty()) row->message += QStringLiteral("；");
    row->message += message;
}

bool parseBoolean(const QString &raw, bool *value)
{
    const QString normalized = raw.trimmed().toUpper();
    if (normalized.isEmpty() || normalized == QStringLiteral("否") || normalized == QStringLiteral("0")
        || normalized == QStringLiteral("N") || normalized == QStringLiteral("NO")
        || normalized == QStringLiteral("FALSE")) {
        if (value) *value = false;
        return true;
    }
    if (normalized == QStringLiteral("是") || normalized == QStringLiteral("1")
        || normalized == QStringLiteral("Y") || normalized == QStringLiteral("YES")
        || normalized == QStringLiteral("TRUE")) {
        if (value) *value = true;
        return true;
    }
    return false;
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
    QList<WorkbookSheet> sheets;
    if (!loadWorkbook(filePath, &sheets, errorMessage)) return false;
    for (const WorkbookSheet &sheet : std::as_const(sheets)) {
        if (sheet.name == QStringLiteral("主机原材料库存")) {
            parseStandardSheet(sheet.name, sheet.cells, 7, QStringLiteral("RAW"), rows);
        } else if (sheet.name == QStringLiteral("主机耗材库存")) {
            parseStandardSheet(sheet.name, sheet.cells, 6, QStringLiteral("CONSUMABLE"), rows);
        } else if (sheet.name == QStringLiteral("头端原材料库存")) {
            parseStandardSheet(sheet.name, sheet.cells, 6, QStringLiteral("RAW"), rows);
        } else if (sheet.name == QStringLiteral("核心件")) {
            parseStandardSheet(sheet.name, sheet.cells, 7, QStringLiteral("SEMI"), rows);
        } else if (sheet.name == QStringLiteral("成品")) {
            parseFinishedSheet(sheet.name, sheet.cells, rows);
        } else if (sheet.name == QStringLiteral("期初库存")) {
            parseInitialTemplate(sheet.name, sheet.cells, rows);
        } else if (sheet.name == QStringLiteral("Sheet1")) {
            LegacyImportRow warning;
            warning.status = LegacyImportStatus::Warning;
            warning.sourceSheet = sheet.name;
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

bool MaterialExcelImporter::parseFile(const QString &filePath,
                                      QList<MaterialImportRow> *rows,
                                      QString *errorMessage)
{
    if (!rows) {
        setImportError(errorMessage, QStringLiteral("物料导入结果容器无效。"));
        return false;
    }
    rows->clear();
    QList<WorkbookSheet> sheets;
    if (!loadWorkbook(filePath, &sheets, errorMessage)) return false;
    const WorkbookSheet *materialSheet = nullptr;
    for (const WorkbookSheet &sheet : std::as_const(sheets)) {
        if (sheet.name == QStringLiteral("物料导入")) {
            materialSheet = &sheet;
            break;
        }
    }
    if (!materialSheet) {
        setImportError(errorMessage, QStringLiteral("Excel中缺少“物料导入”工作表。"));
        return false;
    }
    const QStringList requiredHeaders = {QStringLiteral("物料编码"), QStringLiteral("物料名称"),
        QStringLiteral("规格"), QStringLiteral("分类编码"), QStringLiteral("单位"),
        QStringLiteral("最低库存"), QStringLiteral("默认仓库编码"), QStringLiteral("默认库位编码"),
        QStringLiteral("批次管理"), QStringLiteral("SN管理")};
    for (int column = 1; column <= requiredHeaders.size(); ++column) {
        if (cell(materialSheet->cells, 1, column) != requiredHeaders.at(column - 1)) {
            setImportError(errorMessage, QStringLiteral("物料导入表第 %1 列应为“%2”。")
                                           .arg(column).arg(requiredHeaders.at(column - 1)));
            return false;
        }
    }

    QMap<QString, int> codeRows;
    const int lastRow = materialSheet->cells.isEmpty() ? 0 : materialSheet->cells.lastKey();
    for (int sourceRow = 2; sourceRow <= lastRow; ++sourceRow) {
        bool hasValue = false;
        for (int column = 1; column <= 12; ++column) {
            if (!cell(materialSheet->cells, sourceRow, column).isEmpty()) {
                hasValue = true;
                break;
            }
        }
        if (!hasValue) continue;
        MaterialImportRow row;
        row.sourceRow = sourceRow;
        row.materialCode = cell(materialSheet->cells, sourceRow, 1).toUpper();
        row.materialName = cell(materialSheet->cells, sourceRow, 2);
        row.specification = cell(materialSheet->cells, sourceRow, 3);
        row.categoryCode = cell(materialSheet->cells, sourceRow, 4).toUpper();
        row.unit = cell(materialSheet->cells, sourceRow, 5);
        row.defaultWarehouseCode = cell(materialSheet->cells, sourceRow, 7).toUpper();
        row.defaultLocationCode = cell(materialSheet->cells, sourceRow, 8).toUpper();
        row.brand = cell(materialSheet->cells, sourceRow, 11);
        row.notes = cell(materialSheet->cells, sourceRow, 12);
        if (row.materialCode.isEmpty()) appendMaterialMessage(&row, MaterialImportStatus::Error,
                                                               QStringLiteral("缺少物料编码"));
        if (row.materialName.isEmpty()) appendMaterialMessage(&row, MaterialImportStatus::Error,
                                                               QStringLiteral("缺少物料名称"));
        if (row.categoryCode.isEmpty()) appendMaterialMessage(&row, MaterialImportStatus::Error,
                                                               QStringLiteral("缺少分类编码"));
        if (row.unit.isEmpty()) appendMaterialMessage(&row, MaterialImportStatus::Error,
                                                       QStringLiteral("缺少单位"));
        const QString minimumText = cell(materialSheet->cells, sourceRow, 6);
        if (!minimumText.isEmpty()) {
            bool ok = false;
            row.minimumStock = minimumText.toDouble(&ok);
            if (!ok || row.minimumStock < 0.0)
                appendMaterialMessage(&row, MaterialImportStatus::Error,
                                      QStringLiteral("最低库存必须是大于等于0的数字"));
        }
        if (!parseBoolean(cell(materialSheet->cells, sourceRow, 9), &row.requireBatch))
            appendMaterialMessage(&row, MaterialImportStatus::Error,
                                  QStringLiteral("批次管理只能填写是或否"));
        if (!parseBoolean(cell(materialSheet->cells, sourceRow, 10), &row.requireSerial))
            appendMaterialMessage(&row, MaterialImportStatus::Error,
                                  QStringLiteral("SN管理只能填写是或否"));
        if (row.defaultWarehouseCode.isEmpty() != row.defaultLocationCode.isEmpty())
            appendMaterialMessage(&row, MaterialImportStatus::Error,
                                  QStringLiteral("默认仓库和默认库位必须同时填写或同时留空"));
        if (!row.materialCode.isEmpty() && codeRows.contains(row.materialCode)) {
            appendMaterialMessage(&row, MaterialImportStatus::Error,
                                  QStringLiteral("文件内物料编码重复"));
            appendMaterialMessage(&(*rows)[codeRows.value(row.materialCode)], MaterialImportStatus::Error,
                                  QStringLiteral("文件内物料编码重复"));
        } else if (!row.materialCode.isEmpty()) {
            codeRows.insert(row.materialCode, rows->size());
        }
        if (row.message.isEmpty()) row.message = QStringLiteral("可导入");
        rows->append(row);
    }
    if (rows->isEmpty()) {
        setImportError(errorMessage, QStringLiteral("物料导入表中没有数据。"));
        return false;
    }
    return true;
}

void MaterialExcelImporter::validateReferences(QSqlDatabase database,
                                               QList<MaterialImportRow> *rows)
{
    if (!database.isOpen() || !rows) return;
    for (MaterialImportRow &row : *rows) {
        if (row.status == MaterialImportStatus::Error) continue;
        QSqlQuery category(database);
        category.prepare(QStringLiteral(
            "SELECT id FROM material_categories WHERE code=? AND is_active=1"));
        category.addBindValue(row.categoryCode);
        if (!category.exec() || !category.next()) {
            appendMaterialMessage(&row, MaterialImportStatus::Error,
                                  QStringLiteral("分类编码不存在或已停用"));
            continue;
        }
        if (!row.defaultWarehouseCode.isEmpty()) {
            QSqlQuery location(database);
            location.prepare(QStringLiteral(
                "SELECT w.id,l.id FROM warehouses w JOIN locations l ON l.warehouse_id=w.id "
                "WHERE w.code=? AND l.code=? AND w.is_active=1 AND l.is_active=1"));
            location.addBindValue(row.defaultWarehouseCode);
            location.addBindValue(row.defaultLocationCode);
            if (!location.exec() || !location.next()) {
                appendMaterialMessage(&row, MaterialImportStatus::Error,
                                      QStringLiteral("默认仓库或库位不存在、已停用或不匹配"));
                continue;
            }
        }
        QSqlQuery existing(database);
        existing.prepare(QStringLiteral(
            "SELECT id,require_batch,require_serial FROM materials WHERE code=?"));
        existing.addBindValue(row.materialCode);
        if (existing.exec() && existing.next()) {
            const qlonglong materialId = existing.value(0).toLongLong();
            if (existing.value(1).toBool() != row.requireBatch
                || existing.value(2).toBool() != row.requireSerial) {
                QSqlQuery used(database);
                used.prepare(QStringLiteral(
                    "SELECT EXISTS(SELECT 1 FROM business_document_items WHERE material_id=?)"));
                used.addBindValue(materialId);
                if (used.exec() && used.next() && used.value(0).toBool()) {
                    appendMaterialMessage(&row, MaterialImportStatus::Error,
                        QStringLiteral("已有业务记录，不能通过Excel修改批次或SN管理方式"));
                    continue;
                }
            }
            appendMaterialMessage(&row, MaterialImportStatus::Warning,
                                  QStringLiteral("物料已存在，将更新资料"));
        }
    }
}

bool MaterialExcelImporter::importRows(QSqlDatabase database,
                                       qlonglong operatorId,
                                       const QList<MaterialImportRow> &rows,
                                       int *createdCount,
                                       int *updatedCount,
                                       QString *errorMessage)
{
    if (!database.isOpen() || operatorId <= 0) {
        setImportError(errorMessage, QStringLiteral("数据库未连接或当前用户无效。"));
        return false;
    }
    if (createdCount) *createdCount = 0;
    if (updatedCount) *updatedCount = 0;
    int importable = 0;
    for (const MaterialImportRow &row : rows)
        if (row.status != MaterialImportStatus::Error) ++importable;
    if (importable == 0) {
        setImportError(errorMessage, QStringLiteral("没有可导入的物料记录。"));
        return false;
    }
    if (!database.transaction()) {
        setImportError(errorMessage, QStringLiteral("无法开始物料导入事务：%1")
                                       .arg(database.lastError().text()));
        return false;
    }
    int created = 0;
    int updated = 0;
    for (const MaterialImportRow &row : rows) {
        if (row.status == MaterialImportStatus::Error) continue;
        QSqlQuery category(database);
        category.prepare(QStringLiteral("SELECT id FROM material_categories WHERE code=? AND is_active=1"));
        category.addBindValue(row.categoryCode);
        if (!category.exec() || !category.next()) {
            setImportError(errorMessage, QStringLiteral("第%1行分类编码已发生变化，请重新预览。")
                                           .arg(row.sourceRow));
            database.rollback();
            return false;
        }
        QVariant warehouseId;
        QVariant locationId;
        if (!row.defaultWarehouseCode.isEmpty()) {
            QSqlQuery location(database);
            location.prepare(QStringLiteral(
                "SELECT w.id,l.id FROM warehouses w JOIN locations l ON l.warehouse_id=w.id "
                "WHERE w.code=? AND l.code=? AND w.is_active=1 AND l.is_active=1"));
            location.addBindValue(row.defaultWarehouseCode);
            location.addBindValue(row.defaultLocationCode);
            if (!location.exec() || !location.next()) {
                setImportError(errorMessage, QStringLiteral("第%1行仓库库位已发生变化，请重新预览。")
                                               .arg(row.sourceRow));
                database.rollback();
                return false;
            }
            warehouseId = location.value(0);
            locationId = location.value(1);
        }
        QSqlQuery existing(database);
        existing.prepare(QStringLiteral("SELECT id FROM materials WHERE code=?"));
        existing.addBindValue(row.materialCode);
        const bool exists = existing.exec() && existing.next();
        const qlonglong materialId = exists ? existing.value(0).toLongLong() : 0;
        QSqlQuery save(database);
        if (exists) {
            save.prepare(QStringLiteral(
                "UPDATE materials SET name=?,specification=?,category_id=?,brand=?,unit=?,minimum_stock=?,"
                "default_warehouse_id=?,default_location_id=?,require_batch=?,require_serial=?,notes=?,"
                "updated_at=? WHERE id=?"));
        } else {
            save.prepare(QStringLiteral(
                "INSERT INTO materials(code,name,specification,category_id,brand,unit,minimum_stock,"
                "default_warehouse_id,default_location_id,require_batch,require_serial,notes) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"));
            save.addBindValue(row.materialCode);
        }
        save.addBindValue(row.materialName);
        save.addBindValue(databaseText(row.specification));
        save.addBindValue(category.value(0));
        save.addBindValue(databaseText(row.brand));
        save.addBindValue(row.unit);
        save.addBindValue(row.minimumStock);
        save.addBindValue(warehouseId);
        save.addBindValue(locationId);
        save.addBindValue(row.requireBatch);
        save.addBindValue(row.requireSerial);
        save.addBindValue(databaseText(row.notes));
        if (exists) {
            save.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
            save.addBindValue(materialId);
        }
        if (!save.exec()) {
            setImportError(errorMessage, QStringLiteral("第%1行物料保存失败：%2")
                                           .arg(row.sourceRow).arg(save.lastError().text()));
            database.rollback();
            return false;
        }
        const qlonglong savedId = exists ? materialId : save.lastInsertId().toLongLong();
        QSqlQuery audit(database);
        audit.prepare(QStringLiteral(
            "INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) VALUES(?,?,?,?,?)"));
        audit.addBindValue(operatorId);
        audit.addBindValue(exists ? QStringLiteral("MATERIAL_IMPORT_UPDATE")
                                  : QStringLiteral("MATERIAL_IMPORT_CREATE"));
        audit.addBindValue(QStringLiteral("material"));
        audit.addBindValue(savedId);
        audit.addBindValue(row.materialCode + QStringLiteral(" - ") + row.materialName);
        if (!audit.exec()) {
            setImportError(errorMessage, audit.lastError().text());
            database.rollback();
            return false;
        }
        if (exists) ++updated;
        else ++created;
    }
    if (!database.commit()) {
        setImportError(errorMessage, QStringLiteral("提交物料导入失败：%1").arg(database.lastError().text()));
        database.rollback();
        return false;
    }
    if (createdCount) *createdCount = created;
    if (updatedCount) *updatedCount = updated;
    return true;
}

QString MaterialExcelImporter::statusText(MaterialImportStatus status)
{
    switch (status) {
    case MaterialImportStatus::Ready: return QStringLiteral("可导入");
    case MaterialImportStatus::Warning: return QStringLiteral("警告");
    case MaterialImportStatus::Error: return QStringLiteral("错误");
    }
    return {};
}

bool OfficePreviewExtractor::previewXlsx(const QString &filePath,
                                         QList<SpreadsheetPreviewSheet> *sheets,
                                         QString *errorMessage)
{
    if (!sheets) {
        setImportError(errorMessage, QStringLiteral("Excel预览结果容器无效。"));
        return false;
    }
    sheets->clear();
    QList<WorkbookSheet> workbookSheets;
    if (!loadWorkbook(filePath, &workbookSheets, errorMessage)) return false;
    constexpr int MaximumPreviewRows = 200;
    constexpr int MaximumPreviewColumns = 30;
    for (const WorkbookSheet &source : std::as_const(workbookSheets)) {
        SpreadsheetPreviewSheet preview;
        preview.name = source.name;
        const int lastRow = source.cells.isEmpty()
            ? 0 : qMin(source.cells.lastKey(), MaximumPreviewRows);
        int lastColumn = 0;
        for (auto iterator = source.cells.cbegin(); iterator != source.cells.cend(); ++iterator) {
            if (!iterator.value().isEmpty())
                lastColumn = qMax(lastColumn, iterator.value().lastKey());
        }
        lastColumn = qMin(lastColumn, MaximumPreviewColumns);
        for (int row = 1; row <= lastRow; ++row) {
            QStringList values;
            for (int column = 1; column <= lastColumn; ++column)
                values.append(cell(source.cells, row, column));
            preview.rows.append(values);
        }
        sheets->append(preview);
    }
    return true;
}

bool OfficePreviewExtractor::previewDocx(const QString &filePath,
                                         QString *text,
                                         QString *errorMessage)
{
    if (!text) {
        setImportError(errorMessage, QStringLiteral("Word预览结果容器无效。"));
        return false;
    }
    text->clear();
    const QFileInfo info(filePath);
    if (!info.isFile() || info.suffix().compare(QStringLiteral("docx"), Qt::CaseInsensitive) != 0) {
        setImportError(errorMessage, QStringLiteral("请选择有效的 .docx 文件。"));
        return false;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid() || !extractArchive(info.absoluteFilePath(), temporary.path(), errorMessage))
        return false;
    QFile document(temporary.filePath(QStringLiteral("word/document.xml")));
    if (!document.open(QIODevice::ReadOnly)) {
        setImportError(errorMessage, QStringLiteral("Word文件缺少正文内容。"));
        return false;
    }
    QXmlStreamReader xml(&document);
    QString result;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("t")) {
            result += xml.readElementText();
        } else if (xml.isStartElement() && xml.name() == QStringLiteral("tab")) {
            result += QLatin1Char('\t');
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("p")) {
            result += QLatin1Char('\n');
        }
    }
    if (xml.hasError()) {
        setImportError(errorMessage, QStringLiteral("Word正文格式错误：%1").arg(xml.errorString()));
        return false;
    }
    *text = result.trimmed();
    return true;
}
