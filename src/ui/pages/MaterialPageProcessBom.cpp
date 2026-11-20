#include "ui/pages/MaterialPage.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QSqlError>
#include <QSqlQuery>
#include <QTabBar>
#include <QJsonObject>
#include <QJsonDocument>
#include "import/LegacyInventoryImporter.h"
#include <QTreeWidget>
#include <functional>

namespace {
QList<ProcessBomItem> &childrenAt(ProcessBom &bom, QTreeWidgetItem *parent)
{
    QList<int> path;
    for (auto *row = parent; row && row->parent(); row = row->parent())
        path.prepend(row->parent()->indexOfChild(row));
    auto *children = &bom.groups;
    for (const int index : path) children = &(*children)[index].items;
    return *children;
}

bool replaceMaterialTree(QSqlDatabase db, qlonglong productId, const ProcessBom &bom, QString &error)
{
    BomImportResult hierarchy;
    hierarchy.sourceSheet = QStringLiteral("JSON");
    hierarchy.productCode = bom.productCode;
    hierarchy.rows.append(BomImportRow{}); // The shared importer numbers the product root as row 0.
    QHash<QString, qlonglong> materialIds;
    std::function<void(const QList<ProcessBomItem> &, int)> append =
        [&](const QList<ProcessBomItem> &nodes, int parent) {
        for (const auto &node : nodes) {
            BomImportRow row;
            row.materialCode = node.code;
            row.quantity = node.quantity;
            row.parentIndex = parent;
            row.sourceRow = hierarchy.rows.size();
            hierarchy.rows.append(row);
            materialIds.insert(node.code, node.materialId);
            append(node.items, row.sourceRow);
        }
    };
    append(bom.groups, 0);
    return BomExcelImporter::replaceHierarchyInTransaction(db, productId, hierarchy, materialIds, &error);
}

}

bool MaterialPage::processView() const
{
    return m_viewTabs->currentIndex() == 1;
}

bool MaterialPage::saveBomTree(qlonglong productId, const ProcessBom &bom, const QString *expected,
                                  const QString &action, const QString &detail)
{
    if (!m_session.canManageMaterials()) return false;
    QString error;
    if (!m_database.transaction()) {
        error = m_database.lastError().text();
    } else {
        QSqlQuery audit(m_database);
        audit.prepare("INSERT INTO audit_logs(user_id,action,entity_type,entity_id,detail) "
                      "VALUES(?,?,'material',?,?)");
        audit.addBindValue(m_session.userId);
        audit.addBindValue(action);
        audit.addBindValue(productId);
        audit.addBindValue(detail);
        const bool stored = processView() ? bom.store(m_database, productId, expected, error)
                                          : replaceMaterialTree(m_database, productId, bom, error);
        if (stored) {
            if (!audit.exec()) error = audit.lastError().text();
            else if (!m_database.commit()) error = m_database.lastError().text();
            else {
                refreshBomTree();
                emit dataChanged();
                return true;
            }
        }
        m_database.rollback();
    }
    QMessageBox::warning(this, QStringLiteral("保存 BOM 失败"), error);
    refreshBomTree();
    return false;
}

void MaterialPage::changeProcessNode(QTreeWidgetItem *item, bool add, const QString &code,
                                     const QString &name, double quantity)
{
    auto changed = m_processBom;
    auto *parent = add ? item : item->parent();
    auto &siblings = childrenAt(changed, parent);
    const int index = add ? -1 : parent->indexOfChild(item);
    if (code.isEmpty() && name.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("调整工艺组"), QStringLiteral("请填写工艺组名称。"));
        return;
    }
    const bool sameMaterial = !add && siblings[index].code.compare(code, Qt::CaseInsensitive) == 0;
    for (int i = 0; i < siblings.size(); ++i) {
        if (i == index) continue;
        const auto &other = siblings.at(i);
        // Repeated source lines retain their own quantities when edited.
        if ((!sameMaterial && !code.isEmpty() && other.code.compare(code, Qt::CaseInsensitive) == 0)
            || (code.isEmpty() && other.code.isEmpty() && other.name == name)) {
            QMessageBox::warning(this, QStringLiteral("节点已存在"), QStringLiteral("同一上级下已有此节点。"));
            return;
        }
    }
    auto &node = add ? siblings.emplaceBack() : siblings[index];
    node.code = code;
    node.name = name;
    node.quantity = quantity;
    saveBomTree(m_bomProductCombo->currentData().toLongLong(), changed, &m_processBom.originalPayload,
                   add ? "PROCESS_BOM_ADD" : "PROCESS_BOM_ADJUST",
                   QStringLiteral("%1，用量 %2").arg(node.label()).arg(quantity, 0, 'g', 15));
}

