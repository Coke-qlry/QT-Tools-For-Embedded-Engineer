#pragma once

// =====================================================================
// bleadvertiser.h - BLE 广播模块（外设 Peripheral 角色）
//
// 说明：Qt 6.10 已移除独立的 QBluetoothLowEnergyAdvertisingManager 类，
// 外设广播统一通过 QLowEnergyController::createPeripheral() 创建外设控制器，
// 再调用 startAdvertising()/stopAdvertising() 进行广播管理。
//
// 平台说明：
//  - Android / Linux(BlueZ) / macOS / iOS 支持外设广播，功能完整启用；
//  - Windows 桌面版 Qt 不支持 BLE 外设广播，
//    此时 isSupported() 返回 false，startAdvertise() 会发送错误提示，
//    相关代码通过 BLE_ADVERTISING_SUPPORTED 宏在编译期裁剪。
// =====================================================================

#include <QObject>
#include <QtGlobal>

// 支持 BLE 外设广播的平台
#if defined(Q_OS_ANDROID) || defined(Q_OS_LINUX) || defined(Q_OS_MACOS) || defined(Q_OS_IOS)
#  define BLE_ADVERTISING_SUPPORTED
#endif

#ifdef BLE_ADVERTISING_SUPPORTED
QT_BEGIN_NAMESPACE
class QLowEnergyController;
QT_END_NAMESPACE
#endif

class BleAdvertiser : public QObject
{
    Q_OBJECT
public:
    explicit BleAdvertiser(QObject *parent = nullptr);

    bool isAdvertising() const;
    bool isSupported() const; // 当前平台/适配器是否支持 BLE 外设广播

public slots:
    void startAdvertise(const QString &localName = QStringLiteral("BLE SAR"),
                        const QString &serviceUuid = QString(),
                        int intervalMs = 100);
    void stopAdvertise();

signals:
    void advertisingChanged(bool advertising);
    void errorOccurred(const QString &message);

private:
#ifdef BLE_ADVERTISING_SUPPORTED
    QLowEnergyController *m_controller = nullptr;
#endif
    bool m_advertising = false;
};
