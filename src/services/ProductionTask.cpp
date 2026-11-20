#include "services/ProductionTask.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>
#include <cmath>
#include <functional>

namespace {
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
}

bool ProductionTask::load(QSqlDatabase db, qlonglong runId, qlonglong productId,
                          ProductionTask &task, QString *error)
{
    task.m_nodes.clear();
    QSqlQuery saved(db);
    saved.prepare(QStringLiteral("SELECT payload FROM production_task_boms WHERE production_run_id=?"));
    saved.addBindValue(runId);
    if (!saved.exec()) return fail(error, saved.lastError().text());
    if (saved.next()) {
        const auto json = QJsonDocument::fromJson(saved.value(0).toString().toUtf8());
        if (!json.isArray() || json.array().isEmpty())
            return fail(error, QStringLiteral("生产任务的BOM快照损坏，不能重新套用当前BOM。"));
        for (const auto &value : json.array()) {
            const auto o = value.toObject();
            ProductionStage n;
            n.key = o.value("key").toString(); n.parent = o.value("parent").toString();
            n.materialId = o.value("material").toString().toLongLong();
            n.code = o.value("code").toString(); n.name = o.value("name").toString();
            n.model = o.value("model").toString(); n.category = o.value("category").toString();
            n.quantity = o.value("quantity").toDouble(); n.perMachine = o.value("perMachine").toDouble();
            task.m_nodes.append(n);
        }
    } else {
        QSqlQuery root(db);
        root.prepare(QStringLiteral("SELECT m.code,m.name,m.specification,c.code FROM materials m "
                                    "JOIN material_categories c ON c.id=m.category_id WHERE m.id=?"));
        root.addBindValue(productId);
        if (!root.exec() || !root.next()) return fail(error, QStringLiteral("找不到生产成品。"));
        ProductionStage n;
        n.key = QStringLiteral("0"); n.materialId = productId;
        n.code = root.value(0).toString(); n.name = root.value(1).toString();
        n.model = root.value(2).toString(); n.category = root.value(3).toString();
        task.m_nodes.append(n);
        QSqlQuery nodes(db);
        nodes.prepare(QStringLiteral("SELECT b.id,COALESCE(b.parent_item_id,0),b.component_material_id,"
            "b.quantity,m.code,m.name,m.specification,c.code FROM material_bom_items b "
            "JOIN materials m ON m.id=b.component_material_id JOIN material_categories c ON c.id=m.category_id "
            "WHERE b.product_material_id=? ORDER BY b.id"));
        nodes.addBindValue(productId);
        if (!nodes.exec()) return fail(error, nodes.lastError().text());
        while (nodes.next()) {
            ProductionStage child;
            child.key = nodes.value(0).toString(); child.parent = nodes.value(1).toString();
            child.materialId = nodes.value(2).toLongLong(); child.quantity = nodes.value(3).toDouble();
            child.code = nodes.value(4).toString(); child.name = nodes.value(5).toString();
            child.model = nodes.value(6).toString(); child.category = nodes.value(7).toString();
            task.m_nodes.append(child);
        }
    }
    QMap<QString, int> indexes;
    for (int i = 0; i < task.m_nodes.size(); ++i) {
        const auto &n = task.m_nodes.at(i);
        if (n.key.isEmpty() || indexes.contains(n.key) || n.materialId <= 0 || !std::isfinite(n.quantity)
            || n.quantity <= 0 || (n.key != QStringLiteral("0") && n.parent.isEmpty()))
            return fail(error, QStringLiteral("BOM节点或用量无效。"));
        indexes.insert(n.key, i);
    }
    if (!indexes.contains(QStringLiteral("0")) || task.stage(QStringLiteral("0"))->materialId != productId)
        return fail(error, QStringLiteral("生产任务与BOM成品不一致。"));
    // Resolve parents explicitly; reject cycles and orphaned nodes instead of silently dropping demand.
    for (auto &n : task.m_nodes) {
        QSet<QString> visited;
        double total = n.quantity;
        QString parent = n.parent;
        visited.insert(n.key);
        while (!parent.isEmpty()) {
            if (visited.contains(parent) || !indexes.contains(parent))
                return fail(error, QStringLiteral("BOM存在循环或缺失的上级节点。"));
            visited.insert(parent);
            const auto &p = task.m_nodes.at(indexes.value(parent));
            total *= p.quantity;
            parent = p.parent;
        }
        if (!std::isfinite(total) || total <= 0) return fail(error, QStringLiteral("BOM累计用量无效。"));
        n.perMachine = total;
    }
    return true;
}