void MaterialPage::removeProcessNode(QTreeWidgetItem *item)
{
    const bool clear = !item->parent();
    if (clear && m_processBom.groups.isEmpty()) return;
    const auto title = clear ? QStringLiteral("清空整棵工艺 BOM") : QStringLiteral("删除工艺 BOM 节点");
    if (QMessageBox::warning(this, title,
            QStringLiteral("确认移除“%1”的%2？物料档案和库存将保留。").arg(item->text(0),
                    clear ? QStringLiteral("全部工艺 BOM 节点") : QStringLiteral("节点及全部下级")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    auto changed = m_processBom;
    if (clear) changed.groups.clear();
    else childrenAt(changed, item->parent()).removeAt(item->parent()->indexOfChild(item));
    saveBomTree(m_bomProductCombo->currentData().toLongLong(), changed, &m_processBom.originalPayload,
                   clear ? "PROCESS_BOM_CLEAR" : "PROCESS_BOM_REMOVE", item->text(0));
}

void MaterialPage::importBomJson(bool process)
{
    if (!m_session.canManageMaterials() || process != processView()) return;
    const QString target = process ? QStringLiteral("工艺 BOM") : QStringLiteral("成品 BOM");
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("选择%1 JSON").arg(target), {},
                                                    QStringLiteral("JSON 文件 (*.json)"));
    if (path.isEmpty()) return;
    const auto warn = [this, target](const QString &message) {
        QMessageBox::warning(this, QStringLiteral("导入%1失败").arg(target), message);
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { warn(file.errorString()); return; }
    const auto json = file.readAll();
    ProcessBom bom;
    QString error;
    if (!ProcessBom::parse(json, bom, error)) { warn(error); return; }
    if (!bom.bomType.isEmpty() && bom.bomType != (process ? "process" : "material")) {
        warn(QStringLiteral("文件标记的 bomType 与当前%1页签不符，请选择对应的文件。").arg(target));
        return;
    }

    QSqlQuery product(m_database);
    product.prepare("SELECT m.id,m.name FROM materials m JOIN material_categories c ON c.id=m.category_id "
                    "WHERE m.code=? AND c.code='FINISHED'");
    product.addBindValue(bom.productCode);
    if (!product.exec()) { warn(product.lastError().text()); return; }
    if (!product.next()) { warn(QStringLiteral("找不到成品档案 %1。").arg(bom.productCode)); return; }
    const auto id = product.value(0);
    const auto name = product.value(1).toString();
    product.finish();
    if (!bom.resolveMaterials(m_database, error)) { warn(error); return; }
    QStringList unavailable;
    int rows = 0;
    std::function<void(const QList<ProcessBomItem> &)> inspect = [&](const QList<ProcessBomItem> &nodes) {
        for (const auto &node : nodes) {
            ++rows;
            inspect(node.items);
            if (!process && node.code.isEmpty())
                unavailable << node.label() + QStringLiteral("（成品 BOM 的每个节点都需要物料编码 code）");
            if (!node.issueError.isEmpty()) unavailable << node.label() + "（" + node.issueError + "）";
        }
    };
    inspect(bom.groups);
    unavailable.removeDuplicates();
    if (!process && !unavailable.isEmpty()) {
        warn(QStringLiteral("以下节点无法导入成品 BOM：\n%1").arg(unavailable.join('\n')));
        return;
    }
    auto summary = QStringLiteral("为 %1 %2 导入 %3 个节点，替换该成品的%4。")
                       .arg(bom.productCode, name).arg(rows).arg(target);
    if (!unavailable.isEmpty())
        summary += QStringLiteral("\n\n以下物料需核对后才能领用：\n") + unavailable.join('\n');
    if (QMessageBox::question(this, QStringLiteral("确认导入%1").arg(target), summary,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if (saveBomTree(id.toLongLong(), bom, nullptr, process ? "PROCESS_BOM_IMPORT" : "MATERIAL_BOM_IMPORT", summary)) {
        refreshBomProducts();
        m_bomProductCombo->setCurrentIndex(m_bomProductCombo->findData(id));
    }
}
