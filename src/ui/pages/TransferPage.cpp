#include "ui/pages/TransferPage.h"

#include "services/InventoryService.h"
#include "ui/widgets/StockLineTable.h"
#include "ui/widgets/TableExcelExport.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QVBoxLayout>

#include <utility>

namespace {
constexpr int ItemIdRole = Qt::UserRole + 1;
constexpr int RequireSerialRole = Qt::UserRole + 2;
constexpr int CreatorIdRole = Qt::UserRole + 3;

QStringList parseSerialNumbers(const QString &text)
{
    QStringList serials;
    for (const QString &value : text.split(QRegularExpression(QStringLiteral("[\\r\\n,;]+")),
                                           Qt::SkipEmptyParts)) {
        const QString serial = value.trimmed().toUpper();
        if (!serial.isEmpty() && !serials.contains(serial)) serials.append(serial);
    }
    return serials;
}
}

TransferPage::TransferPage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(20, 18, 20, 20);
    auto *heading = new QLabel(QStringLiteral("库存调拨"), panel);
    heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;"));
    layout->addWidget(heading);
    auto *hint = new QLabel(QStringLiteral("每次调拨一条库存记录；SN物料需选择全部调拨SN。"), panel);
    hint->setObjectName(QStringLiteral("mutedText"));
    layout->addWidget(hint);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_dateEdit = new QDateEdit(QDate::currentDate(), panel);
    m_dateEdit->setCalendarPopup(true);
    m_dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_handlerEdit = new QLineEdit(m_session.displayName, panel);
    m_targetWarehouse = new QComboBox(panel);
    m_targetLocation = new QComboBox(panel);
    m_notesEdit = new QTextEdit(panel);
    m_notesEdit->setMaximumHeight(65);
    form->addRow(QStringLiteral("调拨日期 *"), m_dateEdit);
    form->addRow(QStringLiteral("经办人员"), m_handlerEdit);
    form->addRow(QStringLiteral("目标仓库 *"), m_targetWarehouse);
    form->addRow(QStringLiteral("目标库位 *"), m_targetLocation);
    form->addRow(QStringLiteral("备注"), m_notesEdit);
    layout->addLayout(form);
    m_sourceLine = new StockLineTable(m_database, StockLineTable::Mode::Outbound, panel);
    layout->addWidget(m_sourceLine);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    m_submitButton = new QPushButton(QStringLiteral("确认调拨"), panel);
    m_submitButton->setProperty("primary", true);
    actions->addWidget(m_submitButton);
    layout->addLayout(actions);
    root->addWidget(panel);

    auto *recentPanel = new QFrame(this);
    recentPanel->setObjectName(QStringLiteral("panel"));
    auto *recentLayout = new QVBoxLayout(recentPanel);
    auto *recentToolbar = new QHBoxLayout;
    recentToolbar->addWidget(new QLabel(QStringLiteral("近期调拨单"), recentPanel));
    recentToolbar->addStretch();
    auto *fullScreenRecentButton = new QPushButton(QStringLiteral("全屏显示"), recentPanel);
    recentToolbar->addWidget(fullScreenRecentButton);
    m_reverseButton = new QPushButton(QStringLiteral("部分/全部撤销"), recentPanel);
    m_reverseButton->setProperty("danger", true);
    recentToolbar->addWidget(m_reverseButton);
    recentLayout->addLayout(recentToolbar);
    m_transferTable = new QTableWidget(0, 10, recentPanel);
    m_transferTable->setProperty("businessDocumentTable", true);
    m_transferTable->setHorizontalHeaderLabels({QStringLiteral("调拨单号"), QStringLiteral("日期"),
        QStringLiteral("物料"), QStringLiteral("批次"), QStringLiteral("原仓库/库位"),
        QStringLiteral("目标仓库/库位"), QStringLiteral("数量"), QStringLiteral("已撤销"),
        QStringLiteral("可撤销"), QStringLiteral("状态")});
    m_transferTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_transferTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_transferTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_transferTable->verticalHeader()->setVisible(false);
    m_transferTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_transferTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    recentLayout->addWidget(m_transferTable);
    root->addWidget(recentPanel, 1);
    connect(fullScreenRecentButton, &QPushButton::clicked, this, [this] {
        TableExcelExport::fullScreenTable(
            m_transferTable, QStringLiteral("全部调拨单"), this, [this] { refreshTransfers(); });
    });
    connect(m_targetWarehouse, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &TransferPage::loadTargetLocations);
    connect(m_submitButton, &QPushButton::clicked, this, &TransferPage::submit);
    connect(m_transferTable, &QTableWidget::itemSelectionChanged,
            this, &TransferPage::updateReversalState);
    connect(m_reverseButton, &QPushButton::clicked,
            this, &TransferPage::reverseSelectedTransfer);
    refreshReferenceData();
}

