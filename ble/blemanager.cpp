#include "blemanager.h"

#include "bleadvertiser.h"
#include "bleconnection.h"
#include "blepermissions.h"
#include "blescanner.h"
#include "bleterminal.h"

#include <QBluetoothDeviceInfo>

BleManager::BleManager(QObject *parent)
    : QObject(parent)
{
    m_permissions = new BlePermissions(this);
    m_scanner = new BleScanner(this);
    m_advertiser = new BleAdvertiser(this);
    m_connection = new BleConnection(this);
    m_terminal = new BleTerminal(this);

    // 扫描模块信号转发
    connect(m_scanner, &BleScanner::deviceFound,
            this, &BleManager::deviceFound);
    connect(m_scanner, &BleScanner::scanningChanged,
            this, &BleManager::scanningChanged);
    connect(m_scanner, &BleScanner::scanFinished,
            this, &BleManager::scanFinished);
    connect(m_scanner, &BleScanner::errorOccurred,
            this, &BleManager::errorOccurred);

    // 广播模块信号转发
    connect(m_advertiser, &BleAdvertiser::advertisingChanged,
            this, &BleManager::advertisingChanged);
    connect(m_advertiser, &BleAdvertiser::errorOccurred,
            this, &BleManager::errorOccurred);
    // 外设（Peripheral）被连接 / 收到数据
    connect(m_advertiser, &BleAdvertiser::connectedChanged,
            this, &BleManager::peripheralConnectedChanged);
    connect(m_advertiser, &BleAdvertiser::dataReceived,
            this, &BleManager::peripheralDataReceived);
    // 系统蓝牙名称变化（Android 上即广播实际名称）
    connect(m_advertiser, &BleAdvertiser::localDeviceNameChanged,
            this, &BleManager::localDeviceNameChanged);

    // 连接模块信号转发
    connect(m_connection, &BleConnection::connectedChanged,
            this, &BleManager::connectedChanged);
    connect(m_connection, &BleConnection::servicesDiscovered,
            this, &BleManager::servicesDiscovered);
    connect(m_connection, &BleConnection::detailsDiscovered,
            this, &BleManager::detailsDiscovered);
    connect(m_connection, &BleConnection::dataReceived,
            this, &BleManager::dataReceived);
    connect(m_connection, &BleConnection::dataWritten,
            this, &BleManager::dataWritten);
    connect(m_connection, &BleConnection::notifyChanged,
            this, &BleManager::notifyChanged);
    connect(m_connection, &BleConnection::errorOccurred,
            this, &BleManager::errorOccurred);

    // 调试终端模块：监听连接状态与收发的数据
    connect(this, &BleManager::connectedChanged,
            m_terminal, &BleTerminal::setCentralConnected);
    connect(this, &BleManager::peripheralConnectedChanged,
            m_terminal, &BleTerminal::setPeripheralConnected);
    connect(this, &BleManager::dataReceived,
            m_terminal, &BleTerminal::onCentralData);
    connect(m_advertiser, &BleAdvertiser::dataReceived,
            m_terminal, &BleTerminal::onPeripheralData);
}

bool BleManager::scanning() const
{
    return m_scanner->isScanning();
}

bool BleManager::advertising() const
{
    return m_advertiser->isAdvertising();
}

bool BleManager::connected() const
{
    return m_connection->isConnected();
}

bool BleManager::peripheralConnected() const
{
    return m_advertiser->isConnected();
}

QString BleManager::localDeviceName() const
{
    return m_advertiser ? m_advertiser->localDeviceName() : QString();
}

void BleManager::refreshLocalDeviceName()
{
    if (m_advertiser)
        m_advertiser->refreshLocalDeviceName();
}

BlePermissions *BleManager::permissions() const
{
    return m_permissions;
}

BleTerminal *BleManager::terminal() const
{
    return m_terminal;
}

bool BleManager::hasPermission(int permission) const
{
    return m_permissions->hasPermission(
        static_cast<BlePermissions::Permission>(permission));
}

void BleManager::requestPermission(int permission)
{
    m_permissions->requestPermission(
        static_cast<BlePermissions::Permission>(permission));
}

// ---- 扫描 ----

void BleManager::startScan(int timeoutMs)
{
    m_scanner->startScan(timeoutMs);
}

void BleManager::stopScan()
{
    m_scanner->stopScan();
}

void BleManager::clearDevices()
{
    m_scanner->clear();
}

QVariantList BleManager::deviceList() const
{
    QVariantList result;
    const QList<BleScanner::DeviceEntry> entries = m_scanner->devices();
    for (const BleScanner::DeviceEntry &e : entries) {
        QVariantMap item;
        item.insert(QStringLiteral("name"), e.name);
        item.insert(QStringLiteral("address"), e.address);
        item.insert(QStringLiteral("rssi"), e.rssi);
        item.insert(QStringLiteral("isLe"), e.isLe);
        result.append(item);
    }
    return result;
}

// ---- 广播 ----

void BleManager::startAdvertise(const QString &localName,
                                const QString &serviceUuid,
                                int intervalMs)
{
    m_advertiser->startAdvertise(localName, serviceUuid, intervalMs);
}

void BleManager::stopAdvertise()
{
    m_advertiser->stopAdvertise();
}

void BleManager::sendPeripheralData(const QByteArray &data)
{
    m_advertiser->sendData(data);
}

// ---- GATT 连接 ----

void BleManager::connectToDevice(const QString &address)
{
    const QBluetoothDeviceInfo info = m_scanner->findDevice(address);
    if (!info.isValid()) {
        emit errorOccurred(QStringLiteral("找不到设备: %1（请先扫描）")
                               .arg(address));
        return;
    }
    m_connection->connectToDevice(info);
}

void BleManager::disconnectFromDevice()
{
    m_connection->disconnectFromDevice();
}

void BleManager::discoverServices()
{
    m_connection->discoverServices();
}

void BleManager::discoverDetails(const QString &serviceUuid)
{
    m_connection->discoverDetails(serviceUuid);
}

QVariantList BleManager::services() const
{
    return m_connection->services();
}

QVariantList BleManager::characteristics(const QString &serviceUuid) const
{
    return m_connection->characteristics(serviceUuid);
}

void BleManager::readCharacteristic(const QString &serviceUuid,
                                    const QString &charUuid)
{
    m_connection->readCharacteristic(serviceUuid, charUuid);
}

void BleManager::writeCharacteristic(const QString &serviceUuid,
                                     const QString &charUuid,
                                     const QByteArray &data)
{
    m_connection->writeCharacteristic(serviceUuid, charUuid, data);
}

void BleManager::enableNotify(const QString &serviceUuid,
                              const QString &charUuid,
                              bool enable)
{
    m_connection->enableNotify(serviceUuid, charUuid, enable);
}
