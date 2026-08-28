#pragma once

// =====================================================================
// blescanner.h - BLE 扫描模块
// 封装 QBluetoothDeviceDiscoveryAgent，负责周围 BLE 设备的发现、去重，
// 并向外部（QML / 其它模块）提供设备列表与信号。
// =====================================================================

#include <QObject>
#include <QList>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>

class BleScanner : public QObject
{
    Q_OBJECT
public:
    struct DeviceEntry
    {
        QString name;      // 广播名（可能为空）
        QString address;   // 地址 / 设备标识
        int rssi = 0;      // 信号强度 dBm
        bool isLe = false; // 是否为低功耗蓝牙设备
    };

    explicit BleScanner(QObject *parent = nullptr);

    bool isScanning() const;
    QList<DeviceEntry> devices() const;

    // 按地址查找完整设备信息（供 GATT 连接使用）
    QBluetoothDeviceInfo findDevice(const QString &address) const;

public slots:
    void startScan(int timeoutMs = 10000);
    void stopScan();
    void clear();

signals:
    void deviceFound(const QString &name, const QString &address, int rssi, bool isLe);
    void scanningChanged(bool scanning);
    void scanFinished();
    void errorOccurred(const QString &message);

private slots:
    void onDeviceDiscovered(const QBluetoothDeviceInfo &info);
    void onScanFinished();
    void onScanCanceled();

private:
    void setScanning(bool scanning);

    QBluetoothDeviceDiscoveryAgent *m_agent = nullptr;
    QList<QBluetoothDeviceInfo> m_infos; // 保留完整设备信息，用于后续连接
    bool m_scanning = false;
};