void TransferPage::refreshReferenceData()
{
    const QVariant previous = m_targetWarehouse->currentData();
    m_targetWarehouse->blockSignals(true);
    m_targetWarehouse->clear();
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("SELECT id,code,name FROM warehouses WHERE is_active=1 ORDER BY code"));
    while (query.next())
        m_targetWarehouse->addItem(QStringLiteral("%1 - %2").arg(query.value(1).toString(),
                                                                  query.value(2).toString()),
                                   query.value(0));
    const int selected = m_targetWarehouse->findData(previous);
    if (selected >= 0) m_targetWarehouse->setCurrentIndex(selected);
    m_targetWarehouse->blockSignals(false);
    loadTargetLocations();
    m_sourceLine->refreshReferenceData();
    m_submitButton->setEnabled(m_session.canManageWarehouse());
    refreshTransfers();
}

void TransferPage::loadTargetLocations()
{
    const QVariant previous = m_targetLocation->currentData();
    m_targetLocation->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id,code,name FROM locations WHERE warehouse_id=? AND is_active=1 ORDER BY code"));
    query.addBindValue(m_targetWarehouse->currentData());
    query.exec();
    while (query.next())
        m_targetLocation->addItem(QStringLiteral("%1 - %2").arg(query.value(1).toString(),
                                                                 query.value(2).toString()),
                                  query.value(0));
    const int selected = m_targetLocation->findData(previous);
    if (selected >= 0) m_targetLocation->setCurrentIndex(selected);
}

void TransferPage::refreshTransfers()
{
    m_transferTable->setRowCount(0);
    QSqlQuery query(m_database);
    query.exec(QStringLiteral(
        "SELECT i.id,d.document_no,d.document_date,m.code||' - '||m.name,i.batch_no,"
        "sw.code||' / '||sl.code,tw.code||' / '||tl.code,i.quantity,i.reversed_quantity,"
        "i.quantity-i.reversed_quantity,d.status,m.require_serial,d.created_by,d.id "
        "FROM business_documents d JOIN business_document_items i ON i.document_id=d.id "
        "JOIN materials m ON m.id=i.material_id JOIN warehouses sw ON sw.id=i.warehouse_id "
        "JOIN locations sl ON sl.id=i.location_id JOIN warehouses tw ON tw.id=i.target_warehouse_id "
        "JOIN locations tl ON tl.id=i.target_location_id "
        "WHERE d.document_type='DB' AND d.stock_direction='TRANSFER' "
        "ORDER BY d.id DESC")
        + (m_transferTable->property("tableFullScreenActive").toBool() ? QString() : QStringLiteral(" LIMIT 30")));
    while (query.next()) {
        const int row = m_transferTable->rowCount();
        m_transferTable->insertRow(row);
        auto *number = new QTableWidgetItem(query.value(1).toString());
        number->setData(Qt::UserRole, query.value(13));
        number->setData(ItemIdRole, query.value(0));
        number->setData(RequireSerialRole, query.value(11));
        number->setData(CreatorIdRole, query.value(12));
        m_transferTable->setItem(row, 0, number);
        for (int column = 1; column <= 8; ++column)
            m_transferTable->setItem(row, column,
                                     new QTableWidgetItem(query.value(column + 1).toString()));
        const QString status = query.value(10).toString();
        m_transferTable->setItem(row, 9, new QTableWidgetItem(
            status == QStringLiteral("REVERSED") ? QStringLiteral("已全部撤销")
            : status == QStringLiteral("PARTIALLY_REVERSED") ? QStringLiteral("部分撤销")
                                                             : QStringLiteral("已生效")));
    }
    if (m_transferTable->rowCount() > 0) m_transferTable->selectRow(0);
    updateReversalState();
}

