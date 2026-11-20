#include "import/JsonFormRenderer.h"
#include "import/XlsxExporter.h"

#include <QDate>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
const QString spreadsheetNamespace = QStringLiteral("http://schemas.openxmlformats.org/spreadsheetml/2006/main");

// These errors describe an editable configuration and are reported with its filename.
void require(bool condition, const QString &message)
{
    if (!condition) throw std::runtime_error(message.toUtf8().constData());
}

double number(const QJsonObject &object, const QString &key, double minimum, double maximum)
{
    const auto value = object.value(key);
    require(value.isDouble() && value.toDouble() >= minimum && value.toDouble() <= maximum,
            QStringLiteral("%1 必须是 %2 到 %3 之间的数值").arg(key).arg(minimum).arg(maximum));
    return value.toDouble();
}

int integer(const QJsonObject &object, const QString &key, int minimum, int maximum)
{
    const double value = number(object, key, minimum, maximum);
    require(std::floor(value) == value, key + QStringLiteral(" 必须是整数"));
    return static_cast<int>(value);
}

QString letters(int column)
{
    QString label;
    while (column > 0) {
        --column;
        label.prepend(QChar('A' + column % 26));
        column /= 26;
    }
    return label;
}

QString reference(int column, int row)
{
    return letters(column) + QString::number(row);
}

void valueElement(QXmlStreamWriter &xml, const QString &name, const QString &value)
{
    xml.writeEmptyElement(name);
    xml.writeAttribute("val", value);
}

struct Style {
    QString font, align, border, borderStyle;
    double size;
    bool bold, wrap;
};

class Styles {
public:
    explicit Styles(const QJsonObject &definitions)
    {
        require(definitions.contains("default"), QStringLiteral("缺少 styles.default"));
        const auto defaults = definitions.value("default").toObject();
        QStringList names = definitions.keys();
        names.removeAll("default");
        names.prepend("default"); // Excel uses the first font for column width units.
        for (const auto &name : names) {
            auto config = defaults;
            const auto custom = definitions.value(name).toObject();
            for (auto it = custom.begin(); it != custom.end(); ++it) config.insert(it.key(), it.value());
            Style style{config.value("font").toString("宋体"), config.value("align").toString("general"),
                        config.value("border").toString(), config.value("borderStyle").toString("thin"),
                        number(config, "size", 1, 409), config.value("bold").toBool(), config.value("wrap").toBool()};
            require(QStringList{"general","left","center","right"}.contains(style.align),
                    QStringLiteral("无效的对齐方式：") + style.align);
            require(QStringList{"thin","medium","thick","dashed","dotted","double"}.contains(style.borderStyle),
                    QStringLiteral("无效的边框线型：") + style.borderStyle);
            for (const auto &side : style.border.split(' ', Qt::SkipEmptyParts))
                require(QStringList{"all","left","right","top","bottom"}.contains(side), QStringLiteral("无效的边框：") + side);
            ids.insert(name, values.size());
            values.append(style);
        }
    }

    int id(const QString &name) const
    {
        require(ids.contains(name), QStringLiteral("找不到样式：") + name);
        return ids.value(name);
    }

    int mergedEdge(int base, const QStringList &edges)
    {
        Style style = values.at(base);
        if (style.border.isEmpty()) return base;
        const auto sides = style.border.split(' ', Qt::SkipEmptyParts);
        QStringList visible;
        for (const auto &edge : edges)
            if (sides.contains("all") || sides.contains(edge)) visible.append(edge);
        style.border = visible.join(' ');
        const auto key = qMakePair(base, style.border);
        if (edgeIds.contains(key)) return edgeIds.value(key);
        const int index = values.size();
        values.append(style);
        edgeIds.insert(key, index);
        return index;
    }

