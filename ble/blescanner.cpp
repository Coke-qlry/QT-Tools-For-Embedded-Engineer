#include "blescanner.h"

#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothUuid>
#include <QUuid>

namespace {

// 生成用于 UI 展示与去重的设备标识：
// 优先使用 LE 设备 UUID，回退到经典蓝牙 MAC（Qt 6.8+ 已弃用 address()，仅作兜底）
QString deviceAddressString(const QBluetoothDeviceInfo &info)
{
    const QBluetoothUuid uuid = info.deviceUuid();
    if (!uuid.isNull())
        return uuid.toString(QUuid::WithoutBraces);

    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    return info.address().toString();
    QT_WARNING_POP
}

bool isLeDevice(const QBluetoothDeviceInfo &info)
{
    return info.coreConfigurations()
        .testFlag(QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
}

// 设备展示名：没有广播名称 / 无法识别的设备，统一显示为 N/A，不留空白
QString displayName(const QBluetoothDeviceInfo &info)
{
    const QString name = info.name();
    return name.trimmed().isEmpty() ? QStringLiteral("N/A") : name;
}

} // namespace

BleScanner::BleScanner(QObject *parent)
    : QObject(parent)
    , m_agent(new QBluetoothDeviceDiscoveryAgent(this))
{
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
            this, &BleScanner::onDeviceDiscovered);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::finished,
            this, &BleScanner::onScanFinished);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::canceled,
            this, &BleScanner::onScanCanceled);
    connect(m_agent,
            QOverload<QBluetoothDeviceDiscoveryAgent::Error>::of(
                &QBluetoothDeviceDiscoveryAgent::errorOccurred),
            this, [this](QBluetoothDeviceDiscoveryAgent::Error) {
                emit errorOccurred(m_agent->errorString());
                setScanning(false);
            });
}

bool BleScanner::isScanning() const
{
    return m_scanning;
}

QList<BleScanner::DeviceEntry> BleScanner::devices() const
{
    QList<DeviceEntry> entries;
    entries.reserve(m_infos.size());
    for (const QBluetoothDeviceInfo &info : m_infos) {
        DeviceEntry e;
        e.name = displayName(info);
        e.address = deviceAddressString(info);
        e.rssi = info.rssi();
        e.isLe = isLeDevice(info);
        entries.append(e);
    }
    return entries;
}

QBluetoothDeviceInfo BleScanner::findDevice(const QString &address) const
{
    for (const QBluetoothDeviceInfo &info : m_infos) {
        if (deviceAddressString(info) == address)
            return info;
    }
    return QBluetoothDeviceInfo();
}

void BleScanner::startScan(int timeoutMs)
{
    if (m_scanning)
        return;

    // 开始新一轮扫描前先清空上一轮的发现缓存：
    // 否则先前发现过的设备会因地址去重被直接忽略、不再上报，
    // 导致新扫描只能看到"本轮新增"的设备。
    clear();

    // 重置本轮扫描内的"已自动连接"去重集合：每一轮扫描允许再次触发
    // 自动连接（用户可能切换设备或暂时离开后又回来），但同一轮内
    // 同一地址只连一次，避免重复骚扰。
    m_autoConnectedThisScan.clear();

    if (timeoutMs > 0)
        m_agent->setLowEnergyDiscoveryTimeout(timeoutMs);

    m_agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
    setScanning(true);
}

void BleScanner::stopScan()
{
    if (m_agent->isActive())
        m_agent->stop();
    setScanning(false);
}

void BleScanner::clear()
{
    m_infos.clear();
    m_addresses.clear();
}

void BleScanner::onDeviceDiscovered(const QBluetoothDeviceInfo &info)
{
    const QString address = deviceAddressString(info);
    if (address.isEmpty())
        return;

    // 去重（哈希表 O(1)）：同一设备地址只保留首次记录
    if (m_addresses.contains(address))
        return;
    m_addresses.insert(address);

    m_infos.append(info);
    const QString name = displayName(info);
    emit deviceFound(name, address, info.rssi(), isLeDevice(info));

    // 命中已绑定列表：本轮扫描内仅触发一次自动连接
    const QString addrLower = address.toLower();
    if (!m_boundAddresses.isEmpty()
        && m_boundAddresses.contains(addrLower)
        && !m_autoConnectedThisScan.contains(addrLower)) {
        m_autoConnectedThisScan.insert(addrLower);
        emit boundDeviceDiscovered(name, address);
    }
}

void BleScanner::setBoundAddresses(const QStringList &addresses)
{
    // 小写归一化后再写入，与 boundDeviceDiscovered 触发处的处理保持一致
    m_boundAddresses.clear();
    for (const QString &a : addresses) {
        const QString k = a.trimmed().toLower();
        if (!k.isEmpty()) m_boundAddresses.insert(k);
    }
}

void BleScanner::onScanFinished()
{
    setScanning(false);
    emit scanFinished();
}

void BleScanner::onScanCanceled()
{
    setScanning(false);
    emit scanFinished();
}

void BleScanner::setScanning(bool scanning)
{
    if (m_scanning == scanning)
        return;
    m_scanning = scanning;
    emit scanningChanged(scanning);
}
