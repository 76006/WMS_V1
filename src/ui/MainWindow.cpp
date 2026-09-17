#include "ui/MainWindow.h"

#include "ui/pages/DashboardPage.h"
#include "ui/pages/AttachmentPage.h"
#include "ui/pages/AuditLogPage.h"
#include "ui/pages/BatchTracePage.h"
#include "ui/pages/ExcelImportPage.h"
#include "ui/pages/FinishedGoodsInPage.h"
#include "ui/pages/InventoryPage.h"
#include "ui/pages/InventoryReportPage.h"
#include "ui/pages/InventoryCountPage.h"
#include "ui/pages/InspectionPage.h"
#include "ui/pages/LedgerPage.h"
#include "ui/pages/MaterialPage.h"
#include "ui/pages/PlaceholderPage.h"
#include "ui/pages/ProductionIssuePage.h"
#include "ui/pages/ProductionReturnPage.h"
#include "ui/pages/SerialTracePage.h"
#include "ui/pages/ShipmentQueryPage.h"
#include "ui/pages/StockInPage.h"
#include "ui/pages/StockOutPage.h"
#include "ui/pages/SystemSettingsPage.h"
#include "ui/pages/TransferPage.h"
#include "ui/pages/UserManagementPage.h"
#include "ui/pages/WarehousePage.h"
#include "ui/widgets/TableExcelExport.h"
#include "ui/dialogs/BusinessDocumentEditDialog.h"
#include "services/InventoryService.h"
#include "services/OfficeTemplateService.h"
#include "services/UserService.h"

#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSqlError>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextEdit>
#include <QVBoxLayout>

#include <utility>

