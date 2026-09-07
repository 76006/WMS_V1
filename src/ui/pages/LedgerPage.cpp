#include "ui/pages/LedgerPage.h"

#include "services/InventoryService.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QTableView>
#include <QTextEdit>
#include <QVBoxLayout>

#include <utility>

namespace {
QStringList parseSerialNumbers(const QString &text)
{
    QStringList values = text.split(QRegularExpression(QStringLiteral("[,;\\r\\n]+")), Qt::SkipEmptyParts);
    for (QString &value : values) {
        value = value.trimmed().toUpper();
    }
    return values;
}
}

LedgerPage::LedgerPage(QSqlDatabase database, Session session, QWidget *parent)
    : QWidget(parent), m_database(std::move(database)), m_session(std::move(session))
{
    setObjectName(QStringLiteral("pageRoot"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *toolbar = new QHBoxLayout;
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("单据号、物料编码、名称、批次或SN"));
    m_searchEdit->setClearButtonEnabled(true);
    m_typeCombo = new QComboBox(this);
    m_typeCombo->addItem(QStringLiteral("全部业务"), QVariant());
    m_typeCombo->addItem(QStringLiteral("采购入库"), QStringLiteral("CGRK"));
    m_typeCombo->addItem(QStringLiteral("生产完工入库"), QStringLiteral("SCWG"));
    m_typeCombo->addItem(QStringLiteral("退料入库"), QStringLiteral("TLRK"));
    m_typeCombo->addItem(QStringLiteral("其他入库"), QStringLiteral("QTRK"));
    m_typeCombo->addItem(QStringLiteral("期初入库"), QStringLiteral("QC"));
    m_typeCombo->addItem(QStringLiteral("生产领料"), QStringLiteral("SCLL"));
    m_typeCombo->addItem(QStringLiteral("生产退料"), QStringLiteral("SCTL"));
    m_typeCombo->addItem(QStringLiteral("成品入库"), QStringLiteral("CPRK"));
    m_typeCombo->addItem(QStringLiteral("销售出库"), QStringLiteral("XSCK"));
    m_typeCombo->addItem(QStringLiteral("其他出库"), QStringLiteral("QTCK"));
    m_typeCombo->addItem(QStringLiteral("库存调拨"), QStringLiteral("DB"));
    m_typeCombo->addItem(QStringLiteral("撤销"), QStringLiteral("CX"));
    m_fromDate = new QDateEdit(QDate::currentDate().addMonths(-1), this);
    m_fromDate->setCalendarPopup(true);
    m_fromDate->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_toDate = new QDateEdit(QDate::currentDate(), this);
    m_toDate->setCalendarPopup(true);
    m_toDate->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    auto *searchButton = new QPushButton(QStringLiteral("查询"), this);
    searchButton->setProperty("primary", true);
    auto *clearButton = new QPushButton(QStringLiteral("近30天"), this);
    m_reverseButton = new QPushButton(QStringLiteral("部分/全部撤销"), this);
    m_reverseButton->setProperty("danger", true);
    m_reverseButton->setEnabled(false);
    toolbar->addWidget(m_searchEdit, 1);
    toolbar->addWidget(m_typeCombo);
    toolbar->addWidget(new QLabel(QStringLiteral("从"), this));
    toolbar->addWidget(m_fromDate);
    toolbar->addWidget(new QLabel(QStringLiteral("至"), this));
    toolbar->addWidget(m_toDate);
    toolbar->addWidget(searchButton);
    toolbar->addWidget(clearButton);
    toolbar->addSpacing(10);
    toolbar->addWidget(m_reverseButton);
    root->addLayout(toolbar);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(12, 12, 12, 12);
    m_table = new QTableView(panel);
    m_model = new QSqlQueryModel(this);
    m_table->setModel(m_model);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    panelLayout->addWidget(m_table);
    root->addWidget(panel, 1);

    connect(searchButton, &QPushButton::clicked, this, &LedgerPage::refresh);
    connect(clearButton, &QPushButton::clicked, this, &LedgerPage::clearFilters);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &LedgerPage::refresh);
    connect(m_typeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &LedgerPage::refresh);
    connect(m_reverseButton, &QPushButton::clicked, this, &LedgerPage::reverseSelectedItem);
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &LedgerPage::updateActionState);
    refresh();
}

void LedgerPage::clearFilters()
{
    m_searchEdit->clear();
    m_typeCombo->setCurrentIndex(0);
    m_fromDate->setDate(QDate::currentDate().addMonths(-1));
    m_toDate->setDate(QDate::currentDate());
    refresh();
}

