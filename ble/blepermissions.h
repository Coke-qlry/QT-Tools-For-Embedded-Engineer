#pragma once

// =====================================================================
// blepermissions.h - BLE 运行时权限管理
// 统一封装各平台（重点 Android）扫描 / 连接 / 广播所需的运行时权限，
// 供 QML 在发起操作前检查并引导用户授权。
//
// 底层使用 Qt 6.5+ 的 QBluetoothPermission（Qt Application Permissions），
// Qt 会自动处理 Android 各 API 级别的差异：
//   - Android 12+ (API 31)：映射 BLUETOOTH_SCAN / BLUETOOTH_CONNECT / BLUETOOTH_ADVERTISE
//   - Android 11 及以下：映射 BLUETOOTH + ACCESS_FINE_LOCATION
// 非 Android 平台视为始终已授权。
// =====================================================================

#include <QObject>
#include <QBluetoothPermission>

class BlePermissions : public QObject
{
    Q_OBJECT

public:
    // 权限类别：与 UI 操作对应，便于按需请求、避免一次性索要过多权限
    enum Permission {
        ScanPermission,      // 扫描 BLE
        ConnectPermission,   // GATT 连接
        AdvertisePermission  // 广播
    };
    Q_ENUM(Permission)

    explicit BlePermissions(QObject *parent = nullptr);

    // 是否已获得指定权限（未授权返回 false，调用方据此引导授权）
    Q_INVOKABLE bool hasPermission(Permission permission) const;

    // 请求指定权限；授权结果通过 permissionGranted / permissionDenied 信号异步返回
    Q_INVOKABLE void requestPermission(Permission permission);

    // 尚未授权的权限对应的提示文案（用于 UI 显示授权引导），已全部授权则返回空
    Q_INVOKABLE QString pendingMessage() const;

signals:
    void permissionGranted(BlePermissions::Permission permission);
    void permissionDenied(BlePermissions::Permission permission,
                          const QString &message);
    void permissionChanged(BlePermissions::Permission permission,
                           bool granted);

private:
    // 将权限类别映射为 QBluetoothPermission 对应的通信模式
    QBluetoothPermission bluetoothPermission(Permission permission) const;
    void emitResult(Permission permission, bool granted);
};