MainWindow::MainWindow(QSqlDatabase database,
                       Session session,
                       const QString &databaseFilePath,
                       QWidget *parent)
    : QMainWindow(parent),
      m_database(std::move(database)),
      m_session(std::move(session)),
      m_databaseFilePath(databaseFilePath)
{
    setWindowTitle(QStringLiteral("冰美肌库存管理"));
    resize(1440, 880);
    setMinimumSize(1120, 700);
    buildUi();
    statusBar()->showMessage(QStringLiteral("数据库：%1").arg(QFileInfo(m_databaseFilePath).absoluteFilePath()));
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *sidebar = new QFrame(central);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(210);
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 0, 10);
    sideLayout->setSpacing(0);
    auto *brand = new QLabel(QStringLiteral("冰美肌 WMS"), sidebar);
    brand->setObjectName(QStringLiteral("brandTitle"));
    auto *subtitle = new QLabel(QStringLiteral("轻量库存管理"), sidebar);
    subtitle->setObjectName(QStringLiteral("brandSubtitle"));
    sideLayout->addWidget(brand);
    sideLayout->addWidget(subtitle);

    auto *scroll = new QScrollArea(sidebar);
    scroll->setObjectName(QStringLiteral("sidebarScroll"));
    scroll->viewport()->setObjectName(QStringLiteral("sidebarViewport"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *navWidget = new QWidget(scroll);
    navWidget->setObjectName(QStringLiteral("sidebarNav"));
    auto *navLayout = new QVBoxLayout(navWidget);
    navLayout->setContentsMargins(0, 0, 0, 0);
    navLayout->setSpacing(0);
    auto *buttonGroup = new QButtonGroup(this);
    buttonGroup->setExclusive(true);

    auto *content = new QWidget(central);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    auto *topBar = new QFrame(content);
    topBar->setObjectName(QStringLiteral("topBar"));
    topBar->setStyleSheet(QStringLiteral("QFrame#topBar { border-radius: 0; border-top: 0; border-left: 0; border-right: 0; }"));
    auto *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(20, 12, 20, 12);
    m_pageTitle = new QLabel(QStringLiteral("首页"), topBar);
    m_pageTitle->setObjectName(QStringLiteral("pageTitle"));
    auto *refreshButton = new QPushButton(QStringLiteral("刷新"), topBar);
    auto *exportButton = new QPushButton(QStringLiteral("导出Excel"), topBar);
    m_userLabel = new QLabel(QStringLiteral("%1（%2）")
                                 .arg(m_session.displayName, m_session.roleCode), topBar);
    m_userLabel->setObjectName(QStringLiteral("mutedText"));
    topLayout->addWidget(m_pageTitle);
    topLayout->addStretch();
    topLayout->addWidget(exportButton);
    topLayout->addSpacing(8);
    topLayout->addWidget(refreshButton);
    topLayout->addSpacing(10);
    topLayout->addWidget(m_userLabel);
    contentLayout->addWidget(topBar);

    m_stack = new QStackedWidget(content);
    contentLayout->addWidget(m_stack, 1);

    auto add = [&](const QString &title, QWidget *page, bool enabled = true) {
        const int index = m_stack->addWidget(page);
        TableExcelExport::install(page, title);
        auto *button = new QPushButton(title, navWidget);
        button->setProperty("nav", true);
        button->setCheckable(true);
        button->setEnabled(enabled);
        buttonGroup->addButton(button, index);
        navLayout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, index] { showPage(index); });
        if (index == 0) {
            button->setChecked(true);
        }
    };

    m_dashboardPage = new DashboardPage(m_database, m_stack);
    m_materialPage = new MaterialPage(m_database, m_session, m_stack);
    m_warehousePage = new WarehousePage(m_database, m_session, m_stack);
    m_excelImportPage = new ExcelImportPage(m_database, m_session, m_stack);
    m_stockInPage = new StockInPage(m_database, m_session, m_stack);
    m_inspectionPage = new InspectionPage(m_database, m_session, m_stack);
    m_stockOutPage = new StockOutPage(m_database, m_session, m_stack);
    m_shipmentQueryPage = new ShipmentQueryPage(m_database, m_session, m_stack);
    m_productionIssuePage = new ProductionIssuePage(m_database, m_session, m_stack);
    m_productionReturnPage = new ProductionReturnPage(m_database, m_session, m_stack);
    m_finishedGoodsInPage = new FinishedGoodsInPage(m_database, m_session, m_stack);
    m_inventoryPage = new InventoryPage(m_database, m_stack);
    m_inventoryReportPage = new InventoryReportPage(m_database, m_stack);
    m_ledgerPage = new LedgerPage(m_database, m_session, m_stack);
    m_transferPage = new TransferPage(m_database, m_session, m_stack);
    m_countPage = new InventoryCountPage(m_database, m_session, m_stack);
    m_batchTracePage = new BatchTracePage(m_database, m_stack);
    m_serialTracePage = new SerialTracePage(m_database, m_stack);
    m_attachmentPage = new AttachmentPage(m_database, m_session, m_stack);
    m_auditLogPage = new AuditLogPage(m_database, m_stack);
    m_userManagementPage = new UserManagementPage(m_database, m_session, m_stack);
    m_systemSettingsPage = new SystemSettingsPage(m_database, m_session,
                                                   m_databaseFilePath, m_stack);

    add(QStringLiteral("首页"), m_dashboardPage);
    add(QStringLiteral("物料维护"), m_materialPage);
    add(QStringLiteral("仓库/库位管理"), m_warehousePage);
    add(QStringLiteral("库存Excel导入"), m_excelImportPage, m_session.canManageWarehouse());
    add(QStringLiteral("材料送检"), m_inspectionPage, m_session.canManageWarehouse());
    add(QStringLiteral("入库管理"), m_stockInPage, m_session.canManageWarehouse());
    add(QStringLiteral("出库管理"), m_stockOutPage, m_session.canManageWarehouse());
    add(QStringLiteral("发货查询"), m_shipmentQueryPage, m_session.canViewInventory());
    add(QStringLiteral("领料管理"), m_productionIssuePage, m_session.canPostProduction());
    add(QStringLiteral("生产退料"), m_productionReturnPage, m_session.canPostProduction());
    add(QStringLiteral("成品入库"), m_finishedGoodsInPage, m_session.canPostProduction());
    add(QStringLiteral("库存查询"), m_inventoryPage, m_session.canViewInventory());
    add(QStringLiteral("出入库统计"), m_inventoryReportPage, m_session.canViewInventory());
    add(QStringLiteral("库存流水"), m_ledgerPage, m_session.canViewInventory());
    add(QStringLiteral("库存调拨"), m_transferPage, m_session.canManageWarehouse());
    add(QStringLiteral("库存盘点"), m_countPage, m_session.canManageWarehouse());
    add(QStringLiteral("批次查询"), m_batchTracePage, m_session.canViewInventory());
    add(QStringLiteral("SN查询"), m_serialTracePage, m_session.canViewInventory());
    add(QStringLiteral("附件管理"), m_attachmentPage);
    add(QStringLiteral("操作日志"), m_auditLogPage, m_session.canViewAudit());
    add(QStringLiteral("用户管理"), m_userManagementPage, m_session.canManageUsers());
    add(QStringLiteral("系统设置"), m_systemSettingsPage);

    navLayout->addStretch();
    scroll->setWidget(navWidget);
    sideLayout->addWidget(scroll, 1);
    root->addWidget(sidebar);
    root->addWidget(content, 1);
    setCentralWidget(central);

    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::refreshCurrentPage);
    connect(exportButton, &QPushButton::clicked, this, [this] {
        TableExcelExport::exportPage(m_stack->currentWidget(), m_pageTitle->text(), this);
    });
    connect(m_materialPage, &MaterialPage::dataChanged, this, &MainWindow::refreshInventoryViews);
    connect(m_warehousePage, &WarehousePage::dataChanged, m_stockInPage, &StockInPage::refreshReferenceData);
    connect(m_warehousePage, &WarehousePage::dataChanged, m_stockOutPage, &StockOutPage::refreshReferenceData);
    connect(m_warehousePage, &WarehousePage::dataChanged, m_excelImportPage, &ExcelImportPage::refreshReferenceData);
    connect(m_warehousePage, &WarehousePage::dataChanged, m_transferPage, &TransferPage::refreshReferenceData);
    connect(m_warehousePage, &WarehousePage::dataChanged, m_countPage, &InventoryCountPage::refreshReferenceData);
    connect(m_excelImportPage, &ExcelImportPage::stockChanged, this, &MainWindow::refreshInventoryViews);
    connect(m_stockInPage, &StockInPage::stockChanged, this, &MainWindow::refreshInventoryViews);
    connect(m_inspectionPage, &InspectionPage::inspectionChanged,
            m_stockInPage, &StockInPage::refreshInspectionNotices);
    connect(m_stockOutPage, &StockOutPage::stockChanged, this, &MainWindow::refreshInventoryViews);
    connect(m_transferPage, &TransferPage::stockChanged, this, &MainWindow::refreshInventoryViews);
    connect(m_countPage, &InventoryCountPage::stockChanged, this, &MainWindow::refreshInventoryViews);
    connect(m_productionIssuePage, &ProductionIssuePage::stockChanged,
            this, &MainWindow::refreshInventoryViews);
    connect(m_productionReturnPage, &ProductionReturnPage::stockChanged,
            this, &MainWindow::refreshInventoryViews);
    connect(m_finishedGoodsInPage, &FinishedGoodsInPage::stockChanged,
            this, &MainWindow::refreshInventoryViews);
    connect(m_ledgerPage, &LedgerPage::stockChanged, this, &MainWindow::refreshInventoryViews);
    connect(m_systemSettingsPage, &SystemSettingsPage::businessDataCleared,
            this, &MainWindow::refreshInventoryViews);
    connect(m_userManagementPage, &UserManagementPage::usersChanged,
            this, &MainWindow::refreshCurrentUserDisplayName);
}

