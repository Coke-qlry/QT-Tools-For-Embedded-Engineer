#pragma once

// =====================================================================
// bleadvertiser.h - BLE 外设（Peripheral）模块：真实可连接的 GATT 服务 + 广播
//
// 说明：Qt 6.10 已移除独立的 QBluetoothLowEnergyAdvertisingManager 类，
// 外设广播统一通过 QLowEnergyController::createPeripheral() 创建外设控制器，
// 再调用 startAdvertising()/stopAdvertising() 进行广播管理。
//
// 本模块提供一个真实可连接的 BLE 外设：
//  - 内置一个 SAR 数据 GATT 服务（serviceUuid），含可读/可写/可通知特征；
//  - 使用 AdvInd（可连接广播）模式，其他设备可扫描到并可连接；
//  - 广播名称：Linux / macOS / iOS 平台使用应用设置的名称作为
//    “Complete Local Name”广播（主广播包空间足够时放名称，完整名称放
//    扫描响应包，扫描端合并后显示）；Android 平台受系统限制只能广播
//    系统蓝牙名称，程序把系统蓝牙名读出并暴露为 localDeviceName 属性，
//    修改名称需到系统「设置 → 蓝牙 → 设备名称」。
//
// 平台说明：
//  - Android / Linux(BlueZ) / macOS / iOS 支持外设广播，功能完整启用；
//  - Windows 桌面版 Qt 不支持 BLE 外设广播，
//    此时 isSupported() 返回 false，startAdvertise() 会发送错误提示，
//    相关代码通过 BLE_ADVERTISING_SUPPORTED 宏在编译期裁剪。
// =====================================================================

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QtGlobal>

// 支持 BLE 外设广播的平台
#if defined(Q_OS_ANDROID) || defined(Q_OS_LINUX) || defined(Q_OS_MACOS) || defined(Q_OS_IOS)
#  define BLE_ADVERTISING_SUPPORTED
#endif

#ifdef BLE_ADVERTISING_SUPPORTED
// m_sarChar 作为值成员存储，需要完整类型（不能仅前向声明）
#include <QBluetoothUuid>
#include <QLowEnergyCharacteristic>
QT_BEGIN_NAMESPACE
class QLowEnergyController;
class QLowEnergyService;
QT_END_NAMESPACE
#endif

class BleAdvertiser : public QObject
{
    Q_OBJECT
    // 本机系统蓝牙名称（Android 上即广播包中实际广播出去的名称）
    Q_PROPERTY(QString localDeviceName READ localDeviceName
               NOTIFY localDeviceNameChanged)
public:
    explicit BleAdvertiser(QObject *parent = nullptr);
    ~BleAdvertiser() override;

    bool isAdvertising() const;   // 是否正在广播
    bool isConnected() const;     // 是否有 Central 设备已连接
    bool isSupported() const;     // 当前平台/适配器是否支持 BLE 外设广播
    // 本机系统蓝牙名称（Android 上即广播包实际广播的名称）
    QString localDeviceName() const;
    // 重新读取系统蓝牙名称（Android 12+ 需 BLUETOOTH_CONNECT 权限，
    // 权限授予后调用可获取到真实名称）
    Q_INVOKABLE void refreshLocalDeviceName();

public slots:
    void startAdvertise(const QString &localName = QStringLiteral("BLE SAR"),
                        const QString &serviceUuid = QString(),
                        int intervalMs = 100);
    void stopAdvertise();
    // 向已连接设备写入 SAR 数据（通过通知特征下发）
    void sendData(const QByteArray &data);

signals:
    void advertisingChanged(bool advertising);
    void connectedChanged(bool connected);
    // 系统蓝牙名称变化（权限授予 / 用户改名后刷新）
    void localDeviceNameChanged();
    // 收到 Central 设备通过可写特征发来的数据
    void dataReceived(const QByteArray &data);
    void errorOccurred(const QString &message);

private:
#ifdef BLE_ADVERTISING_SUPPORTED
    void setupGattService(const QBluetoothUuid &serviceUuid);
    void setupControllerConnections();
    QLowEnergyController *m_controller = nullptr;
    QLowEnergyService *m_sarService = nullptr;
    QLowEnergyCharacteristic m_sarChar;      // 可读/可写/可通知特征
    QBluetoothUuid m_serviceUuid;            // 当前已注册的 GATT 服务 UUID
#endif
    bool m_advertising = false;
    bool m_connected = false;
    // 系统蓝牙名称（Android 上即广播包实际广播的名称），默认占位名
    QString m_localDeviceName = QStringLiteral("BLE_SAR");
};