QString ProductionTask::payload() const
{
    QJsonArray rows;
    for (const auto &n : m_nodes) rows.append(QJsonObject{
        {"key",n.key},{"parent",n.parent},{"material",QString::number(n.materialId)},
        {"code",n.code},{"name",n.name},{"model",n.model},{"category",n.category},
        {"quantity",n.quantity},{"perMachine",n.perMachine}});
    return QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact));
}

QString ProductionTask::fingerprint() const
{
    return QString::fromLatin1(QCryptographicHash::hash(payload().toUtf8(), QCryptographicHash::Sha256).toHex());
}

const ProductionStage *ProductionTask::stage(const QString &key) const
{
    for (const auto &n : m_nodes) if (n.key == key) return &n;
    return nullptr;
}

QList<ProductionStage> ProductionTask::stages() const
{
    QList<ProductionStage> result;
    for (const auto &n : m_nodes) {
        if (n.key == QStringLiteral("0")) { result.append(n); continue; }
        if (n.category != QStringLiteral("SEMI")) continue;
        for (const auto &c : m_nodes) if (c.parent == n.key) { result.append(n); break; }
    }
    return result;
}

QString ProductionTask::stagePath(const QString &key) const
{
    QStringList parts;
    const auto *node = stage(key);
    while (node) {
        parts.prepend(node->name);
        node = node->parent.isEmpty() ? nullptr : stage(node->parent);
    }
    return parts.join(QStringLiteral(" / "));
}

bool ProductionTask::demand(QSqlDatabase db, qlonglong runId, const QString &key, double count,
                            QList<ProductionDemand> &result, QString *error) const
{
    result.clear();
    const auto *target = stage(key);
    bool valid = false;
    for (const auto &s : stages()) if (s.key == key) valid = true;
    if (!target || !valid || !std::isfinite(count) || count <= 0)
        return fail(error, QStringLiteral("请选择有效的整机装配或半成品工序。"));
    QMap<qlonglong, ProductionDemand> grouped;
    std::function<void(const QString &, double)> expand = [&](const QString &parent, double multiplier) {
        for (const auto &n : m_nodes) {
            if (n.parent != parent) continue;
            bool hasChildren = false;
            for (const auto &child : m_nodes) if (child.parent == n.key) { hasChildren = true; break; }
            const double usage = multiplier * n.quantity;
            if (hasChildren && n.category != QStringLiteral("SEMI")) {
                expand(n.key, usage);
            } else {
                auto &d = grouped[n.materialId];
                d.materialId = n.materialId; d.code = n.code; d.name = n.name;
                d.unitUsage += usage;
            }
        }
    };
    expand(key, 1);
    QSqlQuery issued(db);
    issued.prepare(QStringLiteral("SELECT i.material_id,SUM(i.quantity-i.returned_quantity-i.reversed_quantity) "
        "FROM production_document_stages s JOIN business_documents d ON d.id=s.document_id "
        "JOIN business_document_items i ON i.document_id=d.id WHERE s.production_run_id=? AND s.stage_key=? "
        "AND d.production_run_id=s.production_run_id AND d.document_type='SCLL' "
        "AND d.status IN ('POSTED','PARTIALLY_REVERSED','REVERSED') "
        "AND EXISTS(SELECT 1 FROM inventory_ledger l WHERE l.document_item_id=i.id AND l.business_type='SCLL') "
        "GROUP BY i.material_id"));
    issued.addBindValue(runId); issued.addBindValue(key);
    if (!issued.exec()) return fail(error, issued.lastError().text());
    QMap<qlonglong,double> totals;
    while (issued.next()) totals.insert(issued.value(0).toLongLong(), issued.value(1).toDouble());
    for (auto d : grouped) {
        d.required = d.unitUsage * target->perMachine * count;
        d.issued = totals.value(d.materialId);
        if (!std::isfinite(d.required)) return fail(error, QStringLiteral("生产计划用量超出范围。"));
        result.append(d);
    }
    return true;
}

double ProductionTask::received(QSqlDatabase db, qlonglong runId, const QString &key, QString *error)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT COALESCE(SUM(i.quantity-i.reversed_quantity),0) "
        "FROM production_document_stages s JOIN business_documents d ON d.id=s.document_id "
        "JOIN business_document_items i ON i.document_id=d.id WHERE s.production_run_id=? AND s.stage_key=? "
        "AND d.production_run_id=s.production_run_id AND d.document_type='CPRK' "
        "AND d.status IN ('POSTED','PARTIALLY_REVERSED')"));
    q.addBindValue(runId); q.addBindValue(key);
    if (!q.exec() || !q.next()) { fail(error, q.lastError().text()); return -1; }
    return q.value(0).toDouble();
}
