#include "bleadvertiser.h"

#ifdef BLE_ADVERTISING_SUPPORTED
// Qt 6.10：外设广播通过 QLowEnergyController 的 Peripheral 角色实现。
// QLowEnergyAdvertisingManager 类在 Qt 6.10 中已移除。
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QLowEnergyServiceData>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyCharacteristicData>
#include <QLowEnergyDescriptor>
#include <QLowEnergyDescriptorData>
#include <QLowEnergyAdvertisingData>
#include <QLowEnergyAdvertisingParameters>
#include <QBluetoothUuid>
#include <QString>
#include <QByteArray>
#include <QtGlobal>
#endif

// ---------------------------------------------------------------------
// 将广播名称按 UTF-8 字节安全截断到 BLE Local Name 字段上限（29 字节）。
// BLE 广播报文一个 AD Structure 最多 31 字节，Local Name 字段本身上限为
// 29 字节（31 - 2 字节的长度/类型头）。超长时按 UTF-8 边界截断，避免把
// 多字节字符（如中文）从中切开形成乱码。
// ---------------------------------------------------------------------
static QString truncateLocalName(const QString &name)
{
    QByteArray utf8 = name.toUtf8();
    if (utf8.size() <= 29)
        return name;

    // 从第 29 字节处向左回退，直到落在合法的 UTF-8 起始字节上
    int cut = 29;
    while (cut > 0 && (static_cast<unsigned char>(utf8.at(cut)) & 0xC0) == 0x80)
        --cut;          // 跳过续字节
    utf8.truncate(cut);
    return QString::fromUtf8(utf8);
}

BleAdvertiser::BleAdvertiser(QObject *parent)
    : QObject(parent)
{
#ifdef BLE_ADVERTISING_SUPPORTED
    // 创建外设（Peripheral）控制器，用于 BLE 广播与对外连接
    m_controller = QLowEnergyController::createPeripheral(this);
    if (m_controller)
        setupControllerConnections();
#endif
}

#ifdef BLE_ADVERTISING_SUPPORTED
void BleAdvertiser::setupControllerConnections()
{
    // 广播状态变化：AdvertisingState 表示广播中
    connect(m_controller, &QLowEnergyController::stateChanged,
            this, [this](QLowEnergyController::ControllerState state) {
                const bool advertising =
                    (state == QLowEnergyController::AdvertisingState);
                if (advertising != m_advertising) {
                    m_advertising = advertising;
                    emit advertisingChanged(advertising);
                }
            });

    // 外设被连接 / 断开
    connect(m_controller, &QLowEnergyController::connected,
            this, [this]() {
                if (!m_connected) {
                    m_connected = true;
                    emit connectedChanged(true);
                }
            });
    connect(m_controller, &QLowEnergyController::disconnected,
            this, [this]() {
                if (m_connected) {
                    m_connected = false;
                    emit connectedChanged(false);
                }
            });

    // 广播/控制器错误
    connect(m_controller, &QLowEnergyController::errorOccurred,
            this, [this](QLowEnergyController::Error error) {
                if (error == QLowEnergyController::NoError)
                    return;
                emit errorOccurred(
                    QStringLiteral("BLE 外设错误: %1")
                        .arg(m_controller ? m_controller->errorString()
                                          : QStringLiteral("未知错误")));
            });
}
#endif

BleAdvertiser::~BleAdvertiser()
{
    stopAdvertise();
}

bool BleAdvertiser::isAdvertising() const
{
    return m_advertising;
}

bool BleAdvertiser::isConnected() const
{
    return m_connected;
}

bool BleAdvertiser::isSupported() const
{
#ifdef BLE_ADVERTISING_SUPPORTED
    // 外设控制器创建成功即视为支持（具体可用性以运行时 startAdvertising 为准）
    return m_controller != nullptr;
#else
    return false;
#endif
}

