#include "bleconnection.h"

#include <QLowEnergyCharacteristic>
#include <QLowEnergyController>
#include <QLowEnergyDescriptor>
#include <QLowEnergyService>
#include <QDebug>
#include <QStringList>
#include <QTimer>
#include <QUuid>

// [BLE-DBG] 调试日志总开关：置 1 恢复输出，置 0 静默（等效注释全部调试打印）。
// 排查蓝牙连接 / 收发问题时改成 1 重新编译即可重新看到日志。
#define BLE_DBG_ENABLED 0
#if BLE_DBG_ENABLED
#  define BLE_DBG_LOG() qDebug()
#else
#  define BLE_DBG_LOG() QNoDebug()
#endif

namespace {

// Qt 6.10 已移除 QLowEnergyService::errorName()，这里提供本地描述
QString serviceErrorToString(QLowEnergyService::ServiceError error)
{
    switch (error) {
    case QLowEnergyService::NoError:
        return QStringLiteral("无错误");
    case QLowEnergyService::OperationError:
        return QStringLiteral("操作错误");
    case QLowEnergyService::CharacteristicWriteError:
        return QStringLiteral("特征写入错误");
    case QLowEnergyService::DescriptorWriteError:
        return QStringLiteral("描述符写入错误");
    case QLowEnergyService::CharacteristicReadError:
        return QStringLiteral("特征读取错误");
    case QLowEnergyService::DescriptorReadError:
        return QStringLiteral("描述符读取错误");
    case QLowEnergyService::UnknownError:
        break;
    }
    return QStringLiteral("未知错误");
}

// 调试辅助：特征属性位 -> 可读文本
QString propsToString(QLowEnergyCharacteristic::PropertyTypes props)
{
    QStringList flags;
    if (props & QLowEnergyCharacteristic::Broadcasting)
        flags << QLatin1String("Broadcast");
    if (props & QLowEnergyCharacteristic::Read)
        flags << QLatin1String("Read");
    if (props & QLowEnergyCharacteristic::WriteNoResponse)
        flags << QLatin1String("WriteNoResponse");
    if (props & QLowEnergyCharacteristic::Write)
        flags << QLatin1String("Write");
    if (props & QLowEnergyCharacteristic::Notify)
        flags << QLatin1String("Notify");
    if (props & QLowEnergyCharacteristic::Indicate)
        flags << QLatin1String("Indicate");
    if (props & QLowEnergyCharacteristic::WriteSigned)
        flags << QLatin1String("WriteSigned");
    if (props & QLowEnergyCharacteristic::ExtendedProperty)
        flags << QLatin1String("Extended");
    return flags.join(QLatin1String("|"));
}

// 调试辅助：数据转大写十六进制文本（带空格分隔）
QString dataToHex(const QByteArray &data)
{
    return QString::fromLatin1(data.toHex(' ').toUpper());
}

} // namespace

QString BleConnection::uuidString(const QBluetoothUuid &uuid)
{
    return uuid.toString(QUuid::WithoutBraces);
}

BleConnection::BleConnection(QObject *parent)
    : QObject(parent)
{
}

BleConnection::~BleConnection()
{
    if (m_controller)
        m_controller->disconnectFromDevice();
}

bool BleConnection::isConnected() const
{
    return m_connected;
}

