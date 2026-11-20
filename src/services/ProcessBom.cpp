#include "services/ProcessBom.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <cmath>
#include <functional>

namespace {
constexpr double maxLineQuantity = 999999999999.0; // StockLineTable quantity and usage controls.
bool readNodes(const QJsonArray &array, QList<ProcessBomItem> &nodes, bool topLevel,
               const QSet<QString> &ancestors, QString &error)
{
    QSet<QString> namedGroups;
    for (const auto &value : array) {
        const auto object = value.toObject();
        ProcessBomItem node;
        node.code = object.value("code").toString().trimmed();
        node.name = object.value("name").toString().trimmed();
        const bool group = topLevel && node.code.isEmpty();
        node.quantity = object.value("quantity").toDouble(group && !object.contains("quantity") ? 1.0 : 0.0);
        const auto children = object.value("items");
        if ((!group && node.code.isEmpty()) || (group && (node.name.isEmpty() || namedGroups.contains(node.name)
            || !children.isArray())) || (!children.isUndefined() && !children.isArray())) {
            error = QStringLiteral("第 %1 个节点缺少物料编码、工艺组名称重复或下级列表格式错误。").arg(nodes.size()+1);
            return false;
        }
        if (group) namedGroups.insert(node.name);
        if (!(node.quantity > 0 && node.quantity <= maxLineQuantity)
            || QString::number(node.quantity, 'f', 6).toDouble() != node.quantity) {
            error = QStringLiteral("%1 的节点用量应为正数，最多六位小数。").arg(node.label());
            return false;
        }
        auto path = ancestors;
        if (!node.code.isEmpty()) {
            if (path.contains(node.code.toUpper())) {
                error = QStringLiteral("%1 与其上级物料形成循环。").arg(node.code);
                return false;
            }
            path.insert(node.code.toUpper());
        }
        if (!readNodes(children.toArray(), node.items, false, path, error)) return false;
        nodes.append(std::move(node));
    }
    return true;
}

QJsonArray writeNodes(const QList<ProcessBomItem> &nodes)
{
    QJsonArray array;
    for (const auto &node : nodes) {
        QJsonObject object{{"name", node.name}, {"quantity", node.quantity}};
        if (!node.code.isEmpty()) object.insert("code", node.code);
        if (node.code.isEmpty() || !node.items.isEmpty()) object.insert("items", writeNodes(node.items));
        array.append(object);
    }
    return array;
}
}

bool ProcessBom::parse(const QByteArray &json, ProcessBom &bom, QString &error)
{
    QJsonParseError parseError;
    const auto root = QJsonDocument::fromJson(json, &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        error = QStringLiteral("JSON 格式错误（位置 %1）：%2").arg(parseError.offset).arg(parseError.errorString());
        return false;
    }
    ProcessBom parsed;
    if (root.contains("bomType")) {
        parsed.bomType = root.value("bomType").toString();
        if (parsed.bomType != "material" && parsed.bomType != "process") {
            error = QStringLiteral("bomType 应为 material（成品 BOM）或 process（工艺 BOM）。");
            return false;
        }
    }
    parsed.productCode = root.value("productCode").toString().trimmed();
    const auto nodes = root.value(root.contains("nodes") ? "nodes" : "groups").toArray();
    if (parsed.productCode.isEmpty() || nodes.isEmpty()) {
        error = QStringLiteral("请填写成品编码 productCode 和非空的工艺 BOM 节点列表。");
        return false;
    }
    if (!readNodes(nodes, parsed.groups, true, {parsed.productCode.toUpper()}, error)) return false;
    bom = std::move(parsed);
    return true;
}

bool ProcessBom::load(QSqlDatabase db, qlonglong productId, ProcessBom &bom, QString &error)
{
    QSqlQuery query(db);
    query.prepare("SELECT payload FROM process_boms WHERE product_material_id=?");
    query.addBindValue(productId);
    if (!query.exec()) { error = query.lastError().text(); return false; }
    bom = {};
    if (!query.next()) return true;
    const auto payload = query.value(0).toString();
    if (!parse(payload.toUtf8(), bom, error)) return false;
    bom.originalPayload = payload;
    return bom.resolveMaterials(db, error);
}