void LedgerPage::refresh()
{
    if (m_fromDate->date() > m_toDate->date()) {
        m_fromDate->setDate(m_toDate->date());
    }
    const QString keyword = QStringLiteral("%%1%").arg(m_searchEdit->text().trimmed());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT l.id, i.id, d.id, l.occurred_at, l.business_type, d.document_no, "
        "m.code, m.name, l.batch_no, "
        "COALESCE((SELECT group_concat(sn.serial_no, ', ') FROM inventory_ledger_serials ils "
        " JOIN serial_numbers sn ON sn.id=ils.serial_id WHERE ils.ledger_id=l.id), ''), "
        "l.quantity_in, l.quantity_out, l.quantity_before, l.quantity_after, "
        "w.name, loc.code, u.display_name, d.stock_direction, d.status, "
        "i.quantity-i.reversed_quantity-CASE WHEN d.document_type='SCLL' THEN i.returned_quantity ELSE 0 END, "
        "m.require_serial, d.created_by "
        "FROM inventory_ledger l "
        "JOIN business_documents d ON d.id=l.document_id "
        "JOIN business_document_items i ON i.id=l.document_item_id "
        "JOIN materials m ON m.id=l.material_id "
        "JOIN warehouses w ON w.id=l.warehouse_id "
        "JOIN locations loc ON loc.id=l.location_id "
        "JOIN users u ON u.id=l.operator_id "
        "WHERE date(l.occurred_at) BETWEEN ? AND ? "
        "AND (? IS NULL OR d.document_type=?) "
        "AND (d.document_no LIKE ? OR m.code LIKE ? OR m.name LIKE ? OR l.batch_no LIKE ? "
        " OR EXISTS(SELECT 1 FROM inventory_ledger_serials x JOIN serial_numbers sn ON sn.id=x.serial_id "
        " WHERE x.ledger_id=l.id AND sn.serial_no LIKE ?)) "
        "ORDER BY l.id DESC LIMIT 1000"));
    query.addBindValue(m_fromDate->date().toString(Qt::ISODate));
    query.addBindValue(m_toDate->date().toString(Qt::ISODate));
    const QVariant type = m_typeCombo->currentData();
    query.addBindValue(type);
    query.addBindValue(type);
    for (int i = 0; i < 5; ++i) query.addBindValue(keyword);
    query.exec();
    m_model->setQuery(std::move(query));

    const QStringList headers = {
        QStringLiteral("流水ID"), QStringLiteral("明细ID"), QStringLiteral("单据ID"),
        QStringLiteral("日期时间"), QStringLiteral("业务类型"), QStringLiteral("单据号"),
        QStringLiteral("物料编码"), QStringLiteral("物料名称"), QStringLiteral("批次"),
        QStringLiteral("SN"), QStringLiteral("入库数量"), QStringLiteral("出库数量"),
        QStringLiteral("操作前库存"), QStringLiteral("操作后库存"), QStringLiteral("仓库"),
        QStringLiteral("库位"), QStringLiteral("操作人员"), QStringLiteral("方向"),
        QStringLiteral("单据状态"), QStringLiteral("可撤销数量"), QStringLiteral("SN管理"),
        QStringLiteral("创建人ID")
    };
    for (int column = 0; column < headers.size(); ++column) {
        m_model->setHeaderData(column, Qt::Horizontal, headers.at(column));
    }
    for (int hidden : {0, 1, 2, 17, 19, 20, 21}) {
        m_table->hideColumn(hidden);
    }
    m_table->resizeColumnsToContents();
    m_table->setColumnWidth(7, qMax(m_table->columnWidth(7), 150));
    m_table->setColumnWidth(9, qMax(m_table->columnWidth(9), 160));
    updateActionState();
}