#ifdef BLE_ADVERTISING_SUPPORTED
void BleAdvertiser::setupGattService(const QBluetoothUuid &serviceUuid)
{
    // ---- 构建 SAR 数据服务 ----
    // 含一个特征（Characteristic），支持：读（Central 主动读取）、
    // 写（Central 下发控制指令）、通知（外设主动上报 SAR 数据）。
    // 特征 UUID：0x2A46（Generic Data）
    const QBluetoothUuid charUuid(
        QStringLiteral("00002a46-0000-1000-8000-00805f9b34fb"));

    QLowEnergyCharacteristicData charData;
    charData.setUuid(charUuid);
    charData.setProperties(QLowEnergyCharacteristic::Read
                           | QLowEnergyCharacteristic::Write
                           | QLowEnergyCharacteristic::Notify);
    charData.setValue(QByteArray("BLE SAR")); // 初始可读值

    // 使能“通知”所需的 CCCD（Client Characteristic Configuration Descriptor）
    // UUID：0x2902
    QLowEnergyDescriptorData cccd(
        QBluetoothUuid(QStringLiteral(
            "00002902-0000-1000-8000-00805f9b34fb")),
        QByteArray(2, 0));
    charData.addDescriptor(cccd);

    QLowEnergyServiceData serviceData;
    serviceData.setType(QLowEnergyServiceData::ServiceTypePrimary);
    serviceData.setUuid(serviceUuid);
    serviceData.addCharacteristic(charData);

    m_sarService = m_controller->addService(serviceData);
    if (!m_sarService)
        return;

    // 记录可操作特征句柄
    const QList<QLowEnergyCharacteristic> chars =
        m_sarService->characteristics();
    if (!chars.isEmpty())
        m_sarChar = chars.first();

    // Central 写入特征（外设端收到写请求）
    connect(m_sarService, &QLowEnergyService::characteristicWritten,
            this, [this, charUuid](const QLowEnergyCharacteristic &info,
                                   const QByteArray &value) {
                if (info.isValid() && info.uuid() == charUuid)
                    emit dataReceived(value);
            });
}
#endif

