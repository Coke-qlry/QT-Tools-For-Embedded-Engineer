cmake_minimum_required(VERSION 3.16)

project(BLE_SAR VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 模块：Quick(界面) + Bluetooth(BLE 扫描/广播/连接)
find_package(Qt6 REQUIRED COMPONENTS Quick Bluetooth)

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
)

qt_add_qml_module(appBLE_SAR
    URI BLE_SAR
    VERSION 1.0
    QML_FILES
        Main.qml
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
)

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