void BleConnection::connectToDevice(const QBluetoothDeviceInfo &info)
{
    // 清理上一次连接
    if (m_controller) {
        m_controller->disconnectFromDevice();
        m_controller->deleteLater();
        m_controller = nullptr;
    }
    // 注意：服务对象（父对象为 this，由 createServiceObject 创建）并不会随
    // controller 销毁而释放，但删除它们必须等旧 controller 真正销毁之后——
    // 旧 controller 内部仍持有服务指针，若在 deleteLater 期间提前删除，
    // 其析构时会访问悬空指针导致 use-after-free 崩溃。
    // 因此这里仅清空索引：服务对象统一由下一次服务发现完成时
    // （onServiceDiscoveryFinished 中的 qDeleteAll）或 BleConnection 析构回收。
    m_services.clear();
    m_serviceObjects.clear();
    // 清理上一次连接遗留的通知使能写入队列（队列中的服务对象即将失效）
    resetNotifyQueue();

    m_controller = QLowEnergyController::createCentral(info, this);
    if (!m_controller) {
        emit errorOccurred(QStringLiteral("无法创建 GATT 控制器"));
        return;
    }

    connect(m_controller, &QLowEnergyController::connected,
            this, &BleConnection::onConnected);
    connect(m_controller, &QLowEnergyController::disconnected,
            this, &BleConnection::onDisconnected);
    connect(m_controller, &QLowEnergyController::discoveryFinished,
            this, &BleConnection::onServiceDiscoveryFinished);
    connect(m_controller,
            QOverload<QLowEnergyController::Error>::of(
                &QLowEnergyController::errorOccurred),
            this, [this](QLowEnergyController::Error) {
                emit errorOccurred(m_controller
                                       ? m_controller->errorString()
                                       : QStringLiteral("未知蓝牙错误"));
            });

    BLE_DBG_LOG() << "[BLE-DBG] ======== 发起 GATT 连接 ========";
    BLE_DBG_LOG() << "[BLE-DBG] 目标设备 name =" << info.name()
             << " rssi =" << info.rssi();
    emit stateChanged(QStringLiteral("正在连接..."));
    m_controller->connectToDevice();
}

void BleConnection::onConnected()
{
    // 忽略旧控制器（已 deleteLater 但尚未销毁）发出的迟到信号
    if (sender() != m_controller)
        return;
    BLE_DBG_LOG() << "[BLE-DBG] <<<<< GATT 链路已连接 >>>>>";
    m_connected = true;
    emit connectedChanged(true);
    emit stateChanged(QStringLiteral("已连接"));
    discoverServices();
}

void BleConnection::onDisconnected()
{
    if (sender() != m_controller)
        return;
    m_connected = false;
    // 断连后取消未完成的通知使能写入
    resetNotifyQueue();
    emit connectedChanged(false);
    emit stateChanged(QStringLiteral("已断开"));
}

void BleConnection::disconnectFromDevice()
{
    // 仅发起断连：disconnected 为异步信号，须等待其驱动状态更新；
    // 控制器与服务对象的回收统一交给下一次 connectToDevice() 或析构完成，
    // 避免在断连完成前销毁控制器导致状态无法回落到“已断开”。
    if (m_controller)
        m_controller->disconnectFromDevice();
}

void BleConnection::discoverServices()
{
    if (!m_controller || !m_connected) {
        emit errorOccurred(QStringLiteral("设备未连接"));
        return;
    }
    emit stateChanged(QStringLiteral("正在发现服务..."));
    m_controller->discoverServices();
}

void BleConnection::onServiceDiscoveryFinished()
{
    if (!m_controller || sender() != m_controller)
        return;

    // 重建服务对象（每次发现覆盖旧的）
    qDeleteAll(m_serviceObjects);
    m_serviceObjects.clear();
    m_services.clear();

    const QList<QBluetoothUuid> uuids = m_controller->services();
    BLE_DBG_LOG() << "[BLE-DBG] 服务发现完成，共" << uuids.size() << "个服务:";
    for (const QBluetoothUuid &uuid : uuids) {
        QLowEnergyService *service = m_controller->createServiceObject(uuid, this);
        if (service) {
            setupServiceObject(service);
            m_services.insert(uuid, service);
            m_serviceObjects.append(service);
            BLE_DBG_LOG() << "[BLE-DBG]     [service] " << uuidString(uuid);
        }
    }

    emit servicesDiscovered(services());
    emit stateChanged(QStringLiteral("服务发现完成"));
}