void LedgerPage::updateActionState()
{
    const QModelIndex current = m_table->currentIndex();
    bool enabled = false;
    QString reason = QStringLiteral("请选择一条可撤销的入库或出库流水");
    if (current.isValid()) {
        const int row = current.row();
        const QString direction = m_model->index(row, 17).data().toString();
        const QString status = m_model->index(row, 18).data().toString();
        const double remaining = m_model->index(row, 19).data().toDouble();
        const qlonglong creatorId = m_model->index(row, 21).data().toLongLong();
        const bool allowedOwner = m_session.isAdministrator() || creatorId == m_session.userId;
        enabled = m_session.canManageWarehouse() && allowedOwner
                  && (direction == QStringLiteral("IN") || direction == QStringLiteral("OUT"))
                  && status != QStringLiteral("REVERSED") && remaining > 0.0000001;
        if (!allowedOwner) reason = QStringLiteral("只有管理员或原单创建人可以撤销");
        else if (direction != QStringLiteral("IN") && direction != QStringLiteral("OUT"))
            reason = QStringLiteral("调拨流水需在调拨页面撤销");
        else if (remaining <= 0.0000001 || status == QStringLiteral("REVERSED"))
            reason = QStringLiteral("该明细已无可撤销数量");
        else reason.clear();
    }
    m_reverseButton->setEnabled(enabled);
    m_reverseButton->setToolTip(reason);
}

void LedgerPage::reverseSelectedItem()
{
    const QModelIndex current = m_table->currentIndex();
    if (!current.isValid() || !m_reverseButton->isEnabled()) return;
    const int row = current.row();
    const qlonglong itemId = m_model->index(row, 1).data().toLongLong();
    const QString documentNumber = m_model->index(row, 5).data().toString();
    const QString material = QStringLiteral("%1 - %2")
                                 .arg(m_model->index(row, 6).data().toString(),
                                      m_model->index(row, 7).data().toString());
    const double maximum = m_model->index(row, 19).data().toDouble();
    const bool requireSerial = m_model->index(row, 20).data().toBool();
    const QString originalSerials = m_model->index(row, 9).data().toString();

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("撤销库存业务"));
    dialog.setMinimumWidth(520);
    auto *root = new QVBoxLayout(&dialog);
    auto *warning = new QLabel(
        QStringLiteral("原单：%1\n物料：%2\n撤销会生成新的反向单据和库存流水，原记录不会删除。")
            .arg(documentNumber, material), &dialog);
    warning->setWordWrap(true);
    root->addWidget(warning);
    auto *form = new QFormLayout;
    auto *dateEdit = new QDateEdit(QDate::currentDate(), &dialog);
    dateEdit->setCalendarPopup(true);
    dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    auto *quantitySpin = new QDoubleSpinBox(&dialog);
    quantitySpin->setDecimals(6);
    quantitySpin->setRange(0.000001, maximum);
    quantitySpin->setValue(maximum);
    auto *handlerEdit = new QLineEdit(m_session.displayName, &dialog);
    auto *notesEdit = new QTextEdit(&dialog);
    notesEdit->setMaximumHeight(70);
    form->addRow(QStringLiteral("撤销日期 *"), dateEdit);
    form->addRow(QStringLiteral("撤销数量 *"), quantitySpin);
    form->addRow(QStringLiteral("经办人"), handlerEdit);
    QTextEdit *serialEdit = nullptr;
    if (requireSerial) {
        serialEdit = new QTextEdit(&dialog);
        serialEdit->setPlaceholderText(QStringLiteral("每行一个需要撤销的SN"));
        serialEdit->setPlainText(originalSerials.split(QStringLiteral(", ")).join(QLatin1Char('\n')));
        serialEdit->setMaximumHeight(110);
        form->addRow(QStringLiteral("SN列表 *"), serialEdit);
    }
    form->addRow(QStringLiteral("撤销原因 *"), notesEdit);
    root->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确认撤销"));
    buttons->button(QDialogButtonBox::Ok)->setProperty("danger", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (notesEdit->toPlainText().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少撤销原因"), QStringLiteral("请填写撤销原因。"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("再次确认"),
        QStringLiteral("确定撤销 %1，数量 %2？")
            .arg(documentNumber, QString::number(quantitySpin->value(), 'f', 6))) != QMessageBox::Yes) {
        return;
    }

    ReversalRequest request;
    request.sourceItemId = itemId;
    request.documentDate = dateEdit->date();
    request.quantity = quantitySpin->value();
    request.handlerName = handlerEdit->text().trimmed();
    request.notes = notesEdit->toPlainText().trimmed();
    if (serialEdit) request.serialNumbers = parseSerialNumbers(serialEdit->toPlainText());

    InventoryService service(m_database, m_session.userId);
    PostedDocument posted;
    QString error;
    if (!service.reverseItem(request, &posted, &error)) {
        QMessageBox::warning(this, QStringLiteral("撤销失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("撤销完成"),
                             QStringLiteral("已生成反向单据：%1").arg(posted.documentNumber));
    refresh();
    emit stockChanged();
}