    QByteArray xmlData() const
    {
        QByteArray bytes;
        QXmlStreamWriter xml(&bytes);
        xml.writeStartDocument();
        xml.writeStartElement("styleSheet");
        xml.writeDefaultNamespace(spreadsheetNamespace);
        const QString count = QString::number(values.size());
        xml.writeStartElement("fonts");
        xml.writeAttribute("count", count);
        for (const auto &style : values) {
            xml.writeStartElement("font");
            if (style.bold) xml.writeEmptyElement("b");
            valueElement(xml, "sz", QString::number(style.size));
            valueElement(xml, "name", style.font);
            xml.writeEndElement();
        }
        xml.writeEndElement();
        xml.writeStartElement("fills");
        xml.writeAttribute("count", "2");
        for (const auto pattern : {"none", "gray125"}) {
            xml.writeStartElement("fill");
            xml.writeEmptyElement("patternFill");
            xml.writeAttribute("patternType", pattern);
            xml.writeEndElement();
        }
        xml.writeEndElement();
        xml.writeStartElement("borders");
        xml.writeAttribute("count", count);
        for (const auto &style : values) {
            xml.writeStartElement("border");
            const auto sides = style.border.split(' ', Qt::SkipEmptyParts);
            for (const auto side : {"left","right","top","bottom"}) {
                xml.writeEmptyElement(side);
                if (sides.contains("all") || sides.contains(side)) xml.writeAttribute("style", style.borderStyle);
            }
            xml.writeEndElement();
        }
        xml.writeEndElement();
        xml.writeStartElement("cellStyleXfs");
        xml.writeAttribute("count", "1");
        xml.writeEmptyElement("xf");
        xml.writeAttribute("numFmtId", "0");
        xml.writeAttribute("fontId", "0");
        xml.writeAttribute("fillId", "0");
        xml.writeAttribute("borderId", "0");
        xml.writeEndElement();
        xml.writeStartElement("cellXfs");
        xml.writeAttribute("count", count);
        for (int index = 0; index < values.size(); ++index) {
            const auto &style = values.at(index);
            xml.writeStartElement("xf");
            xml.writeAttribute("numFmtId", "0");
            xml.writeAttribute("fontId", QString::number(index));
            xml.writeAttribute("fillId", "0");
            xml.writeAttribute("borderId", QString::number(index));
            xml.writeAttribute("xfId", "0");
            xml.writeAttribute("applyFont", "1");
            xml.writeAttribute("applyBorder", "1");
            xml.writeAttribute("applyAlignment", "1");
            xml.writeEmptyElement("alignment");
            xml.writeAttribute("horizontal", style.align);
            xml.writeAttribute("vertical", "center");
            if (style.wrap) xml.writeAttribute("wrapText", "1");
            xml.writeEndElement();
        }
        xml.writeEndElement();
        xml.writeStartElement("cellStyles");
        xml.writeAttribute("count", "1");
        xml.writeEmptyElement("cellStyle");
        xml.writeAttribute("name","Normal");
        xml.writeAttribute("xfId","0");
        xml.writeAttribute("builtinId","0");
        xml.writeEndDocument();
        return bytes;
    }
    QList<Style> values;
private:
    QMap<QString, int> ids;
    QMap<QPair<int,QString>,int> edgeIds;
};

// The heading/signature grid and the detail columns may be edited independently.
// Their common boundaries become real Excel columns; each cell spans its own interval.
class Grid {
public:
    Grid(int layout, const QJsonArray &columns) : layoutUnits(layout)
    {
        QList<double> units;
        for (const auto &item : columns) {
            auto col = item.toObject();
            int span = col.contains("span") ? integer(col,"span",1,16384) : 1;
            double width = number(col,"width",0.1,255);
            require(units.size()+span <= 16384, QStringLiteral("明细列数超出 Excel 上限"));
            for (int i = 0; i < span; ++i) units.append(width/span);
        }
        detailUnits = units.size();
        require(detailUnits > 0, QStringLiteral("details.columns 至少需要一列"));
        QSet<int> cuts;
        for (int i = 0; i <= layoutUnits; ++i) cuts.insert(i*detailUnits);
        for (int i = 0; i <= detailUnits; ++i) cuts.insert(i*layoutUnits);
        auto points = cuts.values();
        std::sort(points.begin(), points.end());
        require(points.size()-1 <= 16384, QStringLiteral("版式列数超出 Excel 上限"));
        for (int i = 0; i < points.size(); ++i) {
            boundaries.insert(points[i], i);
            if (i+1 < points.size())
                widths.append(units.at(points[i]/layoutUnits)*(points[i+1]-points[i])/layoutUnits);
        }
    }
    int layoutEdge(int units) const { return boundaries.value(units*detailUnits); }
    int detailEdge(int units) const { return boundaries.value(units*layoutUnits); }
    int layoutUnits, detailUnits;
    QList<double> widths;
private:
    QMap<int,int> boundaries;
};

