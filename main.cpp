#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>

#include "ble/blemanager.h"
#include "ble/blepermissions.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    // BLE 功能后端：扫描 / 广播 / GATT 连接（模块化，见 ble/ 目录）
    // QML 中通过全局对象 bleManager 访问，例如：
    //   bleManager.startScan();                                    // 开始扫描
    //   bleManager.stopScan();                                     // 停止扫描
    //   bleManager.connectToDevice(address);                       // 连接设备
    //   bleManager.startAdvertise("BLE SAR", serviceUuid, 100);    // 开始广播
    //   bleManager.stopAdvertise();                                // 停止广播
    BleManager bleManager;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("bleManager", &bleManager);
    // 注册权限枚举类型，QML 中可用 bleManager.permissions.requestPermission(
    // BlePermissions.ConnectPermission) 等方式请求权限
    qmlRegisterType<BlePermissions>("BLE_SAR.Ble", 1, 0, "BlePermissions");

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("BLE_SAR", "Main");

    return app.exec();
}
