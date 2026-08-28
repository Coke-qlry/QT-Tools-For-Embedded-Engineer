#include "blepermissions.h"

#include <QBluetoothPermission>
#include <QCoreApplication>
#include <QPermission>

namespace {

// 权限类别的友好名称，用于提示文案
const char *permissionLabel(BlePermissions::Permission permission)
{
    switch (permission) {
    case BlePermissions::ScanPermission:      return "扫描";
    case BlePermissions::ConnectPermission:   return "连接";
    case BlePermissions::AdvertisePermission: return "广播";
    }
    return "蓝牙";
}

} // namespace

BlePermissions::BlePermissions(QObject *parent)
    : QObject(parent)
{
}

QBluetoothPermission
BlePermissions::bluetoothPermission(Permission permission) const
{
    QBluetoothPermission perm;
    switch (permission) {
    case ScanPermission:
    case ConnectPermission:
        // 扫描与 GATT 连接均依赖“访问”模式（Android 自动映射为
        // BLUETOOTH_SCAN + BLUETOOTH_CONNECT，旧版映射为定位权限）
        perm.setCommunicationModes(QBluetoothPermission::Access);
        break;
    case AdvertisePermission:
        // 广播：使用“访问 + 广播”组合模式（等价于 QBluetoothPermission::Default）。
        // 关键原因：
        //   1. QtBluetooth 底层（QLowEnergyController::createPeripheral /
        //      startAdvertising）内部正是以 Default(组合) 模式请求蓝牙权限，
        //      单独请求 Advertise 会导致 checkPermission/requestPermission
        //      与实际 QtBluetooth 状态不一致，出现“扫描有权限、广播没有”的假象。
        //   2. Android 11 及以下任何模式都要求完整蓝牙权限 + 定位权限，
        //      组合模式可一次性覆盖旧版系统。
        //   3. Android 12+：组合模式会请求 BLUETOOTH_SCAN + BLUETOOTH_CONNECT
        //      + BLUETOOTH_ADVERTISE，其中 ADVERTISE 正是广播所需运行时权限。
        perm.setCommunicationModes(QBluetoothPermission::Access
                                   | QBluetoothPermission::Advertise);
        break;
    }
    return perm;
}

bool BlePermissions::hasPermission(Permission permission) const
{
    return qApp->checkPermission(bluetoothPermission(permission))
        == Qt::PermissionStatus::Granted;
}

void BlePermissions::requestPermission(Permission permission)
{
    QBluetoothPermission perm = bluetoothPermission(permission);

    // 已授权：直接成功
    if (qApp->checkPermission(perm) == Qt::PermissionStatus::Granted) {
        emitResult(permission, true);
        return;
    }

    // 未授权（Undetermined / Denied）：一律再次发起系统授权请求。
    // 注意：Android 12+ 的运行时权限（含广播所需的 BLUETOOTH_ADVERTISE）
    // 必须通过 requestPermission 弹窗授予。若用户此前拒绝过，再次点击
    // 「开始广播」时仍需触发授权流程（不能因曾 Denied 而永久跳过），
    // 否则用户会误以为“广播权限没有给到”。
    // （若用户勾选“不再询问”，requestPermission 会立即返回 Denied，
    //  此时应引导其到系统设置手动开启 —— 见 permissionDenied 信号。）
    qApp->requestPermission(
        perm, this,
        [this, permission](const QPermission &result) {
            const bool granted =
                (result.status() == Qt::PermissionStatus::Granted);
            emitResult(permission, granted);
        });
}

QString BlePermissions::pendingMessage() const
{
    if (!hasPermission(ScanPermission))
        return QStringLiteral("需要扫描权限才能搜索附近的 BLE 设备");
    if (!hasPermission(ConnectPermission))
        return QStringLiteral("需要连接权限才能连接 BLE 设备");
    if (!hasPermission(AdvertisePermission))
        return QStringLiteral("需要广播权限才能作为外设广播");
    return QString();
}

void BlePermissions::emitResult(Permission permission, bool granted)
{
    emit permissionChanged(permission, granted);
    if (granted) {
        emit permissionGranted(permission);
    } else {
        emit permissionDenied(
            permission,
            QStringLiteral("%1权限被拒绝，无法执行该蓝牙操作")
                .arg(QLatin1String(permissionLabel(permission))));
    }
}
