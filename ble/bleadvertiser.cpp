#include "bleadvertiser.h"

#ifdef BLE_ADVERTISING_SUPPORTED
// Qt 6.10：外设广播通过 QLowEnergyController 的 Peripheral 角色实现。
// QLowEnergyAdvertisingManager 类在 Qt 6.10 中已移除。
#include <QLowEnergyController>
#include <QLowEnergyAdvertisingData>
#include <QLowEnergyAdvertisingParameters>
#include <QBluetoothUuid>
#include <QtGlobal>
#endif

BleAdvertiser::BleAdvertiser(QObject *parent)
    : QObject(parent)
{
#ifdef BLE_ADVERTISING_SUPPORTED
    // 创建外设（Peripheral）控制器，用于 BLE 广播
    m_controller = QLowEnergyController::createPeripheral(this);

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

    // 广播/控制器错误
    connect(m_controller, &QLowEnergyController::errorOccurred,
            this, [this](QLowEnergyController::Error error) {
                if (error == QLowEnergyController::NoError)
                    return;
                emit errorOccurred(
                    QStringLiteral("BLE 外设错误: %1")
                        .arg(m_controller->errorString()));
            });
#endif
}

bool BleAdvertiser::isAdvertising() const
{
    return m_advertising;
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

void BleAdvertiser::startAdvertise(const QString &localName,
                                   const QString &serviceUuid,
                                   int intervalMs)
{
#ifdef BLE_ADVERTISING_SUPPORTED
    if (!m_controller) {
        emit errorOccurred(QStringLiteral("无法创建 BLE 外设控制器"));
        return;
    }
    if (m_controller->state() == QLowEnergyController::AdvertisingState)
        m_controller->stopAdvertising();

    // ---- 广播数据 ----
    QLowEnergyAdvertisingData data;
    data.setDiscoverability(
        QLowEnergyAdvertisingData::DiscoverabilityGeneral);
    if (!localName.isEmpty())
        data.setLocalName(localName);
    data.setIncludePowerLevel(true);
    if (!serviceUuid.isEmpty()) {
        const QBluetoothUuid uuid = QBluetoothUuid::fromString(serviceUuid);
        if (!uuid.isNull())
            data.setServices({uuid});
    }

    // ---- 广播参数 ----
    // setMode 取值：AdvInd(可连接)/AdvScanInd(可扫描)/AdvNonConnInd(不可连接)。
    // SAR 外设仅上报数据、无需被连接，使用不可连接广播 AdvNonConnInd。
    QLowEnergyAdvertisingParameters parameters;
    parameters.setMode(QLowEnergyAdvertisingParameters::AdvNonConnInd);
    // setInterval(minimum, maximum) 单位 0.625ms，合法范围 20ms ~ 10240ms。
    // 为简化，最小/最大间隔设为相同值。
    const int clamped = qBound(20, intervalMs > 0 ? intervalMs : 100, 10240);
    const quint16 ticks = static_cast<quint16>(clamped * 8 / 5);
    parameters.setInterval(ticks, ticks);

    m_controller->startAdvertising(parameters, data,
                                   QLowEnergyAdvertisingData());
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
