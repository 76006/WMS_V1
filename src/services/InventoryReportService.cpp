#include "services/InventoryReportService.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <utility>

namespace {
void setReportError(QString *target, const QString &message)
{
    if (target) *target = message;
}

QString likeKeyword(const QString &keyword)
{
    return QStringLiteral("%%1%").arg(keyword.trimmed());
}
}

InventoryReportService::InventoryReportService(QSqlDatabase database)
    : m_database(std::move(database))
{
}

bool InventoryReportService::loadAnnual(int year,
                                        const QString &keyword,
                                        QList<AnnualInventoryReportRow> *rows,
                                        QString *errorMessage) const
{
    if (!rows || !m_database.isOpen() || year < 1900 || year > 9999) {
        setReportError(errorMessage, QStringLiteral("年度统计参数或数据库连接无效。"));
        return false;
    }
    rows->clear();
    for (int month = 1; month <= 12; ++month) {
        AnnualInventoryReportRow row;
        row.month = month;
        rows->append(row);
    }

    const QDate from(year, 1, 1);
    const QDate to = from.addYears(1);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT CAST(strftime('%m',d.document_date) AS INTEGER), "
        "SUM(CASE WHEN d.document_type='CGRK' THEN i.ordered_quantity ELSE 0 END), "
        "SUM(CASE WHEN d.document_type='CGRK' THEN l.quantity_in "
        "         WHEN d.document_type='CX' AND sd.document_type='CGRK' THEN -l.quantity_out "
        "         ELSE 0 END), "
        "SUM(CASE WHEN d.document_type='CGRK' THEN i.gift_quantity "
        "         WHEN d.document_type='CX' AND sd.document_type='CGRK' THEN -i.gift_quantity "
        "         ELSE 0 END), "
        "SUM(CASE WHEN d.stock_direction<>'TRANSFER' THEN l.quantity_in ELSE 0 END), "
        "SUM(CASE WHEN d.stock_direction<>'TRANSFER' THEN l.quantity_out ELSE 0 END) "
        "FROM inventory_ledger l "
        "JOIN business_documents d ON d.id=l.document_id "
        "JOIN business_document_items i ON i.id=l.document_item_id "
        "JOIN materials m ON m.id=l.material_id "
        "LEFT JOIN business_documents sd ON sd.id=d.source_document_id "
        "WHERE date(d.document_date)>=? AND date(d.document_date)<? "
        "AND (m.code LIKE ? OR m.name LIKE ?) "
        "GROUP BY CAST(strftime('%m',d.document_date) AS INTEGER)"));
    query.addBindValue(from.toString(Qt::ISODate));
    query.addBindValue(to.toString(Qt::ISODate));
    const QString pattern = likeKeyword(keyword);
    query.addBindValue(pattern);
    query.addBindValue(pattern);
    if (!query.exec()) {
        setReportError(errorMessage, QStringLiteral("读取年度出入库统计失败：%1")
                                         .arg(query.lastError().text()));
        return false;
    }
    while (query.next()) {
        const int month = query.value(0).toInt();
        if (month < 1 || month > rows->size()) continue;
        AnnualInventoryReportRow &row = (*rows)[month - 1];
        row.orderedQuantity = query.value(1).toDouble();
        row.purchaseReceivedQuantity = query.value(2).toDouble();
        row.giftQuantity = query.value(3).toDouble();
        row.billableQuantity = row.purchaseReceivedQuantity - row.giftQuantity;
        row.inboundQuantity = query.value(4).toDouble();
        row.outboundQuantity = query.value(5).toDouble();
    }
    return true;
}