void MainWindow::showPage(int index)
{
    m_stack->setCurrentIndex(index);
    if (auto *button = qobject_cast<QPushButton *>(sender())) {
        m_pageTitle->setText(button->text());
    }
    refreshCurrentPage();
}

void MainWindow::refreshCurrentPage()
{
    refreshCurrentUserDisplayName();
    QWidget *page = m_stack->currentWidget();
    if (page == m_dashboardPage) m_dashboardPage->refresh();
    else if (page == m_materialPage) m_materialPage->refresh();
    else if (page == m_warehousePage) m_warehousePage->refresh();
    else if (page == m_excelImportPage) m_excelImportPage->refreshReferenceData();
    else if (page == m_stockInPage) m_stockInPage->refreshReferenceData();
    else if (page == m_inspectionPage) m_inspectionPage->refreshReferenceData();
    else if (page == m_stockOutPage) m_stockOutPage->refreshReferenceData();
    else if (page == m_shipmentQueryPage) m_shipmentQueryPage->refresh();
    else if (page == m_productionIssuePage) m_productionIssuePage->refreshReferenceData();
    else if (page == m_productionReturnPage) m_productionReturnPage->refreshReferenceData();
    else if (page == m_finishedGoodsInPage) m_finishedGoodsInPage->refreshReferenceData();
    else if (page == m_inventoryPage) m_inventoryPage->refresh();
    else if (page == m_inventoryReportPage) m_inventoryReportPage->refresh();
    else if (page == m_ledgerPage) m_ledgerPage->refresh();
    else if (page == m_transferPage) m_transferPage->refreshReferenceData();
    else if (page == m_countPage) m_countPage->refreshReferenceData();
    else if (page == m_batchTracePage) m_batchTracePage->refresh();
    else if (page == m_serialTracePage) m_serialTracePage->refresh();
    else if (page == m_attachmentPage) m_attachmentPage->refresh();
    else if (page == m_auditLogPage) m_auditLogPage->refresh();
    else if (page == m_userManagementPage) m_userManagementPage->refresh();
}