void TransferPage::updateReversalState()
{
    const int row = m_transferTable->currentRow();
    bool enabled = false;
    QString reason = QStringLiteral("请选择一张可撤销的调拨单");
    if (row >= 0) {
        const auto *number = m_transferTable->item(row, 0);
        const double remaining = m_transferTable->item(row, 8)->text().toDouble();
        const bool allowedOwner = m_session.isAdministrator()
            || number->data(CreatorIdRole).toLongLong() == m_session.userId;
        enabled = m_session.canManageWarehouse() && allowedOwner && remaining > 0.0000001;
        if (!allowedOwner) reason = QStringLiteral("只有管理员或原调拨单创建人可以撤销");
        else if (remaining <= 0.0000001) reason = QStringLiteral("该调拨单已无可撤销数量");
        else reason.clear();
    }
    m_reverseButton->setEnabled(enabled);
    m_reverseButton->setToolTip(reason);
}

void TransferPage::submit()
{
    QString error;
    const QList<StockMovementRequest> lines = m_sourceLine->lines(&error);
    if (lines.size() != 1) {
        QMessageBox::warning(this, QStringLiteral("调拨明细有误"),
                             lines.isEmpty() && !error.isEmpty()
                                 ? error : QStringLiteral("每张调拨单必须且只能保留一条物料明细。"));
        return;
    }
    if (m_targetLocation->currentIndex() < 0) {
        QMessageBox::warning(this, QStringLiteral("资料不完整"), QStringLiteral("请选择目标库位。"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("确认调拨"),
        QStringLiteral("确认将所选库存调拨到目标库位？")) != QMessageBox::Yes) return;
    TransferRequest request;
    static_cast<StockMovementRequest &>(request) = lines.first();
    request.documentType = QStringLiteral("DB");
    request.documentDate = m_dateEdit->date();
    request.handlerName = m_handlerEdit->text().trimmed();
    request.notes = m_notesEdit->toPlainText().trimmed();
    request.targetWarehouseId = m_targetWarehouse->currentData().toLongLong();
    request.targetLocationId = m_targetLocation->currentData().toLongLong();
    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    if (!service.postTransfer(request, &posted, &error)) {
        QMessageBox::warning(this, QStringLiteral("调拨失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("调拨完成"),
                             QStringLiteral("调拨单 %1 已生效。").arg(posted.documentNumber));
    m_notesEdit->clear();
    m_sourceLine->clearLines();
    emit stockChanged();
    refreshReferenceData();
}

void TransferPage::reverseSelectedTransfer()
{
    const int row = m_transferTable->currentRow();
    if (row < 0 || !m_reverseButton->isEnabled()) return;
    const QTableWidgetItem *numberItem = m_transferTable->item(row, 0);
    const qlonglong itemId = numberItem->data(ItemIdRole).toLongLong();
    const bool requireSerial = numberItem->data(RequireSerialRole).toBool();
    const double maximum = m_transferTable->item(row, 8)->text().toDouble();

    QStringList availableSerials;
    if (requireSerial) {
        QSqlQuery serials(m_database);
        serials.prepare(QStringLiteral(
            "SELECT DISTINCT sn.serial_no FROM serial_numbers sn "
            "JOIN inventory_ledger_serials x ON x.serial_id=sn.id "
            "JOIN inventory_ledger l ON l.id=x.ledger_id "
            "JOIN business_document_items i ON i.id=l.document_item_id "
            "WHERE i.id=? AND l.business_type='DB-IN' AND sn.status='IN_STOCK' "
            "AND sn.warehouse_id=i.target_warehouse_id AND sn.location_id=i.target_location_id "
            "ORDER BY sn.serial_no"));
        serials.addBindValue(itemId);
        if (serials.exec()) while (serials.next()) availableSerials.append(serials.value(0).toString());
        if (availableSerials.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("无法撤销"),
                                 QStringLiteral("原调拨SN已不在目标库位，无法撤销。"));
            return;
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("撤销调拨"));
    dialog.setMinimumWidth(540);
    auto *root = new QVBoxLayout(&dialog);
    auto *summary = new QLabel(QStringLiteral("原调拨单：%1\n物料：%2\n"
                                               "撤销后库存将从目标库位退回原库位，并生成反向流水。")
                                   .arg(numberItem->text(), m_transferTable->item(row, 2)->text()),
                               &dialog);
    summary->setWordWrap(true);
    root->addWidget(summary);
    auto *form = new QFormLayout;
    auto *date = new QDateEdit(QDate::currentDate(), &dialog);
    date->setCalendarPopup(true);
    date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    auto *quantity = new QDoubleSpinBox(&dialog);
    quantity->setDecimals(requireSerial ? 0 : 6);
    quantity->setSingleStep(requireSerial ? 1.0 : 0.1);
    const double availableMaximum = requireSerial
        ? qMin(maximum, static_cast<double>(availableSerials.size())) : maximum;
    quantity->setRange(requireSerial ? 1.0 : 0.000001, availableMaximum);
    quantity->setValue(availableMaximum);
    auto *handler = new QLineEdit(m_session.displayName, &dialog);
    auto *notes = new QTextEdit(&dialog);
    notes->setMaximumHeight(70);
    form->addRow(QStringLiteral("撤销日期 *"), date);
    form->addRow(QStringLiteral("撤销数量 *"), quantity);
    form->addRow(QStringLiteral("经办人员"), handler);
    QTextEdit *serialEdit = nullptr;
    if (requireSerial) {
        serialEdit = new QTextEdit(&dialog);
        serialEdit->setPlainText(availableSerials.join(QLatin1Char('\n')));
        serialEdit->setPlaceholderText(QStringLiteral("每行一个需要退回原库位的SN"));
        serialEdit->setMaximumHeight(120);
        form->addRow(QStringLiteral("SN列表 *"), serialEdit);
    }
    form->addRow(QStringLiteral("撤销原因 *"), notes);
    root->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确认撤销"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (notes->toPlainText().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少原因"), QStringLiteral("请填写撤销原因。"));
        return;
    }
    QStringList selectedSerials;
    if (serialEdit) {
        selectedSerials = parseSerialNumbers(serialEdit->toPlainText());
        if (selectedSerials.size() != static_cast<int>(quantity->value())) {
            QMessageBox::warning(this, QStringLiteral("SN数量不一致"),
                                 QStringLiteral("SN数量必须与撤销数量一致。"));
            return;
        }
    }
    if (QMessageBox::question(this, QStringLiteral("再次确认"),
        QStringLiteral("确定撤销调拨单 %1，数量 %2？")
            .arg(numberItem->text(), QString::number(quantity->value(), 'f', requireSerial ? 0 : 6)))
        != QMessageBox::Yes) return;

    ReversalRequest request;
    request.sourceItemId = itemId;
    request.documentDate = date->date();
    request.quantity = quantity->value();
    request.handlerName = handler->text().trimmed();
    request.notes = notes->toPlainText().trimmed();
    request.serialNumbers = selectedSerials;
    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    QString error;
    if (!service.reverseTransfer(request, &posted, &error)) {
        QMessageBox::warning(this, QStringLiteral("撤销失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("撤销完成"),
                             QStringLiteral("已生成反向调拨单：%1").arg(posted.documentNumber));
    emit stockChanged();
    refreshReferenceData();
}
