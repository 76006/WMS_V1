#include "ui/MainWindow.h"

#include "ui/pages/DashboardPage.h"
#include "ui/pages/FinishedGoodsInPage.h"
#include "ui/pages/InventoryPage.h"
#include "ui/pages/LedgerPage.h"
#include "ui/pages/MaterialPage.h"
#include "ui/pages/PlaceholderPage.h"
#include "ui/pages/ProductionIssuePage.h"
#include "ui/pages/ProductionReturnPage.h"
#include "ui/pages/StockInPage.h"
#include "ui/pages/StockOutPage.h"
#include "ui/pages/WarehousePage.h"

#include <QButtonGroup>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStatusBar>
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
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *navWidget = new QWidget(scroll);
    navWidget->setStyleSheet(QStringLiteral("background: transparent;"));
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
    auto *userLabel = new QLabel(QStringLiteral("%1（%2）")
                                     .arg(m_session.displayName, m_session.roleCode), topBar);
    userLabel->setObjectName(QStringLiteral("mutedText"));
    topLayout->addWidget(m_pageTitle);
    topLayout->addStretch();
    topLayout->addWidget(refreshButton);
    topLayout->addSpacing(10);
    topLayout->addWidget(userLabel);
    contentLayout->addWidget(topBar);

    m_stack = new QStackedWidget(content);
    contentLayout->addWidget(m_stack, 1);

    auto add = [&](const QString &title, QWidget *page, bool enabled = true) {
        const int index = m_stack->addWidget(page);
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
    m_stockInPage = new StockInPage(m_database, m_session, m_stack);
    m_stockOutPage = new StockOutPage(m_database, m_session, m_stack);
    m_productionIssuePage = new ProductionIssuePage(m_database, m_session, m_stack);
    m_productionReturnPage = new ProductionReturnPage(m_database, m_session, m_stack);
    m_finishedGoodsInPage = new FinishedGoodsInPage(m_database, m_session, m_stack);
    m_inventoryPage = new InventoryPage(m_database, m_stack);
    m_ledgerPage = new LedgerPage(m_database, m_session, m_stack);

    add(QStringLiteral("首页"), m_dashboardPage);
    add(QStringLiteral("物料管理"), m_materialPage);
    add(QStringLiteral("仓库/库位管理"), m_warehousePage);
    add(QStringLiteral("入库管理"), m_stockInPage, m_session.canManageWarehouse());
    add(QStringLiteral("出库管理"), m_stockOutPage, m_session.canManageWarehouse());
    add(QStringLiteral("生产领料"), m_productionIssuePage);
    add(QStringLiteral("生产退料"), m_productionReturnPage);
    add(QStringLiteral("成品入库"), m_finishedGoodsInPage);
    add(QStringLiteral("库存查询"), m_inventoryPage);
    add(QStringLiteral("库存流水"), m_ledgerPage);
    add(QStringLiteral("库存调拨"), new PlaceholderPage(QStringLiteral("库存调拨"),
        QStringLiteral("页面已预留。仓库/库位间原子调拨已在库存服务中实现。"), m_stack));
    add(QStringLiteral("库存盘点"), new PlaceholderPage(QStringLiteral("库存盘点"),
        QStringLiteral("页面已预留，数据库已包含盘点任务和盘点明细结构。"), m_stack));
    add(QStringLiteral("批次查询"), new PlaceholderPage(QStringLiteral("批次查询"),
        QStringLiteral("页面已预留，批次库存已经按物料、仓库和库位独立记录。"), m_stack));
    add(QStringLiteral("SN查询"), new PlaceholderPage(QStringLiteral("SN查询"),
        QStringLiteral("页面已预留，SN状态、位置、入库和出库单据已经结构化保存。"), m_stack));
    add(QStringLiteral("附件管理"), new PlaceholderPage(QStringLiteral("附件管理"),
        QStringLiteral("页面已预留，附件将以BLOB形式存入当前SQLite数据库文件。"), m_stack));
    add(QStringLiteral("用户管理"), new PlaceholderPage(QStringLiteral("用户管理"),
        QStringLiteral("页面已预留，仅管理员可以维护账号和角色。"), m_stack), m_session.isAdministrator());
    add(QStringLiteral("系统设置"), new PlaceholderPage(QStringLiteral("系统设置"),
        QStringLiteral("当前数据库文件：%1").arg(QFileInfo(m_databaseFilePath).absoluteFilePath()), m_stack),
        m_session.isAdministrator());

    navLayout->addStretch();
    scroll->setWidget(navWidget);
    sideLayout->addWidget(scroll, 1);
    root->addWidget(sidebar);
    root->addWidget(content, 1);
    setCentralWidget(central);

    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::refreshCurrentPage);
    connect(m_materialPage, &MaterialPage::dataChanged, this, &MainWindow::refreshInventoryViews);
    connect(m_warehousePage, &WarehousePage::dataChanged, m_stockInPage, &StockInPage::refreshReferenceData);
    connect(m_warehousePage, &WarehousePage::dataChanged, m_stockOutPage, &StockOutPage::refreshReferenceData);
    connect(m_stockInPage, &StockInPage::stockChanged, this, &MainWindow::refreshInventoryViews);
    connect(m_stockOutPage, &StockOutPage::stockChanged, this, &MainWindow::refreshInventoryViews);
    connect(m_productionIssuePage, &ProductionIssuePage::stockChanged,
            this, &MainWindow::refreshInventoryViews);
    connect(m_productionReturnPage, &ProductionReturnPage::stockChanged,
            this, &MainWindow::refreshInventoryViews);
    connect(m_finishedGoodsInPage, &FinishedGoodsInPage::stockChanged,
            this, &MainWindow::refreshInventoryViews);
    connect(m_ledgerPage, &LedgerPage::stockChanged, this, &MainWindow::refreshInventoryViews);
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
    QWidget *page = m_stack->currentWidget();
    if (page == m_dashboardPage) m_dashboardPage->refresh();
    else if (page == m_materialPage) m_materialPage->refresh();
    else if (page == m_warehousePage) m_warehousePage->refresh();
    else if (page == m_stockInPage) m_stockInPage->refreshReferenceData();
    else if (page == m_stockOutPage) m_stockOutPage->refreshReferenceData();
    else if (page == m_productionIssuePage) m_productionIssuePage->refreshReferenceData();
    else if (page == m_productionReturnPage) m_productionReturnPage->refreshReferenceData();
    else if (page == m_finishedGoodsInPage) m_finishedGoodsInPage->refreshReferenceData();
    else if (page == m_inventoryPage) m_inventoryPage->refresh();
    else if (page == m_ledgerPage) m_ledgerPage->refresh();
}

void MainWindow::refreshInventoryViews()
{
    m_dashboardPage->refresh();
    m_materialPage->refresh();
    m_inventoryPage->refresh();
    m_ledgerPage->refresh();
    m_stockInPage->refreshReferenceData();
    m_stockOutPage->refreshReferenceData();
    m_productionIssuePage->refreshReferenceData();
    m_productionReturnPage->refreshReferenceData();
    m_finishedGoodsInPage->refreshReferenceData();
}
