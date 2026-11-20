#include "database/SchemaMigrator.h"
#include "import/LegacyInventoryImporter.h"
#include "services/ProcessBom.h"
#include "ui/pages/MaterialPage.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlQuery>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

namespace {
void check(bool ok, const QString &message)
{
    if (!ok) qFatal("%s", qPrintable(message));
}

QPushButton *button(QWidget &page, const QString &text)
{
    for (auto *b : page.findChildren<QPushButton *>())
        if (b->text() == text) return b;
    qFatal("Missing button: %s", qPrintable(text));
}

void checkHierarchyActions(QWidget &page)
{
    for (const QString &name : {"导入BOM", "查看物料详情", "添加下级", "调整选中节点",
                                "删除选中节点", "清空整棵BOM", "全部展开", "全部折叠", "导出 JSON"}) {
        const auto *action = button(page, name);
        check(action->isVisible() && !action->visibleRegion().isEmpty(),
              QStringLiteral("页签未显示操作按钮：%1").arg(name));
    }
}

void editNode(QWidget &page, QString action, QString code, double quantity)
{
    QTimer::singleShot(0, [=] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        check(dialog, "Expected editor");
        auto *box = dialog->findChild<QComboBox *>();
        const int index = box->findText(code + " - ", Qt::MatchStartsWith);
        check(index >= 0, "Missing material option");
        box->setCurrentIndex(index);
        dialog->findChild<QDoubleSpinBox *>()->setValue(quantity);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    button(page, action)->click();
}

void confirm()
{
    auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    check(box && box->button(QMessageBox::Yes), "Expected confirmation");
    box->button(QMessageBox::Yes)->click();
}
QString chooseJsonFile(QPushButton *button, const QString &path, bool accept = true)
{
    QString message;
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, [&] {
        auto *modal = QApplication::activeModalWidget();
        if (auto *file = qobject_cast<QFileDialog *>(modal)) {
            if (path.isEmpty()) { file->reject(); return; }
            file->selectFile(path);
            QMetaObject::invokeMethod(file, "accept");
        } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            message = box->text();
            if (box->button(QMessageBox::Yes))
                box->button(accept ? QMessageBox::Yes : QMessageBox::No)->click();
            else box->accept();
        }
    });
    dialogs.start(10);
    button->click();
    return message;
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("Cannot open %s: %s", qPrintable(path), qPrintable(file.errorString()));
    const auto json = QJsonDocument::fromJson(file.readAll());
    check(json.isObject(), "Expected an exported JSON object");
    return json.object();
}

