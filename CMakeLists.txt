cmake_minimum_required(VERSION 3.21)

project(IceBeautyWms VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Sql)

qt_standard_project_setup()

set(WMS_SOURCES
    src/main.cpp
    src/core/DatabaseConfig.cpp
    src/core/PasswordHasher.cpp
    src/database/DatabaseManager.cpp
    src/database/SchemaMigrator.cpp
    src/import/LegacyInventoryImporter.cpp
    src/services/AttachmentService.cpp
    src/services/InventoryService.cpp
    src/services/InventoryServiceDocuments.cpp
    src/services/InventoryServiceProduction.cpp
    src/services/UserService.cpp
    src/ui/LoginDialog.cpp
    src/ui/MainWindow.cpp
    src/ui/dialogs/MaterialDialog.cpp
    src/ui/pages/DashboardPage.cpp
    src/ui/pages/AttachmentPage.cpp
    src/ui/pages/BatchTracePage.cpp
    src/ui/pages/ExcelImportPage.cpp
    src/ui/pages/FinishedGoodsInPage.cpp
    src/ui/pages/InventoryPage.cpp
    src/ui/pages/InventoryCountPage.cpp
    src/ui/pages/LedgerPage.cpp
    src/ui/pages/MaterialPage.cpp
    src/ui/pages/PlaceholderPage.cpp
    src/ui/pages/ProductionIssuePage.cpp
    src/ui/pages/ProductionReturnPage.cpp
    src/ui/pages/SerialTracePage.cpp
    src/ui/pages/StockInPage.cpp
    src/ui/pages/StockOutPage.cpp
    src/ui/pages/SystemSettingsPage.cpp
    src/ui/pages/TransferPage.cpp
    src/ui/pages/UserManagementPage.cpp
    src/ui/pages/WarehousePage.cpp
    src/ui/widgets/StockLineTable.cpp
    src/core/DatabaseConfig.h
    src/import/LegacyInventoryImporter.h
    src/core/PasswordHasher.h
    src/core/Session.h
    src/database/DatabaseManager.h
    src/database/SchemaMigrator.h
    src/services/AttachmentService.h
    src/services/InventoryService.h
    src/services/UserService.h
    src/ui/LoginDialog.h
    src/ui/MainWindow.h
    src/ui/dialogs/MaterialDialog.h
    src/ui/pages/DashboardPage.h
    src/ui/pages/AttachmentPage.h
    src/ui/pages/BatchTracePage.h
    src/ui/pages/ExcelImportPage.h
    src/ui/pages/FinishedGoodsInPage.h
    src/ui/pages/InventoryPage.h
    src/ui/pages/InventoryCountPage.h
    src/ui/pages/LedgerPage.h
    src/ui/pages/MaterialPage.h
    src/ui/pages/PlaceholderPage.h
    src/ui/pages/ProductionIssuePage.h
    src/ui/pages/ProductionReturnPage.h
    src/ui/pages/SerialTracePage.h
    src/ui/pages/StockInPage.h
    src/ui/pages/StockOutPage.h
    src/ui/pages/SystemSettingsPage.h
    src/ui/pages/TransferPage.h
    src/ui/pages/UserManagementPage.h
    src/ui/pages/WarehousePage.h
    src/ui/widgets/StockLineTable.h
)

qt_add_executable(IceBeautyWms WIN32 ${WMS_SOURCES})
qt_add_resources(IceBeautyWms "wms_resources"
    PREFIX "/"
    FILES
        database/schema.sql
        database/migrations/002_production_workflow.sql
        resources/styles.qss
)

target_include_directories(IceBeautyWms PRIVATE src)
target_link_libraries(IceBeautyWms PRIVATE Qt6::Core Qt6::Widgets Qt6::Sql)

if(MSVC)
    target_compile_options(IceBeautyWms PRIVATE /utf-8 /W4)
