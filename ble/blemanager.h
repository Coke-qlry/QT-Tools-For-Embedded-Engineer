#pragma once

// =====================================================================
// blemanager.h - BLE 统一门面（Facade）
// 组合 扫描(BleScanner) / 广播(BleAdvertiser) / 连接(BleConnection) 三个模块，
// 以单一 QObject 暴露给 QML（全局对象名：bleManager）。
// 界面文件不做任何改动，QML 侧可直接调用：
//   bleManager.startScan();
//   bleManager.startAdvertise("BLE SAR", "0000fff0-...", 100);
//   bleManager.connectToDevice(address);
// =====================================================================

#include <QObject>
#include <QByteArray>
#include <QVariantList>

#include "blepermissions.h"
#include "bleterminal.h"

class BleScanner;
class BleAdvertiser;
class BleConnection;

class BleManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(bool advertising READ advertising NOTIFY advertisingChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    // 外设（Peripheral）被其它设备连接的状态
    Q_PROPERTY(bool peripheralConnected READ peripheralConnected
               NOTIFY peripheralConnectedChanged)
    // 本机系统蓝牙名称（Android 上即广播包中实际广播出去的名称）
    Q_PROPERTY(QString localDeviceName READ localDeviceName
               NOTIFY localDeviceNameChanged)
    Q_PROPERTY(BlePermissions *permissions READ permissions CONSTANT)
    // 调试终端模块（数据收发 / 十六进制 / 定时发送 / 导出）
    Q_PROPERTY(BleTerminal *terminal READ terminal CONSTANT)

public:
    explicit BleManager(QObject *parent = nullptr);

    bool scanning() const;
    bool advertising() const;
    bool connected() const;
    bool peripheralConnected() const;

    // 本机系统蓝牙名称（Android 上即广播包实际广播出去的名称）
    QString localDeviceName() const;
    // 重新读取系统蓝牙名称（授予蓝牙权限后可获取到真实名称）
    Q_INVOKABLE void refreshLocalDeviceName();

    // 返回权限管理对象（QML 通过 bleManager.permissions 访问）
    BlePermissions *permissions() const;
    // 返回调试终端对象（QML 通过 bleManager.terminal 访问）
    BleTerminal *terminal() const;

    // ---- 权限辅助（转发到 BlePermissions）----
    Q_INVOKABLE bool hasPermission(int permission) const;
    Q_INVOKABLE void requestPermission(int permission);

    // ---- 扫描 ----
    Q_INVOKABLE void startScan(int timeoutMs = 10000);
    Q_INVOKABLE void stopScan();
    Q_INVOKABLE void clearDevices();
    // 返回当前已发现的设备列表 [{name, address, rssi, isLe}, ...]
    Q_INVOKABLE QVariantList deviceList() const;

    // ---- 广播 ----
    Q_INVOKABLE void startAdvertise(
        const QString &localName = QStringLiteral("BLE_SAR"),
        const QString &serviceUuid = QString(),
        int intervalMs = 100);
    Q_INVOKABLE void stopAdvertise();
    // 外设（Peripheral）角色：向已连接设备下发数据 / 读取通知数据
    Q_INVOKABLE void sendPeripheralData(const QByteArray &data);

    // ---- GATT 连接 ----
    Q_INVOKABLE void connectToDevice(const QString &address);
    Q_INVOKABLE void disconnectFromDevice();
    Q_INVOKABLE void discoverServices();
    Q_INVOKABLE void discoverDetails(const QString &serviceUuid);
    Q_INVOKABLE QVariantList services() const;
    Q_INVOKABLE QVariantList characteristics(const QString &serviceUuid) const;
    Q_INVOKABLE void readCharacteristic(const QString &serviceUuid,
                                        const QString &charUuid);
    Q_INVOKABLE void writeCharacteristic(const QString &serviceUuid,
                                         const QString &charUuid,
                                         const QByteArray &data);
    Q_INVOKABLE void enableNotify(const QString &serviceUuid,
                                  const QString &charUuid,
                                  bool enable);

signals:
    void scanningChanged(bool scanning);
    void advertisingChanged(bool advertising);
    void connectedChanged(bool connected);
    // 外设（Peripheral）被其它设备连接 / 断开
    void peripheralConnectedChanged(bool connected);
    // 系统蓝牙名称变化
    void localDeviceNameChanged();
    // 外设（Peripheral）收到已连接设备写入的数据
    void peripheralDataReceived(const QByteArray &data);
    void deviceFound(const QString &name, const QString &address,
                     int rssi, bool isLe);
    void scanFinished();
    void servicesDiscovered(const QVariantList &services);
    void detailsDiscovered(const QString &serviceUuid);
    void dataReceived(const QString &serviceUuid, const QString &charUuid,
                      const QByteArray &data);
    void dataWritten(const QString &serviceUuid, const QString &charUuid,
                     const QByteArray &data);
    // 通知(CCCD)使能的最终结果（界面同步开关；失败时自动回退轮询读取）
    void notifyChanged(const QString &serviceUuid, const QString &charUuid,
                       bool enabled, bool success);
    void errorOccurred(const QString &message);

private:
    BlePermissions *m_permissions = nullptr;
    BleScanner *m_scanner = nullptr;
    BleAdvertiser *m_advertiser = nullptr;
    BleConnection *m_connection = nullptr;
    BleTerminal *m_terminal = nullptr;
};
