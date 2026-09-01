#include "bleconnection.h"

#include <QLowEnergyCharacteristic>
#include <QLowEnergyController>
#include <QLowEnergyDescriptor>
#include <QLowEnergyService>
#include <QUuid>

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

    emit stateChanged(QStringLiteral("正在连接..."));
    m_controller->connectToDevice();
}

void BleConnection::onConnected()
{
    // 忽略旧控制器（已 deleteLater 但尚未销毁）发出的迟到信号
    if (sender() != m_controller)
        return;
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
    for (const QBluetoothUuid &uuid : uuids) {
        QLowEnergyService *service = m_controller->createServiceObject(uuid, this);
        if (service) {
            setupServiceObject(service);
            m_services.insert(uuid, service);
            m_serviceObjects.append(service);
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
    connect(service,
            QOverload<QLowEnergyService::ServiceError>::of(
                &QLowEnergyService::errorOccurred),
            this, [this](QLowEnergyService::ServiceError error) {
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
    if (service)
        emit detailsDiscovered(uuidString(service->serviceUuid()));
}

void BleConnection::discoverDetails(const QString &serviceUuid)
{
    QLowEnergyService *service = findService(QBluetoothUuid::fromString(serviceUuid));
    if (!service) {
        emit errorOccurred(QStringLiteral("服务不存在: %1").arg(serviceUuid));
        return;
    }
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
    service->writeCharacteristic(c, data, QLowEnergyService::WriteWithResponse);
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
        emit errorOccurred(QStringLiteral("该特征不支持通知(Notify)"));
        return;
    }
    // 0x0001 = Notifications, 0x0002 = Indications
    const QByteArray value = enable ? QByteArray::fromHex("0100")
                                    : QByteArray::fromHex("0000");
    service->writeDescriptor(cccd, value);
}

void BleConnection::onCharacteristicChanged(
    const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue)
{
    const auto *service = qobject_cast<QLowEnergyService *>(sender());
    emit dataReceived(service ? uuidString(service->serviceUuid()) : QString(),
                      uuidString(characteristic.uuid()), newValue);
}

void BleConnection::onCharacteristicRead(
    const QLowEnergyCharacteristic &characteristic, const QByteArray &value)
{
    const auto *service = qobject_cast<QLowEnergyService *>(sender());
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