else()
    target_compile_options(IceBeautyWms PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    qt_add_executable(WmsCoreTests
        tests/CoreTests.cpp
        src/core/PasswordHasher.cpp
        src/core/PasswordHasher.h
    )
    target_include_directories(WmsCoreTests PRIVATE src)
    target_link_libraries(WmsCoreTests PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME WmsCoreTests COMMAND WmsCoreTests)

    set(WMS_DATABASE_TEST_SOURCES
        src/core/PasswordHasher.cpp
        src/core/PasswordHasher.h
        src/database/DatabaseManager.cpp
        src/database/DatabaseManager.h
        src/database/SchemaMigrator.cpp
        src/database/SchemaMigrator.h
        src/services/InventoryService.cpp
        src/services/InventoryServiceDocuments.cpp
        src/services/InventoryServiceProduction.cpp
        src/services/InventoryService.h
    )

    qt_add_executable(WmsDatabaseTests
        tests/DatabaseTests.cpp
        ${WMS_DATABASE_TEST_SOURCES}
    )
    qt_add_resources(WmsDatabaseTests "wms_test_resources"
        PREFIX "/"
        FILES
            database/schema.sql
            database/migrations/002_production_workflow.sql
    )
    target_include_directories(WmsDatabaseTests PRIVATE src)
    target_link_libraries(WmsDatabaseTests PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    add_test(NAME WmsDatabaseTests COMMAND WmsDatabaseTests)

    qt_add_executable(WmsProductionWorkflowTests
        tests/ProductionWorkflowTests.cpp
        ${WMS_DATABASE_TEST_SOURCES}
    )
    qt_add_resources(WmsProductionWorkflowTests "wms_production_test_resources"
        PREFIX "/"
        FILES
            database/schema.sql
            database/migrations/002_production_workflow.sql
    )
    target_include_directories(WmsProductionWorkflowTests PRIVATE src)
    target_link_libraries(WmsProductionWorkflowTests PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    add_test(NAME WmsProductionWorkflowTests COMMAND WmsProductionWorkflowTests)

    qt_add_executable(WmsWorkflowExpansionTests
        tests/WorkflowExpansionTests.cpp
        src/import/LegacyInventoryImporter.cpp
        src/import/LegacyInventoryImporter.h
        ${WMS_DATABASE_TEST_SOURCES}
    )
    qt_add_resources(WmsWorkflowExpansionTests "wms_expansion_test_resources"
        PREFIX "/"
        FILES
            database/schema.sql
            database/migrations/002_production_workflow.sql
    )
    target_include_directories(WmsWorkflowExpansionTests PRIVATE src)
    target_link_libraries(WmsWorkflowExpansionTests PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    add_test(NAME WmsWorkflowExpansionTests COMMAND WmsWorkflowExpansionTests)

    qt_add_executable(WmsTraceabilityAttachmentTests
        tests/TraceabilityAttachmentTests.cpp
        src/services/AttachmentService.cpp
        src/services/AttachmentService.h
        src/core/PasswordHasher.cpp
        src/core/PasswordHasher.h
        src/database/DatabaseManager.cpp
        src/database/DatabaseManager.h
        src/database/SchemaMigrator.cpp
        src/database/SchemaMigrator.h
    )
    qt_add_resources(WmsTraceabilityAttachmentTests "wms_attachment_test_resources"
        PREFIX "/"
        FILES
            database/schema.sql
            database/migrations/002_production_workflow.sql
    )
    target_include_directories(WmsTraceabilityAttachmentTests PRIVATE src)
    target_link_libraries(WmsTraceabilityAttachmentTests PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    add_test(NAME WmsTraceabilityAttachmentTests COMMAND WmsTraceabilityAttachmentTests)

    qt_add_executable(WmsUserServiceTests
        tests/UserServiceTests.cpp
        src/services/UserService.cpp
        src/services/UserService.h
        src/core/PasswordHasher.cpp
        src/core/PasswordHasher.h
        src/database/DatabaseManager.cpp
        src/database/DatabaseManager.h
        src/database/SchemaMigrator.cpp
        src/database/SchemaMigrator.h
    )
    qt_add_resources(WmsUserServiceTests "wms_user_test_resources"
        PREFIX "/"
        FILES
            database/schema.sql
            database/migrations/002_production_workflow.sql
    )
    target_include_directories(WmsUserServiceTests PRIVATE src)
    target_link_libraries(WmsUserServiceTests PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    add_test(NAME WmsUserServiceTests COMMAND WmsUserServiceTests)

    add_test(NAME WmsAppSmoke
        COMMAND IceBeautyWms --smoke-test --database ${CMAKE_CURRENT_BINARY_DIR}/wms-smoke.db)
    add_test(NAME WmsUiSmoke
        COMMAND IceBeautyWms -platform offscreen --ui-smoke-test
                --database ${CMAKE_CURRENT_BINARY_DIR}/wms-ui-smoke.db)
endif()