QJsonValue field(const QJsonObject &document, const QJsonObject &line, int index, const QString &key)
{
    if (key == "index") return index;
    if (key.startsWith("line.")) {
        const auto name = key.mid(5);
        require(line.contains(name), QStringLiteral("未知明细字段：") + key);
        return line.value(name);
    }
    if (key.startsWith("fields.")) return document.value("fields").toObject().value(key.mid(7));
    require(document.contains(key), QStringLiteral("未知单据字段：") + key);
    return document.value(key);
}

QJsonValue bandText(const QString &pattern, const QJsonObject &document)
{
    static const QRegularExpression tokens(QStringLiteral(R"(\{([\w.]+)(?::([^{}]+))?\})"));
    auto matches = tokens.globalMatch(pattern);
    QString result;
    qsizetype end = 0;
    while (matches.hasNext()) {
        auto match = matches.next();
        auto value = field(document, {}, 0, match.captured(1));
        if (match.capturedLength(2) > 0)
            value = QDate::fromString(value.toString(), Qt::ISODate).toString(match.captured(2));
        if (match.capturedLength() == pattern.size()) return value;
        result += pattern.mid(end, match.capturedStart()-end) + value.toVariant().toString();
        end = match.capturedEnd();
    }
    return result + pattern.mid(end);
}

struct Cell {
    int first, last, rows, style;
    QJsonValue value;
};
struct Row {
    double height;
    QList<Cell> cells;
};

QList<Row> bandRows(const QJsonArray &band, const Grid &grid, const Styles &styles,
                    const QJsonObject &document)
{
    QList<Row> result;
    QList<int> occupied(grid.widths.size(),0);
    for (const auto item : band) {
        const auto settings = item.toObject();
        Row row{number(settings,"height",0.1,409),{}};
        require(settings.value("cells").isArray(), QStringLiteral("版式行缺少 cells 数组"));
        for (const auto def : settings.value("cells").toArray()) {
            const auto cell = def.toObject();
            int start = integer(cell,"from",1,grid.layoutUnits);
            int finish = integer(cell,"to",start,grid.layoutUnits);
            int rows = cell.contains("rows") ? integer(cell,"rows",1,band.size()-result.size()) : 1;
            int first = grid.layoutEdge(start-1)+1;
            int last = grid.layoutEdge(finish);
            for (int i = first-1; i < last; ++i) {
                require(occupied[i] <= result.size(), QStringLiteral("版式中存在重叠的单元格"));
                occupied[i] = result.size()+rows;
            }
            row.cells.append({first,last,rows,styles.id(cell.value("style").toString("default")),
                              bandText(cell.value("text").toString(),document)});
        }
        std::sort(row.cells.begin(),row.cells.end(),[](const Cell &a,const Cell &b){return a.first<b.first;});
        result.append(row);
    }
    return result;
}

double wrappedHeight(const QString &text, double width, const Style &style)
{
    if (!style.wrap || text.isEmpty()) return 0;
    int lines = 1;
    double used = 0;
    const double capacity = std::max(1., (width-0.6)*11/style.size);
    for (const auto character : text) {
        if (character == '\n') { ++lines; used=0; continue; }
        double advance = character.unicode() > 255 ? 2.0 : 1.0;
        if (used > 0 && used+advance > capacity) { ++lines; used=0; }
        used += advance;
    }
    return std::min(409.,lines*style.size*1.25+2);
}