void writeJson(const QString &path, const QJsonObject &json)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), file.errorString());
    const auto bytes = QJsonDocument(json).toJson();
    check(file.write(bytes) == bytes.size(), file.errorString());
}

}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    auto db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(":memory:");
    QString error;
    check(db.open() && SchemaMigrator::migrate(db, &error), error);
    BomImportResult excel;
    excel.productCode = "P1";
    excel.sourceSheet = "BOM";
    excel.rows = {{1,0,-1,"P1","Product","","FINISHED","台",1,{}},
                  {2,1,0,"W1","Frame","","SEMI","套",2,{}},
                  {3,2,1,"M1","Bolt","","RAW","个",3,{}}};
    check(BomExcelImporter::importRows(db, 1, excel, nullptr, nullptr, &error), error);
    excel.rows[2].quantity = 5;
    check(BomExcelImporter::importRows(db, 1, excel, nullptr, nullptr, &error, true), error);

    Session session;
    session.userId = 1;
    session.roleCode = "ADMIN";
    MaterialPage page(db, session);
    page.show();
    auto *tabs = page.findChild<QTabBar *>();
    auto *tree = page.findChild<QTreeWidget *>();
    auto root = [&] { return tree->topLevelItem(0); };
    check(root()->child(0)->child(0)->text(5) == "6", "Ordinary BOM was overwritten");
    auto *materialJson = button(page, "导入成品 BOM JSON");
    auto *processJson = button(page, "导入工艺 BOM JSON");
    checkHierarchyActions(page);
    check(materialJson->isVisible() && !processJson->isVisible(), "Wrong ordinary BOM action");
    QTemporaryDir temp;
    check(temp.isValid(), temp.errorString());
    auto *exportButton = button(page, "导出 JSON");
    chooseJsonFile(exportButton, {});
    check(QDir(temp.path()).isEmpty(), "Cancelled export created a file");
    const QString hierarchy = temp.filePath("成品配方.json");
    const auto exported = chooseJsonFile(exportButton, temp.filePath("成品配方"));
    check(exported.contains(QDir::toNativeSeparators(hierarchy)), "Unexpected export result: " + exported);
    auto material = readJson(hierarchy);
    auto frame = material["nodes"].toArray().first().toObject();
    auto bolt = frame["items"].toArray().first().toObject();
    check(material["bomType"] == "material" && material["productCode"] == "P1" && frame["code"] == "W1"
          && frame["quantity"] == 2 && bolt["name"] == "Bolt" && bolt["quantity"] == 3, "Incorrect material export");
    frame["quantity"] = 1234567.890123;
    material["nodes"] = QJsonArray{frame};
    writeJson(hierarchy, material);
    chooseJsonFile(materialJson, hierarchy);
    const QString exact = temp.filePath("precision.json");
    chooseJsonFile(exportButton, exact);
    check(readJson(exact) == material, "Round trip lost node quantity precision");
    frame["quantity"] = 3;
    material["nodes"] = QJsonArray{frame};
    writeJson(hierarchy, material);
    check(chooseJsonFile(materialJson, hierarchy, false).contains("成品 BOM"), "Wrong confirmation");
    check(root()->child(0)->text(4).toDouble() > 1000000, "Cancel changed ordinary BOM");
    chooseJsonFile(materialJson, hierarchy);
    check(root()->child(0)->child(0)->text(5) == "9", "Ordinary JSON did not replace hierarchy");
    tabs->setCurrentIndex(1);
    check(root()->child(0)->child(0)->text(5) == "10", "Ordinary import changed process BOM");
    check(processJson->isVisible() && !materialJson->isVisible(), "Wrong process BOM action");
    checkHierarchyActions(page);
    check(chooseJsonFile(processJson, hierarchy).contains("bomType"), "Allowed wrong JSON type");
    QFile recipe(temp.filePath("recipe.json"));
    check(recipe.open(QIODevice::WriteOnly), recipe.errorString());
    recipe.write(R"({"productCode":"P1","groups":[{"name":"Assembly","items":[{"code":"M1","name":"Bolt","quantity":2},{"code":"UNKNOWN","name":"Pending","quantity":1}]}]})");
    recipe.close();
    chooseJsonFile(processJson, recipe.fileName());
    check(root()->child(0)->text(0) == "Assembly", "Legacy JSON was not imported");
    const QString processPath = temp.filePath("工艺配方.json");
    chooseJsonFile(exportButton, processPath);
    auto process = readJson(processPath);
    auto group = process["groups"].toArray().first().toObject();
    check(process["bomType"] == "process" && process["productCode"] == "P1" && group["name"] == "Assembly"
          && group["items"].toArray().last().toObject()["code"] == "UNKNOWN", "Lost process group or pending material");
    group["name"] = "装配组";
    process["groups"] = QJsonArray{group};
    writeJson(processPath, process);
    chooseJsonFile(processJson, processPath);
    check(root()->child(0)->text(0) == "装配组", "Edited process export failed to import");
    tree->setCurrentItem(root()->child(0)->child(1));
    check(!button(page, "查看物料详情")->isEnabled(), "Missing archive resolved incorrectly");
    editNode(page, "调整选中节点", "W1", 3);
    tree->setCurrentItem(root()->child(0)->child(1));
    editNode(page, "添加下级", "M1", 4);
    check(root()->child(0)->child(1)->child(0)->text(5) == "12", "Added subtree quantity");
    const QString nestedPath = temp.filePath("嵌套工艺.json");
    tree->setCurrentItem(root()->child(0)->child(1)->child(0));
    chooseJsonFile(exportButton, nestedPath);
    const auto nested = readJson(nestedPath);
    const auto assembly = nested["nodes"].toArray().first().toObject();
    const auto assemblyItems = assembly["items"].toArray();
    check(assembly["name"] == "装配组" && assemblyItems.size() == 2 &&
          assemblyItems.last().toObject()["items"].toArray().first().toObject()["quantity"] == 4,
          "Selecting a leaf lost the rest of the process tree");
    chooseJsonFile(processJson, nestedPath);
    QSqlQuery product(db);
    check(product.exec("SELECT id FROM materials WHERE code='P1'") && product.next(), "Product archive");
    const auto id = product.value(0).toLongLong();
    ProcessBom saved;
    check(ProcessBom::load(db, id, saved, error), error);
    QList<QPair<qlonglong,double>> lines;
    check(saved.groups.first().issueMaterials(3, lines, error) && lines.size()==1
              && lines.first().second == 14, "Expected per-unit issue quantity 2 + 3 x 4");
    tree->setCurrentItem(root()->child(0)->child(1));
    QTimer::singleShot(0, confirm);
    button(page, "删除选中节点")->click();
    check(root()->child(0)->childCount() == 1, "Remove subtree");
    check(!saved.store(db, id, &saved.originalPayload, error), "Overwrote concurrent edit");
    QTimer::singleShot(0, confirm);
    button(page, "清空整棵BOM")->click();
    check(root()->childCount() == 0, "Clear process tree");
    check(!exportButton->isEnabled(), "Empty tree offered an unusable export");
    editNode(page, "添加下级", "W1", 2);
    check(root()->child(0)->text(5) == "2", "Create from empty root");
    tabs->setCurrentIndex(0);
    check(materialJson->isVisible() && root()->child(0)->child(0)->text(5) == "9", "Ordinary BOM isolation");
    check(chooseJsonFile(materialJson, processPath).contains("bomType"), "Accepted process export in material tab");
    check(chooseJsonFile(materialJson, recipe.fileName()).contains("UNKNOWN"), "Missing archive not reported");
    check(root()->child(0)->child(0)->text(5) == "9", "Invalid JSON changed ordinary tree");
    session.roleCode = "VIEWER";
    MaterialPage viewer(db, session);
    viewer.findChild<QTabBar *>()->setCurrentIndex(1);
    check(button(viewer, "导出 JSON")->isEnabled(), "Viewer cannot export the displayed BOM");
    for (const QString &name : {"导入成品 BOM JSON","导入工艺 BOM JSON","导入BOM","添加下级","调整选中节点","删除选中节点","清空整棵BOM"})
        check(!button(viewer, name)->isEnabled(), "Read-only user can edit");
    qInfo("PASS material and process JSON export/edit/import, precision, hierarchy, isolation and permissions");
}