bool InventoryReportService::loadMonthly(int year,
                                         int month,
                                         const QString &keyword,
                                         QList<MonthlyMaterialReportRow> *summaryRows,
                                         QList<InventoryMovementReportRow> *detailRows,
                                         QString *errorMessage) const
{
    const QDate from(year, month, 1);
    if (!summaryRows || !detailRows || !m_database.isOpen() || !from.isValid()) {
        setReportError(errorMessage, QStringLiteral("月度统计参数或数据库连接无效。"));
        return false;
    }
    summaryRows->clear();
    detailRows->clear();
    const QDate to = from.addMonths(1);
    const QString pattern = likeKeyword(keyword);

    QSqlQuery summary(m_database);
    summary.prepare(QStringLiteral(
        "WITH stats AS ("
        " SELECT l.material_id,"
        " SUM(CASE WHEN date(d.document_date)<:from THEN l.quantity_in-l.quantity_out ELSE 0 END) opening_qty,"
        " SUM(CASE WHEN date(d.document_date)>=:from AND date(d.document_date)<:to "
        "          AND d.document_type='CGRK' THEN i.ordered_quantity ELSE 0 END) ordered_qty,"
        " SUM(CASE WHEN date(d.document_date)>=:from AND date(d.document_date)<:to "
        "          AND d.document_type='CGRK' THEN l.quantity_in "
        "          WHEN date(d.document_date)>=:from AND date(d.document_date)<:to "
        "          AND d.document_type='CX' AND sd.document_type='CGRK' THEN -l.quantity_out ELSE 0 END) purchase_received,"
        " SUM(CASE WHEN date(d.document_date)>=:from AND date(d.document_date)<:to "
        "          AND d.document_type='CGRK' THEN i.gift_quantity "
        "          WHEN date(d.document_date)>=:from AND date(d.document_date)<:to "
        "          AND d.document_type='CX' AND sd.document_type='CGRK' THEN -i.gift_quantity ELSE 0 END) gift_qty,"
        " SUM(CASE WHEN date(d.document_date)>=:from AND date(d.document_date)<:to "
        "          AND d.stock_direction<>'TRANSFER' THEN l.quantity_in ELSE 0 END) inbound_qty,"
        " SUM(CASE WHEN date(d.document_date)>=:from AND date(d.document_date)<:to "
        "          AND d.stock_direction<>'TRANSFER' THEN l.quantity_out ELSE 0 END) outbound_qty,"
        " SUM(CASE WHEN date(d.document_date)>=:from AND date(d.document_date)<:to "
        "          AND d.document_type='CGRK' THEN l.quantity_in ELSE 0 END) purchase_gross_in"
        " FROM inventory_ledger l"
        " JOIN business_documents d ON d.id=l.document_id"
        " JOIN business_document_items i ON i.id=l.document_item_id"
        " LEFT JOIN business_documents sd ON sd.id=d.source_document_id"
        " WHERE date(d.document_date)<:to"
        " GROUP BY l.material_id"
        ") "
        "SELECT m.id,m.code,m.name,m.specification,m.unit,s.opening_qty,s.ordered_qty,"
        "s.purchase_received,s.gift_qty,s.inbound_qty-s.purchase_gross_in,s.inbound_qty,"
        "s.outbound_qty,s.opening_qty+s.inbound_qty-s.outbound_qty "
        "FROM stats s JOIN materials m ON m.id=s.material_id "
        "WHERE (m.code LIKE :keyword OR m.name LIKE :keyword) "
        "AND (ABS(s.opening_qty)>0.0000001 OR ABS(s.ordered_qty)>0.0000001 "
        " OR ABS(s.purchase_received)>0.0000001 OR ABS(s.gift_qty)>0.0000001 "
        " OR ABS(s.inbound_qty)>0.0000001 OR ABS(s.outbound_qty)>0.0000001) "
        "ORDER BY m.code"));
    summary.bindValue(QStringLiteral(":from"), from.toString(Qt::ISODate));
    summary.bindValue(QStringLiteral(":to"), to.toString(Qt::ISODate));
    summary.bindValue(QStringLiteral(":keyword"), pattern);
    if (!summary.exec()) {
        setReportError(errorMessage, QStringLiteral("读取月度物料汇总失败：%1")
                                         .arg(summary.lastError().text()));
        return false;
    }
    while (summary.next()) {
        MonthlyMaterialReportRow row;
        row.materialId = summary.value(0).toLongLong();
        row.materialCode = summary.value(1).toString();
        row.materialName = summary.value(2).toString();
        row.specification = summary.value(3).toString();
        row.unit = summary.value(4).toString();
        row.openingQuantity = summary.value(5).toDouble();
        row.orderedQuantity = summary.value(6).toDouble();
        row.purchaseReceivedQuantity = summary.value(7).toDouble();
        row.giftQuantity = summary.value(8).toDouble();
        row.billableQuantity = row.purchaseReceivedQuantity - row.giftQuantity;
        row.otherInboundQuantity = summary.value(9).toDouble();
        row.inboundQuantity = summary.value(10).toDouble();
        row.outboundQuantity = summary.value(11).toDouble();
        row.closingQuantity = summary.value(12).toDouble();
        summaryRows->append(row);
    }

    QSqlQuery detail(m_database);
    detail.prepare(QStringLiteral(
        "SELECT d.document_date,d.document_no,d.document_type,d.stock_direction,"
        "m.code,m.name,m.specification,m.unit,"
        "COALESCE(NULLIF(CASE WHEN d.document_type='CX' THEN sd.supplier ELSE d.supplier END,''),b.supplier,''),"
        "w.name,loc.code,l.batch_no,"
        "CASE WHEN d.document_type='CGRK' THEN i.ordered_quantity ELSE 0 END,"
        "CASE WHEN d.document_type='CGRK' THEN l.quantity_in "
        "     WHEN d.document_type='CX' AND sd.document_type='CGRK' THEN -l.quantity_out ELSE 0 END,"
        "CASE WHEN d.document_type='CGRK' THEN i.gift_quantity "
        "     WHEN d.document_type='CX' AND sd.document_type='CGRK' THEN -i.gift_quantity ELSE 0 END,"
        "l.quantity_in,l.quantity_out,d.handler_name,u.display_name,i.notes "
        "FROM inventory_ledger l "
        "JOIN business_documents d ON d.id=l.document_id "
        "JOIN business_document_items i ON i.id=l.document_item_id "
        "JOIN materials m ON m.id=l.material_id "
        "JOIN warehouses w ON w.id=l.warehouse_id "
        "JOIN locations loc ON loc.id=l.location_id "
        "JOIN users u ON u.id=l.operator_id "
        "LEFT JOIN business_documents sd ON sd.id=d.source_document_id "
        "LEFT JOIN batches b ON b.material_id=l.material_id AND b.batch_no=l.batch_no "
        "WHERE date(d.document_date)>=? AND date(d.document_date)<? "
        "AND d.stock_direction<>'TRANSFER' "
        "AND (m.code LIKE ? OR m.name LIKE ?) "
        "ORDER BY d.document_date,l.id"));
    detail.addBindValue(from.toString(Qt::ISODate));
    detail.addBindValue(to.toString(Qt::ISODate));
    detail.addBindValue(pattern);
    detail.addBindValue(pattern);
    if (!detail.exec()) {
        setReportError(errorMessage, QStringLiteral("读取月度出入库明细失败：%1")
                                         .arg(detail.lastError().text()));
        return false;
    }
    while (detail.next()) {
        InventoryMovementReportRow row;
        row.documentDate = QDate::fromString(detail.value(0).toString(), Qt::ISODate);
        row.documentNumber = detail.value(1).toString();
        row.documentType = detail.value(2).toString();
        row.direction = detail.value(3).toString();
        row.materialCode = detail.value(4).toString();
        row.materialName = detail.value(5).toString();
        row.specification = detail.value(6).toString();
        row.unit = detail.value(7).toString();
        row.supplier = detail.value(8).toString();
        row.warehouse = detail.value(9).toString();
        row.location = detail.value(10).toString();
        row.batchNumber = detail.value(11).toString();
        row.orderedQuantity = detail.value(12).toDouble();
        row.purchaseReceivedQuantity = detail.value(13).toDouble();
        row.giftQuantity = detail.value(14).toDouble();
        row.billableQuantity = row.purchaseReceivedQuantity - row.giftQuantity;
        row.inboundQuantity = detail.value(15).toDouble();
        row.outboundQuantity = detail.value(16).toDouble();
        row.handlerName = detail.value(17).toString();
        row.operatorName = detail.value(18).toString();
        row.notes = detail.value(19).toString();
        detailRows->append(row);
    }
    return true;
}

