#include "ui/pages/TransferPage.h"

#include "services/InventoryService.h"
#include "ui/widgets/StockLineTable.h"

#include <QComboBox>
#include <QDateEdit>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlQuery>
#include <QTextEdit>
#include <QVBoxLayout>

#include <utility>

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
    root->addStretch();
    connect(m_targetWarehouse, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &TransferPage::loadTargetLocations);
    connect(m_submitButton, &QPushButton::clicked, this, &TransferPage::submit);
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