void MainWindow::refreshInventoryViews()
{
    m_dashboardPage->refresh();
    m_materialPage->refresh();
    m_inventoryPage->refresh();
    m_inventoryReportPage->refresh();
    m_ledgerPage->refresh();
    m_stockInPage->refreshReferenceData();
    m_inspectionPage->refreshReferenceData();
    m_stockOutPage->refreshReferenceData();
    m_shipmentQueryPage->refresh();
    m_excelImportPage->refreshReferenceData();
    m_transferPage->refreshReferenceData();
    m_countPage->refreshReferenceData();
    m_batchTracePage->refresh();
    m_serialTracePage->refresh();
    m_attachmentPage->refresh();
    m_productionIssuePage->refreshReferenceData();
    m_productionReturnPage->refreshReferenceData();
    m_finishedGoodsInPage->refreshReferenceData();
}

void MainWindow::refreshCurrentUserDisplayName()
{
    const QString previousName = m_session.displayName.trimmed();
    const QString currentName = UserService::displayNameForUser(
        m_database, m_session.userId, previousName);
    if (currentName.isEmpty()) return;
    m_session.displayName = currentName;
    if (m_userLabel) {
        m_userLabel->setText(QStringLiteral("%1（%2）")
                                 .arg(currentName, m_session.roleCode));
    }
    const auto edits = findChildren<QLineEdit *>();
    for (QLineEdit *edit : edits) {
        if (!edit->property("currentUserDefault").toBool()) continue;
        const QString previousDefault = edit->property("lastCurrentUserDefault").toString();
        const QString text = edit->text().trimmed();
        if (text.isEmpty() || text == previousDefault || text == previousName) {
            edit->setText(currentName);
        }
        edit->setProperty("lastCurrentUserDefault", currentName);
    }
}

void MainWindow::editDocumentById(qlonglong documentId, QWidget *dialogParent)
{
    if (documentId <= 0) {
        QMessageBox::information(dialogParent ? dialogParent : this, QStringLiteral("无法修改"),
                                 QStringLiteral("没有取得当前单据的编号。"));
        return;
    }
    BusinessDocumentEditDialog dialog(m_database, m_session, documentId, dialogParent ? dialogParent : this);
    dialog.exec();
    if (dialog.saved()) refreshInventoryViews();
}

