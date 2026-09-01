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
//  - 广播名称作为“真实广播名称”（Complete Local Name）广播：主广播包携带
//    服务 UUID 与标志位（空间足够时也放名称），完整名称放入扫描响应包，
//    扫描端（如 nRF Connect）合并后显示的名称即为我们设置的广播名称，
//    且名称可随时修改。
//
// 平台说明：
//  - Android / Linux(BlueZ) / macOS / iOS 支持外设广播，功能完整启用；
//  - Windows 桌面版 Qt 不支持 BLE 外设广播，
//    此时 isSupported() 返回 false，startAdvertise() 会发送错误提示，
//    相关代码通过 BLE_ADVERTISING_SUPPORTED 宏在编译期裁剪。
// =====================================================================

#include <QObject>
#include <QByteArray>
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
public:
    explicit BleAdvertiser(QObject *parent = nullptr);
    ~BleAdvertiser() override;

    bool isAdvertising() const;   // 是否正在广播
    bool isConnected() const;     // 是否有 Central 设备已连接
    bool isSupported() const;     // 当前平台/适配器是否支持 BLE 外设广播

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
};
