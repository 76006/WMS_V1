#pragma once

#include "core/Session.h"

#include <QMainWindow>
#include <QSqlDatabase>

class DashboardPage;
class AttachmentPage;
class BatchTracePage;
class ExcelImportPage;
class FinishedGoodsInPage;
class InventoryPage;
class InventoryCountPage;
class LedgerPage;
class MaterialPage;
class ProductionIssuePage;
class ProductionReturnPage;
class SerialTracePage;
class StockInPage;
class StockOutPage;
class SystemSettingsPage;
class TransferPage;
class UserManagementPage;
class WarehousePage;
class QLabel;
class QStackedWidget;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QSqlDatabase database,
               Session session,
               const QString &databaseFilePath,
               QWidget *parent = nullptr);

private slots:
    void showPage(int index);
    void refreshCurrentPage();
    void refreshInventoryViews();

private:
    void buildUi();

    QSqlDatabase m_database;
    Session m_session;
    QString m_databaseFilePath;
    QLabel *m_pageTitle = nullptr;
    QStackedWidget *m_stack = nullptr;
    DashboardPage *m_dashboardPage = nullptr;
    MaterialPage *m_materialPage = nullptr;
    WarehousePage *m_warehousePage = nullptr;
    ExcelImportPage *m_excelImportPage = nullptr;
    StockInPage *m_stockInPage = nullptr;
    StockOutPage *m_stockOutPage = nullptr;
    ProductionIssuePage *m_productionIssuePage = nullptr;
    ProductionReturnPage *m_productionReturnPage = nullptr;
    FinishedGoodsInPage *m_finishedGoodsInPage = nullptr;
    InventoryPage *m_inventoryPage = nullptr;
    LedgerPage *m_ledgerPage = nullptr;
    BatchTracePage *m_batchTracePage = nullptr;
    SerialTracePage *m_serialTracePage = nullptr;
    AttachmentPage *m_attachmentPage = nullptr;
    UserManagementPage *m_userManagementPage = nullptr;
    SystemSettingsPage *m_systemSettingsPage = nullptr;
    TransferPage *m_transferPage = nullptr;
    InventoryCountPage *m_countPage = nullptr;
};
