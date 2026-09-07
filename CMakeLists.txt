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
    src/services/InventoryService.cpp
    src/ui/LoginDialog.cpp
    src/ui/MainWindow.cpp
    src/ui/dialogs/MaterialDialog.cpp
    src/ui/pages/DashboardPage.cpp
    src/ui/pages/InventoryPage.cpp
    src/ui/pages/LedgerPage.cpp
    src/ui/pages/MaterialPage.cpp
    src/ui/pages/PlaceholderPage.cpp
    src/ui/pages/StockInPage.cpp
    src/ui/pages/StockOutPage.cpp
    src/ui/pages/WarehousePage.cpp
    src/core/DatabaseConfig.h
    src/core/PasswordHasher.h
    src/core/Session.h
    src/database/DatabaseManager.h
    src/database/SchemaMigrator.h
    src/services/InventoryService.h
    src/ui/LoginDialog.h
    src/ui/MainWindow.h
    src/ui/dialogs/MaterialDialog.h
    src/ui/pages/DashboardPage.h
    src/ui/pages/InventoryPage.h
    src/ui/pages/LedgerPage.h
    src/ui/pages/MaterialPage.h
    src/ui/pages/PlaceholderPage.h
    src/ui/pages/StockInPage.h
    src/ui/pages/StockOutPage.h
    src/ui/pages/WarehousePage.h
)

qt_add_executable(IceBeautyWms WIN32 ${WMS_SOURCES})
qt_add_resources(IceBeautyWms "wms_resources"
    PREFIX "/"
    FILES
        database/schema.sql
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

    qt_add_executable(WmsDatabaseTests
        tests/DatabaseTests.cpp
        src/core/PasswordHasher.cpp
        src/core/PasswordHasher.h
        src/database/DatabaseManager.cpp
        src/database/DatabaseManager.h
        src/database/SchemaMigrator.cpp
        src/database/SchemaMigrator.h
        src/services/InventoryService.cpp
        src/services/InventoryService.h
    )
    qt_add_resources(WmsDatabaseTests "wms_test_resources"
        PREFIX "/"
        FILES database/schema.sql
    )
    target_include_directories(WmsDatabaseTests PRIVATE src)
    target_link_libraries(WmsDatabaseTests PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    add_test(NAME WmsDatabaseTests COMMAND WmsDatabaseTests)
    add_test(NAME WmsAppSmoke
        COMMAND IceBeautyWms --smoke-test --database ${CMAKE_CURRENT_BINARY_DIR}/wms-smoke.db)
endif()