QList<Row> detailRows(const QJsonObject &details, const Grid &grid, const Styles &styles,
                      const QJsonObject &document)
{
    const auto columns = details.value("columns").toArray();
    const auto lines = document.value("lines").toArray();
    const int count = std::max<int>(integer(details,"minRows",0,1048575),lines.size());
    require(count < 1048576, QStringLiteral("明细行数超出 Excel 上限"));
    const double height = number(details,"rowHeight",0.1,409);
    QList<Row> rows;
    rows.reserve(count+1);
    for (int index = -1; index < count; ++index) {
        const bool heading = index == -1;
        Row row{heading ? number(details,"headerHeight",0.1,409) : height,{}};
        const auto line = index >= 0 && index < lines.size() ? lines.at(index).toObject() : QJsonObject();
        int consumed = 0;
        for (const auto item : columns) {
            const auto column = item.toObject();
            const int first = grid.detailEdge(consumed)+1;
            consumed += column.value("span").toInt(1);
            const int id = styles.id(heading ? column.value("headerStyle").toString("tableHeader")
                                             : column.value("style").toString("detail"));
            QJsonValue value;
            if (heading) {
                require(column.value("label").isString(), QStringLiteral("明细列缺少 label"));
                value = column.value("label");
            } else if (index < lines.size()) {
                value = field(document,line,index+1,column.value("field").toString());
                if (value.toVariant().toString().trimmed().isEmpty() && column.contains("fallback"))
                    value = field(document,line,index+1,column.value("fallback").toString());
                if (value.isDouble() && value.toDouble()<=0 && column.contains("blankZeroUnless") &&
                    field(document,line,index+1,column.value("blankZeroUnless").toString()).toString().trimmed().isEmpty())
                    value = QJsonValue();
            }
            row.height = std::max(row.height,wrappedHeight(value.toVariant().toString(),
                                  column.value("width").toDouble(),styles.values.at(id)));
            row.cells.append({first,grid.detailEdge(consumed),1,id,value});
        }
        rows.append(row);
    }
    return rows;
}

QByteArray sheetXml(const Grid &grid, const QList<Row> &rows, const QJsonObject &page, Styles &styles)
{
    QByteArray bytes;
    QXmlStreamWriter xml(&bytes);
    xml.writeStartDocument();
    xml.writeStartElement("worksheet");
    xml.writeDefaultNamespace(spreadsheetNamespace);
    xml.writeStartElement("sheetPr");
    xml.writeEmptyElement("pageSetUpPr");
    xml.writeAttribute("fitToPage","1");
    xml.writeEndElement();
    xml.writeStartElement("sheetViews");
    xml.writeEmptyElement("sheetView");
    xml.writeAttribute("workbookViewId","0");
    xml.writeAttribute("showGridLines","0");
    xml.writeEndElement();
    xml.writeStartElement("cols");
    for (int i = 0; i < grid.widths.size(); ++i) {
        xml.writeEmptyElement("col");
        xml.writeAttribute("min",QString::number(i+1));
        xml.writeAttribute("max",QString::number(i+1));
        xml.writeAttribute("width",QString::number(grid.widths[i],'g',10));
        xml.writeAttribute("customWidth","1");
    }
    xml.writeEndElement();
    QStringList merges;
    QMap<int,QMap<int,QPair<int,QJsonValue>>> pendingEdges;
    xml.writeStartElement("sheetData");
    for (int i = 0; i < rows.size(); ++i) {
        const Row &row = rows.at(i);
        xml.writeStartElement("row");
        xml.writeAttribute("r",QString::number(i+1));
        xml.writeAttribute("ht",QString::number(row.height));
        xml.writeAttribute("customHeight","1");
        auto entries = pendingEdges.take(i);
        for (const auto &cell : row.cells) {
            if (cell.last == cell.first && cell.rows == 1) {
                entries.insert(cell.first, {cell.style, cell.value});
                continue;
            }
            merges.append(reference(cell.first,i+1) + ":" + reference(cell.last,i+cell.rows));
            // Excel prints merged borders per physical cell along the perimeter.
            for (int r = 0; r < cell.rows; ++r) {
                for (int col = cell.first; col <= cell.last; ++col) {
                    QStringList edges;
                    if (col == cell.first) edges.append("left");
                    if (col == cell.last) edges.append("right");
                    if (r == 0) edges.append("top");
                    if (r == cell.rows-1) edges.append("bottom");
                    const bool anchor = !r && col == cell.first;
                    if (!anchor && (edges.isEmpty() || styles.values.at(cell.style).border.isEmpty())) continue;
                    auto &target = r ? pendingEdges[i+r] : entries;
                    target.insert(col, {styles.mergedEdge(cell.style,edges), anchor ? cell.value : QJsonValue()});
                }
            }
        }
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            const auto &value = it.value().second;
            xml.writeStartElement("c");
            xml.writeAttribute("r",reference(it.key(),i+1));
            xml.writeAttribute("s",QString::number(it.value().first));
            if (value.isDouble()) {
                xml.writeTextElement("v",QString::number(value.toDouble(),'g',15));
            } else if (value.isString()) {
                xml.writeAttribute("t","inlineStr");
                xml.writeStartElement("is");
                xml.writeStartElement("t");
                xml.writeAttribute("xml:space","preserve");
                xml.writeCharacters(value.toString());
                xml.writeEndElement();
                xml.writeEndElement();
            }
            xml.writeEndElement();
        }
        xml.writeEndElement();
    }
    xml.writeEndElement();
    if (!merges.isEmpty()) {
        xml.writeStartElement("mergeCells");
        xml.writeAttribute("count",QString::number(merges.size()));
        for (const auto &range : merges) { xml.writeEmptyElement("mergeCell"); xml.writeAttribute("ref",range); }
        xml.writeEndElement();
    }
    xml.writeEmptyElement("printOptions");
    xml.writeAttribute("horizontalCentered",page.value("centerHorizontally").toBool()?"1":"0");
    xml.writeAttribute("verticalCentered",page.value("centerVertically").toBool()?"1":"0");
    xml.writeEmptyElement("pageMargins");
    const auto margins = page.value("margins").toObject();
    for (const auto edge : {"left","right","top","bottom","header","footer"})
        xml.writeAttribute(edge,QString::number(number(margins,edge,0,49)));
    const auto orientation = page.value("orientation").toString();
    require(orientation=="portrait" || orientation=="landscape",QStringLiteral("无效的打印方向"));
    xml.writeEmptyElement("pageSetup");
    xml.writeAttribute("paperSize",QString::number(integer(page,"paperSize",1,118)));
    xml.writeAttribute("orientation",orientation);
    xml.writeAttribute("fitToWidth",QString::number(integer(page,"fitToWidth",0,32767)));
    xml.writeAttribute("fitToHeight","0");
    xml.writeStartElement("headerFooter");
    xml.writeTextElement("oddHeader",page.value("header").toString());
    xml.writeTextElement("oddFooter",page.value("footer").toString());
    xml.writeEndDocument();
    require(!xml.hasError(),QStringLiteral("单据或模板含有 Excel 不支持的字符"));
    return bytes;
}
}