void BleConnection::setupServiceObject(QLowEnergyService *service)
{
    connect(service, &QLowEnergyService::stateChanged,
            this, &BleConnection::onServiceStateChanged);
    connect(service, &QLowEnergyService::characteristicChanged,
            this, &BleConnection::onCharacteristicChanged);
    connect(service, &QLowEnergyService::characteristicRead,
            this, &BleConnection::onCharacteristicRead);
    connect(service, &QLowEnergyService::characteristicWritten,
            this, &BleConnection::onCharacteristicWritten);
    connect(service, &QLowEnergyService::descriptorWritten,
            this, &BleConnection::onNotifyDescriptorWritten);
    connect(service,
            QOverload<QLowEnergyService::ServiceError>::of(
                &QLowEnergyService::errorOccurred),
            this, [this](QLowEnergyService::ServiceError error) {
                // CCCD 通知使能写入失败：交给通知队列统一重试处理
                // （自动开启多个通知时，失败通常由并发写引起，重试即可恢复，
                // 不在这里重复弹出“服务错误”干扰用户）
                if (error == QLowEnergyService::DescriptorWriteError
                    && m_notifyBusy
                    && sender() == m_activeNotify.service) {
                    finishNotifyWrite(false);
                    return;
                }
                emit errorOccurred(
                    QStringLiteral("服务错误: %1")
                        .arg(serviceErrorToString(error)));
            });
}

