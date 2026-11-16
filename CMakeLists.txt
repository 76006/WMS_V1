cmake_minimum_required(VERSION 3.21)

project(IceBeautyWms VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Sql QuickWidgets WebView PrintSupport Network)

qt_standard_project_setup()

set(WMS_SOURCES
    src/main.cpp
    src/network/NetworkWindow.cpp
    src/network/NetworkWindow.h
    src/core/DatabaseConfig.cpp
    src/core/PasswordHasher.cpp
    src/core/SystemProfile.cpp
    src/database/DatabaseManager.cpp
    src/database/SchemaMigrator.cpp
    src/import/LegacyInventoryImporter.cpp
    src/import/XlsxExporter.cpp
    src/services/AttachmentService.cpp
    src/services/InventoryService.cpp
    src/services/InventoryServiceDocuments.cpp
    src/services/InventoryServiceProduction.cpp
    src/services/InventoryServiceRevision.cpp
    src/services/InspectionService.cpp
    src/services/MaterialCodeService.cpp
    src/services/OfficeTemplateService.cpp
    src/services/ShipmentReceiptService.cpp
    src/services/InventoryReportService.cpp
    src/services/UserService.cpp
    src/ui/LoginDialog.cpp
    src/ui/MainWindow.cpp
    src/ui/dialogs/MaterialDialog.cpp
    src/ui/dialogs/InspectionDialog.cpp
    src/ui/dialogs/DocumentTemplateDialog.cpp
    src/ui/dialogs/BusinessDocumentEditDialog.cpp
    src/ui/pages/DashboardPage.cpp
    src/ui/pages/AttachmentPage.cpp
    src/ui/pages/AuditLogPage.cpp
    src/ui/pages/BatchTracePage.cpp
    src/ui/pages/ExcelImportPage.cpp
    src/ui/pages/FinishedGoodsInPage.cpp
    src/ui/pages/FormManagementPage.cpp
    src/ui/pages/InventoryPage.cpp
    src/ui/pages/InventoryReportPage.cpp
    src/ui/pages/InventoryCountPage.cpp
    src/ui/pages/InspectionPage.cpp
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
    src/core/SystemProfile.h
    src/database/DatabaseManager.h
    src/database/SchemaMigrator.h
    src/services/AttachmentService.h
    src/services/InventoryService.h
    src/services/InspectionService.h
    src/services/MaterialCodeService.h
    src/services/OfficeTemplateService.h
    src/services/ShipmentReceiptService.h
    src/services/InventoryReportService.h
    src/services/UserService.h
    src/ui/LoginDialog.h
    src/ui/MainWindow.h
    src/ui/dialogs/MaterialDialog.h
    src/ui/dialogs/InspectionDialog.h
    src/ui/dialogs/DocumentTemplateDialog.h
    src/ui/dialogs/BusinessDocumentEditDialog.h
    src/ui/pages/DashboardPage.h
    src/ui/pages/AttachmentPage.h
    src/ui/pages/AuditLogPage.h
    src/ui/pages/BatchTracePage.h
    src/ui/pages/ExcelImportPage.h
    src/ui/pages/FinishedGoodsInPage.h
    src/ui/pages/FormManagementPage.h
    src/ui/pages/InventoryPage.h
    src/ui/pages/InventoryReportPage.h
    src/ui/pages/InventoryCountPage.h
    src/ui/pages/InspectionPage.h
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

set(WMS_RESOURCE_FILES
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
        database/migrations/011_document_forms.sql
        database/migrations/012_sales_delivery_date.sql
        database/migrations/013_inspection_notices.sql
        database/migrations/014_production_return_number.sql
        database/migrations/015_shipment_receipts.sql
        database/migrations/016_issue_outbound_types.sql
        database/migrations/017_issue_line_usage.sql
        database/migrations/018_inspection_quantities.sql
        database/migrations/019_inspection_result_records.sql
        database/migrations/020_system_identity.sql
        database/migrations/021_two_digit_number_rules.sql
        resources/PdfPreview.qml
        resources/fill_office_template.ps1
        resources/styles.qss
)

function(add_wms_application target profile_definition)
    qt_add_executable(${target} WIN32 ${WMS_SOURCES})
    qt_add_resources(${target} "${target}_resources"
        PREFIX "/"
        FILES ${WMS_RESOURCE_FILES}
    )
    target_compile_definitions(${target} PRIVATE ${profile_definition})
    target_include_directories(${target} PRIVATE src)
    target_link_libraries(${target} PRIVATE
        Qt6::Core Qt6::Widgets Qt6::Sql Qt6::QuickWidgets Qt6::WebView Qt6::PrintSupport Qt6::Network)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_wms_application(IceBeautyWms WMS_PROFILE_ICE_BEAUTY=1)
add_wms_application(MeiJingLingWms WMS_PROFILE_MEI_JING_LING=1)

install(TARGETS IceBeautyWms RUNTIME DESTINATION . COMPONENT IceBeauty)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/仓库系统表单模板" DESTINATION . COMPONENT IceBeauty)
if(EXISTS "${CMAKE_SOURCE_DIR}/美鲸灵仓库系统表单模板")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/美鲸灵仓库系统表单模板" DESTINATION . COMPONENT MeiJingLing)
endif()
install(TARGETS MeiJingLingWms RUNTIME DESTINATION . COMPONENT MeiJingLing)

set(BUILD_TESTING OFF CACHE BOOL "Build project tests" FORCE)

option(WMS_BUILD_SERVER "Build the PostgreSQL LAN query pilot" ON)
if(WMS_BUILD_SERVER)
    find_package(Qt6 6.5 REQUIRED COMPONENTS HttpServer)
    qt_add_executable(WmsServer
        src/server/main.cpp src/server/LanDatabase.cpp src/server/LanDatabase.h
        src/server/LanServer.cpp src/server/LanServer.h src/core/PasswordHasher.cpp)
    target_include_directories(WmsServer PRIVATE src)
    target_link_libraries(WmsServer PRIVATE Qt6::Core Qt6::Sql Qt6::Network Qt6::HttpServer)
    if(MSVC)
        target_compile_options(WmsServer PRIVATE /utf-8 /W4)
    else()
        target_compile_options(WmsServer PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
