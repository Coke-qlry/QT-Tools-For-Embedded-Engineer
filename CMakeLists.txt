cmake_minimum_required(VERSION 3.16)

project(BLE_SAR VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 模块：Quick(界面) + Bluetooth(BLE 扫描/广播/连接) + Sql(SQLite 配置仓库)
find_package(Qt6 REQUIRED COMPONENTS Quick Bluetooth Sql)

qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(appBLE_SAR
    main.cpp
    # ---- BLE 功能模块（模块化） ----
    ble/bletypes.h
    ble/blescanner.h
    ble/blescanner.cpp
    ble/bleadvertiser.h
    ble/bleadvertiser.cpp
    ble/bleconnection.h
    ble/bleconnection.cpp
    ble/blemanager.h
    ble/blemanager.cpp
    ble/blepermissions.h
    ble/blepermissions.cpp
    ble/bleterminal.h
    ble/bleterminal.cpp
    # ---- SQLite 配置仓库模块（通用可复用） ----
    sqlite/SQLite.h
    sqlite/SQLite.cpp
)

qt_add_qml_module(appBLE_SAR
    URI BLE_SAR
    VERSION 1.0
    QML_FILES
        Main.qml
        ScanPage.qml
        AdvertisePage.qml
        TerminalPage.qml
        CommandPage.qml
        AboutPage.qml
    RESOURCES
        icons/tubiao.png
)

set_target_properties(appBLE_SAR PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
    # Android: 自定义 AndroidManifest.xml，声明 BLE 相关权限
    ANDROID_MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/android/AndroidManifest.xml
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
)

target_link_libraries(appBLE_SAR
    PRIVATE Qt6::Quick
    PRIVATE Qt6::Bluetooth
    PRIVATE Qt6::Sql
)

# SQLite 驱动(qsqlite)是运行时插件，Android 上必须让它随 APK 一起发布，
# 否则启动即报 UnsatisfiedLinkError: libplugins_sqldrivers_qsqlite... not found。
# Qt 6.10 起已移除 qt_add_plugins 命令，androiddeployqt 部署脚本改为从应用
# 目标的 QT_PLUGIN_TARGETS 属性收集插件（见 qt6_android_generate_deployment_settings），
# 因此这里手动把 SQLite 驱动插件登记进该属性即可被收集进 APK。
if(TARGET Qt6::QSQLiteDriverPlugin)
    set_property(TARGET appBLE_SAR APPEND PROPERTY
        QT_PLUGIN_TARGETS Qt6::QSQLiteDriverPlugin)
else()
    message(WARNING "未找到 Qt6::QSQLiteDriverPlugin，请确认 Qt 安装包含 Sql 组件驱动插件")
endif()

# Android：系统目录选择器(SAF)依赖 QtCore 私有 API(QAndroidActivityResultReceiver)
# 私有组件需单独 find_package 声明，否则 Qt6::CorePrivate 目标不存在
if(ANDROID)
    find_package(Qt6 REQUIRED COMPONENTS CorePrivate)
    target_link_libraries(appBLE_SAR PRIVATE Qt6::CorePrivate)
endif()

include(GNUInstallDirs)
install(TARGETS appBLE_SAR
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

set_target_properties(appBLE_SAR PROPERTIES
    # 关键：设置应用显示名称
    QT_ANDROID_APP_NAME "BLE-SAR"
)