void BleConnection::onServiceStateChanged(QLowEnergyService::ServiceState state)
{
    if (state != QLowEnergyService::RemoteServiceDiscovered)
        return;
    const auto *service = qobject_cast<QLowEnergyService *>(sender());
    if (!service)
        return;
    BLE_DBG_LOG() << "[BLE-DBG] ---- 服务详情发现完成:"
             << uuidString(service->serviceUuid());
    const QList<QLowEnergyCharacteristic> chars = service->characteristics();
    for (const QLowEnergyCharacteristic &c : chars) {
        const QLowEnergyDescriptor cccd = c.descriptor(
            QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
        BLE_DBG_LOG() << "[BLE-DBG]     [char] uuid =" << uuidString(c.uuid())
                 << " name =" << c.name()
                 << " props =" << propsToString(c.properties())
                 << (cccd.isValid() ? " [有CCCD可订阅]" : " [无CCCD]");
    }
    emit detailsDiscovered(uuidString(service->serviceUuid()));
}

void BleConnection::discoverDetails(const QString &serviceUuid)
{
    QLowEnergyService *service = findService(QBluetoothUuid::fromString(serviceUuid));
    if (!service) {
        emit errorOccurred(QStringLiteral("服务不存在: %1").arg(serviceUuid));
        return;
    }
    BLE_DBG_LOG() << "[BLE-DBG] >>> 请求发现服务详情(discoverDetails):" << serviceUuid;
    service->discoverDetails();
}

QLowEnergyService *BleConnection::findService(const QBluetoothUuid &uuid) const
{
    return m_services.value(uuid, nullptr);
}

QLowEnergyCharacteristic BleConnection::findCharacteristic(
    QLowEnergyService *service, const QString &charUuid) const
{
    const QBluetoothUuid target = QBluetoothUuid::fromString(charUuid);
    const QList<QLowEnergyCharacteristic> chars = service->characteristics();
    for (const QLowEnergyCharacteristic &c : chars) {
        if (c.uuid() == target)
            return c;
    }
    return QLowEnergyCharacteristic();
}

QVariantList BleConnection::services() const
{
    QVariantList result;
    for (QLowEnergyService *service : m_services) {
        QVariantMap item;
        item.insert(QStringLiteral("uuid"), uuidString(service->serviceUuid()));
        item.insert(QStringLiteral("name"), service->serviceName());
        item.insert(QStringLiteral("state"), static_cast<int>(service->state()));
        result.append(item);
    }
    return result;
}

QVariantList BleConnection::characteristics(const QString &serviceUuid) const
{
    QVariantList result;
    QLowEnergyService *service = findService(QBluetoothUuid::fromString(serviceUuid));
    if (!service || service->state() != QLowEnergyService::RemoteServiceDiscovered)
        return result;

    const QList<QLowEnergyCharacteristic> chars = service->characteristics();
    for (const QLowEnergyCharacteristic &c : chars) {
        QVariantMap item;
        item.insert(QStringLiteral("uuid"), uuidString(c.uuid()));
        item.insert(QStringLiteral("name"), c.name());
        item.insert(QStringLiteral("properties"), static_cast<int>(c.properties()));
        item.insert(QStringLiteral("value"), c.value());
        result.append(item);
    }
    return result;
}

void BleConnection::readCharacteristic(const QString &serviceUuid,
                                       const QString &charUuid)
{
    QLowEnergyService *service = findService(QBluetoothUuid::fromString(serviceUuid));
    if (!service) {
        emit errorOccurred(QStringLiteral("服务不存在: %1").arg(serviceUuid));
        return;
    }
    if (service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        emit errorOccurred(QStringLiteral("服务详情尚未发现完成"));
        return;
    }
    const QLowEnergyCharacteristic c = findCharacteristic(service, charUuid);
    if (!c.isValid()) {
        emit errorOccurred(QStringLiteral("特征不存在: %1").arg(charUuid));
        return;
    }
    BLE_DBG_LOG() << "[BLE-DBG] >>> 主动读取(Read请求): service =" << serviceUuid
             << " char =" << charUuid;
    service->readCharacteristic(c);
}

void BleConnection::writeCharacteristic(const QString &serviceUuid,
                                        const QString &charUuid,
                                        const QByteArray &data)
{
    QLowEnergyService *service = findService(QBluetoothUuid::fromString(serviceUuid));
    if (!service) {
        emit errorOccurred(QStringLiteral("服务不存在: %1").arg(serviceUuid));
        return;
    }
    if (service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        emit errorOccurred(QStringLiteral("服务详情尚未发现完成"));
        return;
    }
    const QLowEnergyCharacteristic c = findCharacteristic(service, charUuid);
    if (!c.isValid()) {
        emit errorOccurred(QStringLiteral("特征不存在: %1").arg(charUuid));
        return;
    }
    const bool withResponse =
        bool(c.properties() & QLowEnergyCharacteristic::Write);
    const bool noResponse =
        bool(c.properties() & QLowEnergyCharacteristic::WriteNoResponse);
    if (!withResponse && !noResponse) {
        emit errorOccurred(QStringLiteral("所选特征不支持写入(Write)"));
        return;
    }
    // 优先按特征自身能力决定写入模式，避免只支持
    // WriteNoResponse 的特征用 WriteWithResponse 写入失败。
    service->writeCharacteristic(
        c, data, withResponse ? QLowEnergyService::WriteWithResponse
                              : QLowEnergyService::WriteWithoutResponse);
}

void BleConnection::enableNotify(const QString &serviceUuid,
                                 const QString &charUuid,
                                 bool enable)
{
    QLowEnergyService *service = findService(QBluetoothUuid::fromString(serviceUuid));
    if (!service) {
        emit errorOccurred(QStringLiteral("服务不存在: %1").arg(serviceUuid));
        return;
    }
    if (service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        emit errorOccurred(QStringLiteral("服务详情尚未发现完成"));
        return;
    }
    const QLowEnergyCharacteristic c = findCharacteristic(service, charUuid);
    if (!c.isValid()) {
        emit errorOccurred(QStringLiteral("特征不存在: %1").arg(charUuid));
        return;
    }
    const QLowEnergyDescriptor cccd = c.descriptor(
        QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
    if (!cccd.isValid()) {
        // 关键：Qt 在部分 Android 机型/蓝牙栈上读不到 CCCD 描述符
        // （即使特征属性声明了 Notify）。此时必须把"通知不可用"同步给目录，
        // 否则界面会误以为通知已开启，导致可读特征连自动轮询回退也被禁用，
        // 最终只能手动点"读取"。
        BLE_DBG_LOG() << "[BLE-DBG] !!! enableNotify被拒: 该特征无CCCD描述符"
                 << "(Android上偶发), 无法订阅通知: service =" << serviceUuid
                 << " char =" << charUuid
                 << " 可读 =" << bool(c.properties()
                                      & QLowEnergyCharacteristic::Read);
        const bool readableProp = bool(
            c.properties() & QLowEnergyCharacteristic::Read);
        emit notifyChanged(serviceUuid, charUuid, enable, false);
        emit errorOccurred(
            readableProp
                ? QStringLiteral("该特征通知开启失败，已自动改用轮询读取")
                : QStringLiteral("该特征不支持通知(Notify)"));
        return;
    }
    // 0x0001 = Notifications, 0x0002 = Indications
    NotifyWrite w;
    w.service = service;
    w.characteristic = c;
    w.enable = enable;
    w.value = enable ? QByteArray::fromHex("0100")
                     : QByteArray::fromHex("0000");
    BLE_DBG_LOG() << "[BLE-DBG] >>> enableNotify(" << (enable ? "开" : "关")
             << ") service =" << serviceUuid
             << " char =" << charUuid
             << " props =" << propsToString(c.properties());
    // 入队串行写入，避免多条连接上并发 GATT 写导致部分通知开启失败
    m_notifyQueue.append(w);
    pumpNotifyQueue();
}

// ---------------------------------------------------------------------
// 通知(CCCD)写入队列：每次只发一个写请求，前一个完成后才发下一个
// ---------------------------------------------------------------------
void BleConnection::pumpNotifyQueue()
{
    if (m_notifyBusy || m_notifyQueue.isEmpty())
        return;
    m_activeNotify = m_notifyQueue.takeFirst();
    m_notifyBusy = true;

    if (!m_notifyTimeout) {
        m_notifyTimeout = new QTimer(this);
        m_notifyTimeout->setSingleShot(true);
        m_notifyTimeout->setInterval(8000);   // 8s 无回执视为失败
        connect(m_notifyTimeout, &QTimer::timeout,
                this, &BleConnection::onNotifyQueueTimeout);
    }
    m_notifyTimeout->start();

    const QLowEnergyDescriptor cccd = m_activeNotify.characteristic.descriptor(
        QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
    BLE_DBG_LOG() << "[BLE-DBG] === 开始写CCCD(订阅): service ="
             << uuidString(m_activeNotify.service->serviceUuid())
             << " char =" << uuidString(m_activeNotify.characteristic.uuid())
             << " 写入值 =" << dataToHex(m_activeNotify.value)
             << "(0100=Notify 0200=Indicate)";
    m_activeNotify.service->writeDescriptor(cccd, m_activeNotify.value);
}

void BleConnection::onNotifyDescriptorWritten(
    const QLowEnergyDescriptor &descriptor, const QByteArray &value)
{
    Q_UNUSED(value);
    if (!m_notifyBusy)
        return;
    const auto *service = qobject_cast<QLowEnergyService *>(sender());
    if (service != m_activeNotify.service)
        return;
    if (descriptor.uuid()
        != QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration)
        return;
    // 注意：不校验回执 value 是否与写入值一致。部分 Android 机型/蓝牙栈
    // 的回执值可能与请求值不同，若严格比对会把成功的 CCCD 写入误判为失败，
    // 导致自动通知未生效、只能手动点"读取"才有数据。
    finishNotifyWrite(true);
}

void BleConnection::onNotifyQueueTimeout()
{
    if (m_notifyBusy)
        finishNotifyWrite(false);
}

void BleConnection::finishNotifyWrite(bool success)
{
    if (!m_notifyBusy)
        return;
    if (m_notifyTimeout)
        m_notifyTimeout->stop();
    NotifyWrite finished = m_activeNotify;
    m_activeNotify = NotifyWrite();
    m_notifyBusy = false;

    if (!success && finished.retries > 0) {
        BLE_DBG_LOG() << "[BLE-DBG] !!! CCCD订阅写入失败，100ms后自动重试一次: char ="
                 << uuidString(finished.characteristic.uuid());
        // CCCD 写入偶发失败（连接拥挤 / 设备繁忙）：短暂延迟后重试一次
        --finished.retries;
        m_notifyQueue.prepend(finished);
        QTimer::singleShot(100, this, &BleConnection::pumpNotifyQueue);
        return;
    }
    if (!success) {
        BLE_DBG_LOG() << "[BLE-DBG] !!! CCCD订阅写入最终失败(重试耗尽): service ="
                 << uuidString(finished.service->serviceUuid())
                 << " char =" << uuidString(finished.characteristic.uuid())
                 << " enable =" << finished.enable;
        // 重试耗尽仍失败：告知界面该特征通知未生效。
        // 若特征可读，界面会自动回退为“轮询读取”，数据仍能自动收到；
        // 不可读则只能提示用户手动处理。
        emit notifyChanged(uuidString(finished.service->serviceUuid()),
                           uuidString(finished.characteristic.uuid()),
                           finished.enable, false);
        if (finished.enable) {
            const bool readable = bool(
                finished.characteristic.properties()
                & QLowEnergyCharacteristic::Read);
            emit errorOccurred(
                readable ? QStringLiteral("该特征通知开启失败，已自动改用轮询读取")
                         : QStringLiteral("自动开启通知失败，"
                                          "可展开「服务与特征」手动开启"));
        }
    } else {
        BLE_DBG_LOG() << "[BLE-DBG] +++ CCCD订阅写入成功: service ="
                 << uuidString(finished.service->serviceUuid())
                 << " char =" << uuidString(finished.characteristic.uuid())
                 << " enable =" << finished.enable;
        emit notifyChanged(uuidString(finished.service->serviceUuid()),
                           uuidString(finished.characteristic.uuid()),
                           finished.enable, true);
    }
    pumpNotifyQueue();
}

void BleConnection::resetNotifyQueue()
{
    m_notifyQueue.clear();
    m_activeNotify = NotifyWrite();
    m_notifyBusy = false;
    if (m_notifyTimeout)
        m_notifyTimeout->stop();
}

void BleConnection::onCharacteristicChanged(
    const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue)
{
    const auto *service = qobject_cast<QLowEnergyService *>(sender());
    // 通知/指示推送到达：说明 CCCD 订阅已生效，设备在主动上报
    BLE_DBG_LOG() << "[BLE-DBG] <<< 【通知推送 NOTIFY】 service ="
             << (service ? uuidString(service->serviceUuid()) : QString())
             << " char =" << uuidString(characteristic.uuid())
             << " len =" << newValue.size()
             << " hex =" << dataToHex(newValue);
    emit dataReceived(service ? uuidString(service->serviceUuid()) : QString(),
                      uuidString(characteristic.uuid()), newValue);
}

void BleConnection::onCharacteristicRead(
    const QLowEnergyCharacteristic &characteristic, const QByteArray &value)
{
    const auto *service = qobject_cast<QLowEnergyService *>(sender());
    // Read 回包到达：由"手动点读取"或"自动轮询读取"触发
    BLE_DBG_LOG() << "[BLE-DBG] <<< 【读取回包 READ】 service ="
             << (service ? uuidString(service->serviceUuid()) : QString())
             << " char =" << uuidString(characteristic.uuid())
             << " len =" << value.size()
             << " hex =" << dataToHex(value);
    emit dataReceived(service ? uuidString(service->serviceUuid()) : QString(),
                      uuidString(characteristic.uuid()), value);
}

void BleConnection::onCharacteristicWritten(
    const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue)
{
    const auto *service = qobject_cast<QLowEnergyService *>(sender());
    emit dataWritten(service ? uuidString(service->serviceUuid()) : QString(),
                     uuidString(characteristic.uuid()), newValue);
}