QString InventoryReportService::documentTypeName(const QString &documentType)
{
    const QString type = documentType.trimmed().toUpper();
    if (type == QStringLiteral("CGRK")) return QStringLiteral("采购入库");
    if (type == QStringLiteral("SCWG")) return QStringLiteral("生产完工入库");
    if (type == QStringLiteral("TLRK")) return QStringLiteral("退料入库");
    if (type == QStringLiteral("QTRK")) return QStringLiteral("其他入库");
    if (type == QStringLiteral("QC")) return QStringLiteral("期初入库");
    if (type == QStringLiteral("SCLL")) return QStringLiteral("生产领料");
    if (type == QStringLiteral("SCTL")) return QStringLiteral("生产退料");
    if (type == QStringLiteral("CPRK")) return QStringLiteral("成品入库");
    if (type == QStringLiteral("XSCK")) return QStringLiteral("销售出库");
    if (type == QStringLiteral("WXLY")) return QStringLiteral("维修领用");
    if (type == QStringLiteral("YPLY")) return QStringLiteral("研发领用");
    if (type == QStringLiteral("QTCK")) return QStringLiteral("其他出库");
    if (type == QStringLiteral("CX")) return QStringLiteral("撤销");
    if (type == QStringLiteral("PD")) return QStringLiteral("盘点调整");
    return type;
}

QString InventoryReportService::directionName(const QString &direction)
{
    const QString value = direction.trimmed().toUpper();
    if (value == QStringLiteral("IN")) return QStringLiteral("入库");
    if (value == QStringLiteral("OUT")) return QStringLiteral("出库");
    if (value == QStringLiteral("ADJUST")) return QStringLiteral("盘点调整");
    if (value == QStringLiteral("TRANSFER")) return QStringLiteral("内部调拨");
    return value;
}
