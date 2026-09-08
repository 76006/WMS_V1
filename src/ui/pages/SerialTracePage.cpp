#include "ui/pages/SerialTracePage.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

#include <utility>

namespace { constexpr int SerialIdRole = Qt::UserRole + 1; }

SerialTracePage::SerialTracePage(QSqlDatabase database, QWidget *parent)
    : QWidget(parent), m_database(std::move(database))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);
    auto *summaryPanel = new QFrame(this);
    summaryPanel->setObjectName(QStringLiteral("panel"));
    auto *summaryLayout = new QVBoxLayout(summaryPanel);
    summaryLayout->addWidget(new QLabel(QStringLiteral("SN 当前状态与位置"), summaryPanel));
    auto *filters = new QHBoxLayout;
    m_keywordEdit = new QLineEdit(summaryPanel);
    m_keywordEdit->setPlaceholderText(QStringLiteral("SN、物料编码、名称或批次"));
    m_statusCombo = new QComboBox(summaryPanel);
    m_statusCombo->addItem(QStringLiteral("全部状态"), QString());
    m_statusCombo->addItem(QStringLiteral("在库"), QStringLiteral("IN_STOCK"));
    m_statusCombo->addItem(QStringLiteral("已出库"), QStringLiteral("OUTBOUND"));
    m_statusCombo->addItem(QStringLiteral("已消耗"), QStringLiteral("CONSUMED"));
    m_statusCombo->addItem(QStringLiteral("已报废"), QStringLiteral("SCRAPPED"));
    m_statusCombo->addItem(QStringLiteral("已作废"), QStringLiteral("VOIDED"));
    m_warehouseCombo = new QComboBox(summaryPanel);
    auto *search = new QPushButton(QStringLiteral("查询"), summaryPanel);
    filters->addWidget(m_keywordEdit, 1);
    filters->addWidget(m_statusCombo);
    filters->addWidget(m_warehouseCombo);
    filters->addWidget(search);
    summaryLayout->addLayout(filters);
    m_serialTable = new QTableWidget(0, 11, summaryPanel);
    m_serialTable->setHorizontalHeaderLabels({QStringLiteral("SN"), QStringLiteral("物料编码"),
                                              QStringLiteral("物料名称"), QStringLiteral("批次"),
                                              QStringLiteral("生产批次"), QStringLiteral("状态"),
                                              QStringLiteral("仓库"), QStringLiteral("库位"),
                                              QStringLiteral("入库时间"), QStringLiteral("出库时间"),
                                              QStringLiteral("最近单据")});
    m_serialTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_serialTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_serialTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_serialTable->verticalHeader()->setVisible(false);
    m_serialTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_serialTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    summaryLayout->addWidget(m_serialTable, 1);
    root->addWidget(summaryPanel, 1);

    auto *historyPanel = new QFrame(this);
    historyPanel->setObjectName(QStringLiteral("panel"));
    auto *historyLayout = new QVBoxLayout(historyPanel);
    m_relationLabel = new QLabel(QStringLiteral("请选择一个SN查看生产关联。"), historyPanel);
    m_relationLabel->setWordWrap(true);
    m_relationLabel->setObjectName(QStringLiteral("mutedText"));
    historyLayout->addWidget(m_relationLabel);
    historyLayout->addWidget(new QLabel(QStringLiteral("相关业务附件"), historyPanel));
    m_attachmentTable = new QTableWidget(0, 4, historyPanel);
    m_attachmentTable->setHorizontalHeaderLabels({QStringLiteral("单据号"), QStringLiteral("业务类型"),
                                                   QStringLiteral("附件名"), QStringLiteral("上传时间")});
    m_attachmentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_attachmentTable->verticalHeader()->setVisible(false);
    m_attachmentTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_attachmentTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_attachmentTable->setMaximumHeight(125);
    historyLayout->addWidget(m_attachmentTable);
    historyLayout->addWidget(new QLabel(QStringLiteral("所选 SN 生命周期流水"), historyPanel));
    m_historyTable = new QTableWidget(0, 9, historyPanel);
    m_historyTable->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("单据号"),
                                               QStringLiteral("业务类型"), QStringLiteral("入库"),
                                               QStringLiteral("出库"), QStringLiteral("变动前"),
                                               QStringLiteral("变动后"), QStringLiteral("仓库/库位"),
                                               QStringLiteral("生产批次")});
    m_historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_historyTable->verticalHeader()->setVisible(false);
    m_historyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_historyTable->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Stretch);
    historyLayout->addWidget(m_historyTable);
    root->addWidget(historyPanel, 1);

    connect(search, &QPushButton::clicked, this, &SerialTracePage::loadSerials);
    connect(m_keywordEdit, &QLineEdit::returnPressed, this, &SerialTracePage::loadSerials);
    connect(m_statusCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SerialTracePage::loadSerials);
    connect(m_warehouseCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SerialTracePage::loadSerials);
    connect(m_serialTable, &QTableWidget::itemSelectionChanged, this, &SerialTracePage::loadHistory);
    refresh();
}

