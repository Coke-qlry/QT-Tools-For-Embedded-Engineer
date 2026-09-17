#pragma once

// =====================================================================
// blescanner.h - BLE 扫描模块
// 封装 QBluetoothDeviceDiscoveryAgent，负责周围 BLE 设备的发现、去重，
// 并向外部（QML / 其它模块）提供设备列表与信号。
// =====================================================================

#include <QObject>
#include <QList>
#include <QSet>
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

    // ---- 已绑定设备（用于扫描时自动连接）----
    // 设置已绑定设备的地址集合（统一小写）。后续扫描时若发现列表中的
    // 设备，将发出 boundDeviceDiscovered，触发自动连接。
    // 启动时由 BleManager 注入，启动后也可被解除绑定流程清空。
    void setBoundAddresses(const QStringList &addresses);

public slots:
    void startScan(int timeoutMs = 10000);
    void stopScan();
    void clear();

signals:
    void deviceFound(const QString &name, const QString &address, int rssi, bool isLe);
    // 扫描时发现已绑定设备。每轮扫描中同一地址仅触发一次，
    // 避免反复尝试连接造成骚扰；新一轮扫描会重置该去重集合。
    void boundDeviceDiscovered(const QString &name, const QString &address);
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
    QList<QBluetoothDeviceInfo> m_infos;  // 保留完整设备信息，用于后续连接
    QSet<QString> m_addresses;            // 已发现地址集合（O(1) 去重）
    bool m_scanning = false;
    // 已绑定地址（小写归一化）：扫描时命中则触发自动连接
    QSet<QString> m_boundAddresses;
    // 当前扫描轮内已触发过自动连接的地址：避免重复触发
    QSet<QString> m_autoConnectedThisScan;
};