bool ProcessBom::resolveMaterials(QSqlDatabase db, QString &error)
{
    QHash<QString, QList<ProcessBomItem *>> byCode;
    std::function<void(QList<ProcessBomItem>&)> collect = [&](QList<ProcessBomItem> &nodes) {
        for (auto &item : nodes) {
            if (!item.code.isEmpty()) {
                item.materialId = 0;
                item.issueError = QStringLiteral("缺少物料档案");
                byCode[item.code.toUpper()].append(&item);
            }
            collect(item.items);
        }
    };
    collect(groups);
    const auto codes = byCode.keys();
    for (qsizetype offset = 0; offset < codes.size(); offset += 500) {
        const auto batch = codes.mid(offset, 500);
        QSqlQuery query(db);
        query.prepare("SELECT m.code,m.id,m.is_active,c.code,m.specification,c.name,m.unit,m.processing_method,"
                      "COALESCE((SELECT SUM(s.quantity) FROM stock_balances s WHERE s.material_id=m.id),0) FROM materials m "
                      "LEFT JOIN material_categories c ON c.id=m.category_id WHERE m.code IN ("
                      + QStringList(batch.size(), QStringLiteral("?")).join(',') + ")");
        for (const auto &code : batch) query.addBindValue(code);
        if (!query.exec()) { error = query.lastError().text(); return false; }
        while (query.next()) {
            for (auto *item : byCode.value(query.value(0).toString().toUpper())) {
                item->materialId = query.value(1).toLongLong();
                item->specification = query.value(4).toString();
                item->category = query.value(5).toString();
                item->unit = query.value(6).toString();
                item->processing = query.value(7).toString();
                item->stock = QString::number(query.value(8).toDouble(), 'g', 12);
                if (!query.value(2).toBool()) item->issueError = QStringLiteral("物料已停用");
                else if (query.value(3).toString() == "FINISHED") item->issueError = QStringLiteral("成品不可作为生产用料");
                else item->issueError.clear();
            }
        }
    }
    return true;
}

bool ProcessBomItem::issueMaterials(double count, QList<QPair<qlonglong, double>> &lines, QString &error) const
{
    QStringList problems;
    QList<QPair<qlonglong, double>> result;
    QHash<qlonglong, qsizetype> positions;
    std::function<void(const ProcessBomItem &, double)> visit = [&](const ProcessBomItem &item, double parentUsage) {
        const double cumulative = parentUsage * item.quantity;
        if (!item.issueError.isEmpty()) {
            problems << QStringLiteral("%1：%2").arg(item.label(), item.issueError);
        } else if (!item.items.isEmpty()) {
            for (const auto &child : item.items) visit(child, cumulative);
        } else if (item.materialId <= 0) {
            problems << QStringLiteral("%1 没有可领用的物料。").arg(item.label());
        } else {
            if (!positions.contains(item.materialId)) {
                positions.insert(item.materialId, result.size());
                result.append({item.materialId, 0.0});
            }
            result[positions.value(item.materialId)].second += cumulative;
        }
    };
    visit(*this, 1);
    for (const auto &line : result) {
        const double usage = line.second;
        if (!std::isfinite(usage) || usage * count > maxLineQuantity || usage < 0.000001
            || std::abs(std::round(usage * 1e6) / 1e6 - usage) > 1e-10) {
            problems << QStringLiteral("累计用量超出领料数量范围或六位小数精度。");
        }
    }
    if (!problems.isEmpty()) {
        problems.removeDuplicates();
        error = problems.join('\n');
        return false;
    }
    lines = std::move(result);
    return true;
}

bool ProcessBom::store(QSqlDatabase db, qlonglong productId, const QString *expected, QString &error) const
{
    QSqlQuery save(db);
    if (groups.isEmpty()) {
        save.prepare("DELETE FROM process_boms WHERE product_material_id=? AND payload=?");
        save.addBindValue(productId);
        save.addBindValue(expected ? *expected : QString());
    } else {
        // Keep flat recipes readable by clients which understand the original group format.
        bool flatGroups = true;
        for (const auto &group : groups) {
            if (!group.code.isEmpty() || group.quantity != 1 || group.items.isEmpty()) flatGroups = false;
            for (const auto &item : group.items)
                if (!item.items.isEmpty()) flatGroups = false;
        }
        const QString payload = QString::fromUtf8(QJsonDocument(QJsonObject{
            {"productCode", productCode}, {flatGroups ? "groups" : "nodes", writeNodes(groups)}
        }).toJson(QJsonDocument::Compact));
        ProcessBom validated;
        if (!parse(payload.toUtf8(), validated, error)) return false;
        if (expected && !expected->isNull()) {
            save.prepare("UPDATE process_boms SET payload=? WHERE product_material_id=? AND payload=?");
            save.addBindValue(payload);
            save.addBindValue(productId);
            save.addBindValue(*expected);
        } else {
            const QString conflict = expected ? "DO NOTHING" : "DO UPDATE SET payload=excluded.payload";
            save.prepare("INSERT INTO process_boms(product_material_id,payload) VALUES(?,?) "
                         "ON CONFLICT(product_material_id) " + conflict);
            save.addBindValue(productId);
            save.addBindValue(payload);
        }
    }
    if (!save.exec()) { error = save.lastError().text(); return false; }
    if (save.numRowsAffected() == 1) return true;
    error = QStringLiteral("工艺 BOM 已发生变化，请刷新后重试。");
    return false;
}
