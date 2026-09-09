cmake_minimum_required(VERSION 3.21)

project(IceBeautyWms VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Sql QuickWidgets WebView PrintSupport)

qt_standard_project_setup()

set(WMS_SOURCES
    src/main.cpp
    src/core/DatabaseConfig.cpp
    src/core/PasswordHasher.cpp
    src/database/DatabaseManager.cpp
    src/database/SchemaMigrator.cpp
    src/import/LegacyInventoryImporter.cpp
    src/import/XlsxExporter.cpp
    src/services/AttachmentService.cpp
    src/services/InventoryService.cpp
    src/services/InventoryServiceDocuments.cpp
    src/services/InventoryServiceProduction.cpp
    src/services/MaterialCodeService.cpp
    src/services/InventoryReportService.cpp
    src/services/UserService.cpp
    src/ui/LoginDialog.cpp
    src/ui/MainWindow.cpp
    src/ui/dialogs/MaterialDialog.cpp
    src/ui/dialogs/InspectionDialog.cpp
    src/ui/pages/DashboardPage.cpp
    src/ui/pages/AttachmentPage.cpp
    src/ui/pages/AuditLogPage.cpp
    src/ui/pages/BatchTracePage.cpp
    src/ui/pages/ExcelImportPage.cpp
    src/ui/pages/FinishedGoodsInPage.cpp
    src/ui/pages/InventoryPage.cpp
    src/ui/pages/InventoryReportPage.cpp
    src/ui/pages/InventoryCountPage.cpp
    src/ui/pages/LedgerPage.cpp
    src/ui/pages/MaterialPage.cpp
    src/ui/pages/PlaceholderPage.cpp
    src/ui/pages/ProductionIssuePage.cpp
    src/ui/pages/ProductionReturnPage.cpp
    src/ui/pages/SerialTracePage.cpp
    src/ui/pages/ShipmentQueryPage.cpp
    src/ui/pages/StockInPage.cpp
    src/ui/pages/StockOutPage.cpp
    src/ui/pages/SystemSettingsPage.cpp
    src/ui/pages/TransferPage.cpp
    src/ui/pages/UserManagementPage.cpp
    src/ui/pages/WarehousePage.cpp
    src/ui/widgets/StockLineTable.cpp
    src/ui/widgets/ComboBoxSearch.cpp
    src/ui/widgets/TableExcelExport.cpp
    src/core/DatabaseConfig.h
    src/import/LegacyInventoryImporter.h
    src/import/XlsxExporter.h
    src/core/PasswordHasher.h
    src/core/Session.h
    src/database/DatabaseManager.h
    src/database/SchemaMigrator.h
    src/services/AttachmentService.h
    src/services/InventoryService.h
    src/services/MaterialCodeService.h
    src/services/InventoryReportService.h
    src/services/UserService.h
    src/ui/LoginDialog.h
    src/ui/MainWindow.h
    src/ui/dialogs/MaterialDialog.h
    src/ui/dialogs/InspectionDialog.h
    src/ui/pages/DashboardPage.h
    src/ui/pages/AttachmentPage.h
    src/ui/pages/AuditLogPage.h
    src/ui/pages/BatchTracePage.h
    src/ui/pages/ExcelImportPage.h
    src/ui/pages/FinishedGoodsInPage.h
    src/ui/pages/InventoryPage.h
    src/ui/pages/InventoryReportPage.h
    src/ui/pages/InventoryCountPage.h
    src/ui/pages/LedgerPage.h
    src/ui/pages/MaterialPage.h
    src/ui/pages/PlaceholderPage.h
    src/ui/pages/ProductionIssuePage.h
    src/ui/pages/ProductionReturnPage.h
    src/ui/pages/SerialTracePage.h
    src/ui/pages/ShipmentQueryPage.h
    src/ui/pages/StockInPage.h
    src/ui/pages/StockOutPage.h
    src/ui/pages/SystemSettingsPage.h
    src/ui/pages/TransferPage.h
    src/ui/pages/UserManagementPage.h
    src/ui/pages/WarehousePage.h
    src/ui/widgets/StockLineTable.h
    src/ui/widgets/ComboBoxSearch.h
    src/ui/widgets/TableExcelExport.h
)

qt_add_executable(IceBeautyWms WIN32 ${WMS_SOURCES})
qt_add_resources(IceBeautyWms "wms_resources"
    PREFIX "/"
    FILES
        database/schema.sql
        database/migrations/002_production_workflow.sql
        database/migrations/003_final_features.sql
        database/migrations/004_material_numbering.sql
        database/migrations/005_inventory_reporting.sql
        database/migrations/006_material_operations.sql
        database/migrations/007_production_issue_number.sql
        database/migrations/008_sales_outbound_details.sql
        database/migrations/009_inbound_inspections.sql
        database/migrations/010_material_bom.sql
        resources/PdfPreview.qml
        resources/styles.qss
)

target_include_directories(IceBeautyWms PRIVATE src)
target_link_libraries(IceBeautyWms PRIVATE Qt6::Core Qt6::Widgets Qt6::Sql Qt6::QuickWidgets Qt6::WebView Qt6::PrintSupport)

install(TARGETS IceBeautyWms RUNTIME DESTINATION .)

if(MSVC)
    target_compile_options(IceBeautyWms PRIVATE /utf-8 /W4)
else()
    target_compile_options(IceBeautyWms PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(BUILD_TESTING OFF CACHE BOOL "Build project tests" FORCE)
