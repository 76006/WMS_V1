#include "core/SystemProfile.h"
#include "services/OfficeTemplateService.h"
#include "import/XlsxExporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 2 && args.size() != 5) return 2;
    const QDir output(args.at(1));
    const QMap<QString, OfficeFormKind> kinds = {
        {"inspection", OfficeFormKind::Inspection}, {"productionIssue", OfficeFormKind::ProductionIssue},
        {"rawInbound", OfficeFormKind::RawMaterialInbound}, {"finishedInbound", OfficeFormKind::FinishedGoodsInbound},
        {"stockOutbound", OfficeFormKind::StockOutbound}, {"deliveryConfirmation", OfficeFormKind::DeliveryConfirmation}
    };
    for (const auto &profile : {"ICE_BEAUTY", "MEI_JING_LING"}) {
        if (args.size() == 5 && args.at(2) != profile) continue;
        SystemProfile::setServerProfile(profile);
        for (auto it = kinds.begin(); it != kinds.end(); ++it) {
            if (args.size() == 5 && args.at(3) != it.key()) continue;
            for (int count : {1, 22, 45}) {
                if (args.size() == 5 && args.at(4).toInt() != count) continue;
                OfficeTemplateDocument doc;
                doc.kind = it.value();
                doc.documentDate = QDate(2026, 11, 19);
                doc.documentNumber = QStringLiteral("TLRK-20261119-0002");
                doc.fields = {{"supplier","测试供应商乙"},{"notes","整单备注"},{"customerCompany","测试客户"},
                              {"salesOrderNumber","TEST-001"},{"urgency","URGENT"},{"warehouseKeeper","不得回填签名"}};
                if (count == 45) doc.fields.insert("originalIssueNumber","TEST-RETURN");
                for (int i = 0; i < count; ++i) {
                    OfficeTemplateLine line;
                    line.materialCode = QStringLiteral("MAT-%1").arg(i,3,10,QChar('0'));
                    line.materialName = QStringLiteral("测试物料");
                    line.batchNo = QStringLiteral("000123");
                    line.specification = "SPEC-001";
                    line.unit = QStringLiteral("个");
                    line.quantity = i+1;
                    line.notes = QStringLiteral("备注%1").arg(i);
                    if (i%2 == 0) line.supplier = QStringLiteral("测试供应商甲（用于版式验证，不是业务数据）");
                    doc.lines.append(line);
                }
                QString error;
                auto name = QStringLiteral("%1-%2-%3.xlsx").arg(profile,it.key()).arg(count);
                if (!OfficeTemplateService::renderToFile(doc,output.filePath(name),&error)) {
                    QTextStream(stderr) << name << " " << error << Qt::endl;
                    return 1;
                }
            }
        }
    }
    if (args.size() == 5) return 0;
    QString error;
    if (!XlsxExporter::writeWorkbook(output.filePath("list.xlsx"),
            {{"列表",{"数量"},{{1}}},{"列表",{"批次"},{{"001"}}}},&error)) {
        QTextStream(stderr) << error << Qt::endl;
        return 1;
    }
    return 0;
}
