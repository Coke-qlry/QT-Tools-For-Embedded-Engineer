#pragma once

// =====================================================================
// bleconnection.h - BLE GATT 连接模块
// 封装 QLowEnergyController + QLowEnergyService，实现：
//   - 连接指定设备（中央模式 Central）
//   - 发现服务 / 特征
//   - 特征读写、使能通知(Notify/Indicate)
// 与 QML 交互均使用字符串 UUID 与 QByteArray 数据。
// =====================================================================

#include <QObject>
#include <QByteArray>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
#include <QHash>
#include <QList>
#include <QVariantList>
// moc 需要完整类型以解析槽参数（QLowEnergyService::ServiceState 等）
#include <QLowEnergyCharacteristic>
#include <QLowEnergyService>

QT_BEGIN_NAMESPACE
class QLowEnergyController;
QT_END_NAMESPACE

class QTimer;

class BleConnection : public QObject
{
    Q_OBJECT
public:
    explicit BleConnection(QObject *parent = nullptr);
    ~BleConnection() override;

    bool isConnected() const;

    // C++ 侧连接入口：使用扫描得到的完整设备信息发起 GATT 连接
    void connectToDevice(const QBluetoothDeviceInfo &info);

public slots:
    void disconnectFromDevice();
    void discoverServices();

    // 发现指定服务的特征详情（发现完成后发 detailsDiscovered 信号）
    void discoverDetails(const QString &serviceUuid);

    // 返回 [{uuid, name, state}, ...] 列表，供 QML 展示
    QVariantList services() const;
    // 返回 [{uuid, name, properties, value}, ...] 列表
    QVariantList characteristics(const QString &serviceUuid) const;

    void readCharacteristic(const QString &serviceUuid, const QString &charUuid);
    void writeCharacteristic(const QString &serviceUuid,
                             const QString &charUuid,
                             const QByteArray &data);
    void enableNotify(const QString &serviceUuid,
                      const QString &charUuid,
                      bool enable);

signals:
    void connectedChanged(bool connected);
    void stateChanged(const QString &state);
    void errorOccurred(const QString &message);
    void servicesDiscovered(const QVariantList &services);
    void detailsDiscovered(const QString &serviceUuid);
    void dataReceived(const QString &serviceUuid,
                      const QString &charUuid,
                      const QByteArray &data);
    void dataWritten(const QString &serviceUuid,
                     const QString &charUuid,
                     const QByteArray &data);
    // 通知(CCCD)写入最终结果（成功或重试耗尽后的失败）：
    // 通知界面据此同步“通知开/关”状态；失败时界面会把该特征
    // 回退为“自动轮询读取”，保证只读型特征也能自动收数据。
    void notifyChanged(const QString &serviceUuid, const QString &charUuid,
                       bool enabled, bool success);

private slots:
    void onConnected();
    void onDisconnected();
    void onServiceDiscoveryFinished();
    void onServiceStateChanged(QLowEnergyService::ServiceState state);
    void onCharacteristicChanged(const QLowEnergyCharacteristic &characteristic,
                                 const QByteArray &newValue);
    void onCharacteristicRead(const QLowEnergyCharacteristic &characteristic,
                              const QByteArray &value);
    void onCharacteristicWritten(const QLowEnergyCharacteristic &characteristic,
                                 const QByteArray &newValue);
    // CCCD（通知使能描述符）写入完成 / 队列超时（串行自动开启通知用）
    void onNotifyDescriptorWritten(const QLowEnergyDescriptor &descriptor,
                                   const QByteArray &value);
    void onNotifyQueueTimeout();

private:
    QLowEnergyService *findService(const QBluetoothUuid &uuid) const;
    QLowEnergyCharacteristic findCharacteristic(QLowEnergyService *service,
                                                const QString &charUuid) const;
    void setupServiceObject(QLowEnergyService *service);
    static QString uuidString(const QBluetoothUuid &uuid);

    // 通知(CCCD)使能写入串行化：
    // Android BLE 不允许对同一条连接并发发起多个 GATT 写操作，
    // 连接后自动开启多个特征通知时若连续 writeDescriptor，后面的会失败，
    // 导致用户还需手动去「服务与特征」里逐个点开。这里改为队列逐一执行，
    // 前一个完成（descriptorWritten/失败/超时）后再写下一个。
    struct NotifyWrite {
        QLowEnergyService *service = nullptr;
        QLowEnergyCharacteristic characteristic;
        QByteArray value;
        bool enable = true;   // 本次请求是开启还是关闭通知
        int retries = 1;      // 失败后自动重试次数
    };
    void resetNotifyQueue();
    void pumpNotifyQueue();               // 队列空闲则取下一个执行
    void finishNotifyWrite(bool success); // 完成一次写入，驱动队列

    QList<NotifyWrite> m_notifyQueue;
    NotifyWrite m_activeNotify;
    bool m_notifyBusy = false;
    QTimer *m_notifyTimeout = nullptr;

    QLowEnergyController *m_controller = nullptr;
    QList<QLowEnergyService *> m_serviceObjects;
    QHash<QBluetoothUuid, QLowEnergyService *> m_services;
    bool m_connected = false;
};