void MainWindow::reverseDocumentById(qlonglong documentId, QWidget *dialogParent)
{
    QWidget *parent = dialogParent ? dialogParent : this;
    if (documentId <= 0) {
        QMessageBox::information(parent, QStringLiteral("无法撤销"),
                                 QStringLiteral("没有取得当前单据的编号。"));
        return;
    }
    if (!m_session.canManageWarehouse()) {
        QMessageBox::warning(parent, QStringLiteral("没有权限"),
                             QStringLiteral("当前账号没有库存单据撤销权限。"));
        return;
    }
    QSqlQuery header(m_database);
    header.prepare(QStringLiteral(
        "SELECT document_no,document_type,status,created_by,handler_name "
        "FROM business_documents WHERE id=?"));
    header.addBindValue(documentId);
    if (!header.exec() || !header.next()) {
        QMessageBox::warning(parent, QStringLiteral("无法撤销"),
                             QStringLiteral("读取单据信息失败：%1")
                                 .arg(header.lastError().text()));
        return;
    }
    const QString documentNumber = header.value(0).toString();
    const QString documentType = header.value(1).toString();
    const QString status = header.value(2).toString().trimmed().toUpper();
    const qlonglong creatorId = header.value(3).toLongLong();
    const bool allowedOwner = m_session.isAdministrator() || creatorId == m_session.userId;
    if (!allowedOwner) {
        QMessageBox::warning(parent, QStringLiteral("无法撤销"),
                             QStringLiteral("只有管理员或原单创建人可以撤销这张单据。"));
        return;
    }
    if (status != QStringLiteral("POSTED")) {
        QMessageBox::warning(parent, QStringLiteral("无法撤销"),
                             QStringLiteral("只有状态为“已入账”的单据可以整单撤销；"
                                            "当前状态为 %1。")
                                 .arg(status));
        return;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("撤销整张单据"));
    dialog.setMinimumWidth(540);
    auto *root = new QVBoxLayout(&dialog);
    auto *warning = new QLabel(QStringLiteral(
        "撤销后将回退整张单据产生的库存和 SN 状态，并将系统生成的表单文件移入“已撤销”归档。"
        "此操作会保留单据和审计记录。"), &dialog);
    warning->setWordWrap(true);
    warning->setProperty("warning", true);
    root->addWidget(warning);
    auto *form = new QFormLayout;
    auto *documentLabel = new QLabel(
        QStringLiteral("%1（%2）").arg(documentNumber, documentType), &dialog);
    auto *handlerEdit = new QLineEdit(m_session.displayName, &dialog);
    handlerEdit->setProperty("currentUserDefault", true);
    auto *reasonEdit = new QTextEdit(&dialog);
    reasonEdit->setPlaceholderText(QStringLiteral("必须填写撤销原因，例如：录入错误、重复入库"));
    reasonEdit->setMinimumHeight(100);
    form->addRow(QStringLiteral("单据"), documentLabel);
    form->addRow(QStringLiteral("撤销人"), handlerEdit);
    form->addRow(QStringLiteral("撤销原因 *"), reasonEdit);
    root->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *confirmButton = buttons->addButton(QStringLiteral("确认撤销"),
                                             QDialogButtonBox::AcceptRole);
    confirmButton->setProperty("danger", true);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    root->addWidget(buttons);

    while (dialog.exec() == QDialog::Accepted) {
        const QString reason = reasonEdit->toPlainText().trimmed();
        if (reason.isEmpty()) {
            QMessageBox::information(&dialog, QStringLiteral("请填写原因"),
                                     QStringLiteral("撤销原因不能为空。"));
            continue;
        }
        if (QMessageBox::question(
                parent, QStringLiteral("最终确认"),
                QStringLiteral("确定撤销单据 %1 吗？\n库存与相关文件会立即同步更新。")
                    .arg(documentNumber),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }

        InventoryService service(m_database, m_session.userId);
        QString reversedNumber;
        QString error;
        if (!service.reversePostedDocument(documentId, handlerEdit->text(), reason,
                                           &reversedNumber, &error)) {
            QMessageBox::warning(parent, QStringLiteral("撤销失败"), error);
            return;
        }
        QStringList archivedPaths;
        QStringList fileWarnings;
        OfficeTemplateService::archiveReversedDocumentForms(
            m_database, documentId, &archivedPaths, &fileWarnings);
        refreshInventoryViews();

        QString message = QStringLiteral("单据 %1 已撤销，库存和 SN 状态已同步更新。")
                              .arg(reversedNumber);
        if (!archivedPaths.isEmpty()) {
            message += QStringLiteral("\n\n已撤销表单文件：\n%1")
                           .arg(archivedPaths.join(QLatin1Char('\n')));
        }
        if (!fileWarnings.isEmpty()) {
            message += QStringLiteral("\n\n文件归档提示：\n%1")
                           .arg(fileWarnings.join(QLatin1Char('\n')));
        }
        QMessageBox::information(parent, QStringLiteral("撤销完成"), message);
        return;
    }
}