void SerialTracePage::refresh()
{
    const QVariant previous = m_warehouseCombo->currentData();
    m_warehouseCombo->blockSignals(true);
    m_warehouseCombo->clear();
    m_warehouseCombo->addItem(QStringLiteral("全部仓库"), 0);
    QSqlQuery warehouses(m_database);
    warehouses.exec(QStringLiteral("SELECT id,code,name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (warehouses.next())
        m_warehouseCombo->addItem(QStringLiteral("%1 - %2").arg(warehouses.value(1).toString(),
                                                                 warehouses.value(2).toString()),
                                  warehouses.value(0));
    const int selected = m_warehouseCombo->findData(previous);
    if (selected >= 0) m_warehouseCombo->setCurrentIndex(selected);
    m_warehouseCombo->blockSignals(false);
    loadSerials();
}

void SerialTracePage::loadSerials()
{
    m_serialTable->setRowCount(0);
    m_historyTable->setRowCount(0);
    m_attachmentTable->setRowCount(0);
    m_relationLabel->setText(QStringLiteral("请选择一个SN查看生产关联。"));
    const QString keyword = QStringLiteral("%%1%").arg(m_keywordEdit->text().trimmed());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT sn.id,sn.serial_no,m.code,m.name,sn.batch_no,sn.production_batch,sn.status,"
        "COALESCE(w.code,''),COALESCE(l.code,''),sn.inbound_at,COALESCE(sn.outbound_at,''),"
        "COALESCE(d.document_no,'') "
        "FROM serial_numbers sn JOIN materials m ON m.id=sn.material_id "
        "LEFT JOIN warehouses w ON w.id=sn.warehouse_id LEFT JOIN locations l ON l.id=sn.location_id "
        "LEFT JOIN business_documents d ON d.id=sn.last_document_id "
        "WHERE (sn.serial_no LIKE ? OR m.code LIKE ? OR m.name LIKE ? OR sn.batch_no LIKE ?) "
        "AND (?='' OR sn.status=?) AND (?=0 OR sn.warehouse_id=?) ORDER BY sn.id DESC"));
    for (int i = 0; i < 4; ++i) query.addBindValue(keyword);
    query.addBindValue(m_statusCombo->currentData());
    query.addBindValue(m_statusCombo->currentData());
    query.addBindValue(m_warehouseCombo->currentData());
    query.addBindValue(m_warehouseCombo->currentData());
    query.exec();
    while (query.next()) {
        const int row = m_serialTable->rowCount();
        m_serialTable->insertRow(row);
        auto *serial = new QTableWidgetItem(query.value(1).toString());
        serial->setData(SerialIdRole, query.value(0));
        m_serialTable->setItem(row, 0, serial);
        for (int column = 1; column < 11; ++column)
            m_serialTable->setItem(row, column,
                new QTableWidgetItem(query.value(column + 1).toString()));
    }
    if (m_serialTable->rowCount() > 0) m_serialTable->selectRow(0);
}