void BleAdvertiser::startAdvertise(const QString &localName,
                                   const QString &serviceUuid,
                                   int intervalMs)
{
#ifdef BLE_ADVERTISING_SUPPORTED
    if (!m_controller) {
        emit errorOccurred(QStringLiteral("无法创建 BLE 外设控制器"));
        return;
    }

    // 解析服务 UUID（无效则回退 Generic Data 0x180C 兜底）
    QBluetoothUuid uuid;
    if (!serviceUuid.isEmpty()) {
        uuid = QBluetoothUuid::fromString(serviceUuid);
        if (uuid.isNull())
            uuid = QBluetoothUuid(
                QStringLiteral("0000180c-0000-1000-8000-00805f9b34fb"));
    } else {
        uuid = QBluetoothUuid(
            QStringLiteral("0000180c-0000-1000-8000-00805f9b34fb"));
    }

    // 已在广播则先停止（广播名称 / 参数可随时更新）
    if (m_controller->state() == QLowEnergyController::AdvertisingState)
        m_controller->stopAdvertising();

    // ---- 1. 建立真实可连接的 GATT 服务层（必须先于广播） ----
    // 关键：BLE 外设若要被扫描到并“可连接”，必须在 startAdvertising()
    // 之前 addService() 添加至少一个有效的 GATT 服务。没有服务层时，
    // Android 系统可能无法正常进入广播状态，或广播但不携带服务信息，
    // 这正是“其他设备扫不到本设备”的常见根因之一。
    //
    // 注意：QLowEnergyController 不提供移除已添加服务的接口，若服务 UUID
    // 变化时仍重复 addService()，会堆积多个服务对象造成资源泄漏。
    // 因此仅在 UUID 变化时重建整个控制器（服务对象随控制器一起销毁）。
    if (!m_sarService || m_serviceUuid != uuid) {
        if (m_controller->state() == QLowEnergyController::ConnectedState)
            m_controller->disconnectFromDevice();
        m_controller->deleteLater();
        m_controller = QLowEnergyController::createPeripheral(this);
        if (!m_controller) {
            m_sarService = nullptr;
            m_serviceUuid = QBluetoothUuid();
            m_advertising = false;
            m_connected = false;
            emit errorOccurred(QStringLiteral("无法重新创建 BLE 外设控制器"));
            return;
        }
        setupControllerConnections();
        m_sarService = nullptr;
        m_serviceUuid = uuid;
        m_advertising = false;
        m_connected = false;
        setupGattService(uuid);
        if (!m_sarService) {
            emit errorOccurred(QStringLiteral("创建 GATT 服务失败"));
            return;
        }
    }

    // ---- 2. 广播名称（真实广播名称 / Complete Local Name）----
    // 广播名称是可修改的：UI 传入的 localName 会作为实际广播出去的设备名称，
    // 扫描端（如 nRF Connect）看到的 "Complete Local Name" 即为此值。
    // 名称为空时回退到默认名，保证扫描端始终能识别到名称。
    const QString name = localName.trimmed().isEmpty()
                             ? QStringLiteral("BLE_SAR")
                             : localName.trimmed();
    const QString safeName = truncateLocalName(name);
    if (safeName != name) {
        emit errorOccurred(
            QStringLiteral("广播名称过长（%1 字节 > 29 字节），"
                           "已自动截断为“%2”")
                .arg(name.toUtf8().size()).arg(safeName));
    }

    // ---- 3. 主广播包数据 ----
    // 主广播包仅有 31 字节，优先放入服务 UUID 与标志位（保证可被识别/连接）。
    // 128-bit UUID 时剩余空间有限，名称放不下的部分交给扫描响应包承载。
    QLowEnergyAdvertisingData data;
    data.setDiscoverability(
        QLowEnergyAdvertisingData::DiscoverabilityGeneral);
    data.setIncludePowerLevel(true);
    data.setServices({uuid});

    // 若主广播包剩余空间足够，也把名称一并放入（如 16-bit UUID 场景）；
    // 空间不足时名称仅存在于扫描响应，扫描端合并后仍显示完整名称。
    // 31 = Flags(3) + TxPower(3) + UUID AD(2+len) + Name AD(2+len)
    const int uuidAdBytes = 2 + static_cast<int>(uuid.minimumSize());
    const int nameCapacity = 31 - 3 - 3 - uuidAdBytes - 2;
    if (nameCapacity >= safeName.toUtf8().size())
        data.setLocalName(safeName);

    // ---- 4. 扫描响应数据：完整广播名称 ----
    // 与 nRF Connect 广播配置一致：名称作为 "Complete Local Name" 广播，
    // 扫描端合并主广播包 + 扫描响应后，显示的名称即为我们设置的广播名称。
    QLowEnergyAdvertisingData scanResponseData;
    scanResponseData.setLocalName(safeName);

    // ---- 5. 广播参数 ----
    // 使用 AdvInd（可连接广播）：这是 createPeripheral() 的典型场景，
    // 允许 Central 设备扫描到并连接我们的外设。AdvNonConnInd（不可连接）
    // 在 Android 上支持有限且部分扫描器读不到名称，故不使用。
    QLowEnergyAdvertisingParameters parameters;
    parameters.setMode(QLowEnergyAdvertisingParameters::AdvInd);
    // setInterval(minimum, maximum) 单位 0.625ms，合法范围 20ms ~ 10240ms。
    const int clamped = qBound(20, intervalMs > 0 ? intervalMs : 100, 10240);
    const quint16 ticks = static_cast<quint16>(clamped * 8 / 5);
    parameters.setInterval(ticks, ticks);

    m_controller->startAdvertising(parameters, data, scanResponseData);
#else
    Q_UNUSED(localName)
    Q_UNUSED(serviceUuid)
    Q_UNUSED(intervalMs)
    emit errorOccurred(
        QStringLiteral("当前平台不支持 BLE 广播（Windows 桌面版受限，"
                       "请使用 Android 目标）"));
#endif
}

void BleAdvertiser::stopAdvertise()
{
#ifdef BLE_ADVERTISING_SUPPORTED
    if (m_controller
        && m_controller->state() == QLowEnergyController::AdvertisingState) {
        m_controller->stopAdvertising();
    }
#else
    if (m_advertising) {
        m_advertising = false;
        emit advertisingChanged(false);
    }
#endif
}

void BleAdvertiser::sendData(const QByteArray &data)
{
#ifdef BLE_ADVERTISING_SUPPORTED
    // 仅当有 Central 设备连接且特征有效时下发
    if (!m_connected || !m_sarChar.isValid() || !m_sarService)
        return;
    m_sarService->writeCharacteristic(m_sarChar, data);
#else
    Q_UNUSED(data)
#endif
}