bool JsonFormRenderer::write(const QString &templatePath, const QJsonObject &document,
                             const QString &outputPath, QString *errorMessage)
{
    try {
        QFile file(templatePath);
        require(file.open(QIODevice::ReadOnly),file.errorString());
        QJsonParseError parse;
        const auto spec = QJsonDocument::fromJson(file.readAll(),&parse).object();
        require(parse.error==QJsonParseError::NoError,
                QStringLiteral("JSON 第 %1 字节：%2").arg(parse.offset).arg(parse.errorString()));
        require(spec.value("version").toInt()==1,QStringLiteral("不支持的模板版本"));
        const auto name = spec.value("sheetName").toString();
        static const QRegularExpression invalidSheetName(QStringLiteral(R"([\\/\?\*\[\]:])"));
        require(!name.isEmpty() && name.size()<=31 && !name.contains(invalidSheetName) &&
                !name.startsWith('\'') && !name.endsWith('\''),QStringLiteral("工作表名无效"));
        require(spec.value("header").isArray() && spec.value("footer").isArray(),QStringLiteral("缺少表头或表尾"));
        const auto details = spec.value("details").toObject();
        const auto page = spec.value("print").toObject();
        Styles styles(spec.value("styles").toObject());
        Grid grid(integer(spec,"layoutColumns",1,16384),details.value("columns").toArray());
        auto rows = bandRows(spec.value("header").toArray(),grid,styles,document);
        const int headerEnd = rows.size()+1;
        rows.append(detailRows(details,grid,styles,document));
        rows.append(bandRows(spec.value("footer").toArray(),grid,styles,document));
        require(rows.size()<=1048576,QStringLiteral("表单行数超出 Excel 上限"));
        QString quotedName = name;
        quotedName.replace("'","''");
        const QString prefix = "'" + quotedName + "'!";
        QMap<QString,QString> names;
        names.insert("_xlnm.Print_Area",prefix+"$A$1:$"+letters(grid.widths.size())+"$"+QString::number(rows.size()));
        if (page.value("repeatHeader").toBool())
            names.insert("_xlnm.Print_Titles",prefix+"$1:$"+QString::number(headerEnd));
        const auto worksheet = sheetXml(grid,rows,page,styles);
        return XlsxExporter::writeForm(outputPath,name,worksheet,styles.xmlData(),names,errorMessage);
    } catch (const std::runtime_error &error) {
        if (errorMessage) *errorMessage = QStringLiteral("%1：%2").arg(templatePath,QString::fromUtf8(error.what()));
        return false;
    }
}