void SerialTracePage::loadHistory()
{
    m_historyTable->setRowCount(0);
    m_attachmentTable->setRowCount(0);
    const int row = m_serialTable->currentRow();
    if (row < 0) {
        m_relationLabel->setText(QStringLiteral("请选择一个SN查看生产关联。"));
        return;
    }
    const qlonglong serialId = m_serialTable->item(row, 0)->data(SerialIdRole).toLongLong();
    const QString productionBatch = m_serialTable->item(row, 4)->text();
    qlonglong productionRunId = 0;
    if (productionBatch.isEmpty()) {
        m_relationLabel->setText(QStringLiteral("该SN尚未关联生产批次。"));
    } else {
        QSqlQuery run(m_database);
        run.prepare(QStringLiteral(
            "SELECT pr.id,pm.code,pm.name FROM production_runs pr "
            "JOIN materials pm ON pm.id=pr.product_material_id WHERE pr.batch_no=?"));
        run.addBindValue(productionBatch);
        if (run.exec() && run.next()) {
            productionRunId = run.value(0).toLongLong();
            QSqlQuery materials(m_database);
            materials.prepare(QStringLiteral(
                "SELECT COALESCE(GROUP_CONCAT(DISTINCT m.code||'（'||"
                "CASE WHEN i.batch_no='' THEN '无批次' ELSE i.batch_no END||'）'),'无') "
                "FROM business_documents d JOIN business_document_items i ON i.document_id=d.id "
                "JOIN materials m ON m.id=i.material_id "
                "WHERE d.production_run_id=? AND d.document_type='SCLL'"));
            materials.addBindValue(productionRunId);
            QString keyMaterialBatches = QStringLiteral("无");
            if (materials.exec() && materials.next()) keyMaterialBatches = materials.value(0).toString();
            m_relationLabel->setText(QStringLiteral("所属产品：%1 - %2    生产批次：%3\n"
                                                     "实际领用物料批次：%4")
                                         .arg(run.value(1).toString(), run.value(2).toString(),
                                              productionBatch, keyMaterialBatches));
        } else {
            m_relationLabel->setText(QStringLiteral("生产批次：%1（未找到对应生产批次资料）")
                                         .arg(productionBatch));
        }
    }
    QSqlQuery attachments(m_database);
    attachments.prepare(QStringLiteral(
        "SELECT DISTINCT d.document_no,d.document_type,a.original_file_name,a.uploaded_at "
        "FROM business_documents d JOIN attachments a "
        "ON a.business_type='business_document' AND a.business_id=d.id "
        "WHERE a.is_deleted=0 AND ((? > 0 AND d.production_run_id=?) OR EXISTS("
        "SELECT 1 FROM inventory_ledger_serials x JOIN inventory_ledger il ON il.id=x.ledger_id "
        "WHERE x.serial_id=? AND il.document_id=d.id)) ORDER BY a.id DESC"));
    attachments.addBindValue(productionRunId);
    attachments.addBindValue(productionRunId);
    attachments.addBindValue(serialId);
    if (attachments.exec()) {
        while (attachments.next()) {
            const int target = m_attachmentTable->rowCount();
            m_attachmentTable->insertRow(target);
            for (int column = 0; column < 4; ++column)
                m_attachmentTable->setItem(target, column,
                    new QTableWidgetItem(attachments.value(column).toString()));
        }
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT l.occurred_at,d.document_no,l.business_type,l.quantity_in,l.quantity_out,"
        "l.quantity_before,l.quantity_after,w.code||' / '||loc.code,COALESCE(pr.batch_no,'') "
        "FROM inventory_ledger_serials x JOIN inventory_ledger l ON l.id=x.ledger_id "
        "JOIN business_documents d ON d.id=l.document_id JOIN warehouses w ON w.id=l.warehouse_id "
        "JOIN locations loc ON loc.id=l.location_id LEFT JOIN production_runs pr ON pr.id=d.production_run_id "
        "WHERE x.serial_id=? ORDER BY l.id"));
    query.addBindValue(serialId);
    query.exec();
    while (query.next()) {
        const int target = m_historyTable->rowCount();
        m_historyTable->insertRow(target);
        for (int column = 0; column < 9; ++column)
            m_historyTable->setItem(target, column,
                                    new QTableWidgetItem(query.value(column).toString()));
    }
}
