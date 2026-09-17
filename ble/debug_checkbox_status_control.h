#pragma once

// =====================================================================
// debug_checkbox_status_control.h - 调试页 CheckBox 状态 & 已绑定设备
// 仓库（SQLite：debug_checkbox_status_master_db）
//
// 模块职责：
//   1) 持久化「调试终端页」上所有 CheckBox 的状态（除「定时发送」外）：
//      hex_send / hex_receive_spaced / hex_receive_no_space /
//      timestamp_enabled / bind_current_device
//   2) 维护「已绑定设备」列表（设备名 + MAC + 绑定时间）：
//      - 调试页"绑定此设备"勾选 → 调用 bindDevice(name, address)
//      - 绑定页"解除绑定"按钮 → 调用 unbindDevice(address)
//      - 扫描时如果发现已绑定设备 → BleScanner 自动连接
//      - 上一次扫描后绑定信息丢失也不影响已存绑定
//
// 存储结构（一个数据库文件，两个表）：
//   Table A: debug_checkbox_status
//     key   TEXT PRIMARY KEY   -- 状态项名称（见 Keys 常量）
//     value TEXT NOT NULL      -- 状态值（bool 转 "0"/"1"）
//   Table B: bound_devices
//     address     TEXT PRIMARY KEY   -- MAC 地址（去重 & 自动连接依据）
//     name        TEXT NOT NULL      -- 最后一次绑定时的广播名
//     bound_time  INTEGER NOT NULL   -- 绑定时间戳（毫秒，便于排序展示）
//
// 设计要点：
//   - 数据库文件存放在 SqliteWarehouse 的同目录（warehouseDir）下，
//     桌面与 Android 都自动落到合适的可写位置，无需额外处理；
//   - 所有写入走 SQL 参数绑定，无注入风险；
//   - 内部使用独立连接名（"debug_checkbox_status_control"），与
//     SqliteWarehouse 的连接互不干扰；
//   - QML 通过 bleManager.checkboxControl 访问本模块。
// =====================================================================

#include <QObject>
#include <QString>
#include <QVariantList>

class QSqlDatabase;

class DebugCheckboxStatusControl : public QObject
{
    Q_OBJECT
    // 暴露给 QML 的便捷属性：每个属性 getter 读数据库，setter 写数据库，
    // 同步发出对应 changed 信号让 UI 自动刷新
    Q_PROPERTY(bool hexSend READ hexSend WRITE setHexSend NOTIFY hexSendChanged)
    Q_PROPERTY(bool hexReceiveSpaced READ hexReceiveSpaced
               WRITE setHexReceiveSpaced NOTIFY hexReceiveSpacedChanged)
    Q_PROPERTY(bool hexReceiveNoSpace READ hexReceiveNoSpace
               WRITE setHexReceiveNoSpace NOTIFY hexReceiveNoSpaceChanged)
    Q_PROPERTY(bool timestampEnabled READ timestampEnabled
               WRITE setTimestampEnabled NOTIFY timestampEnabledChanged)
    Q_PROPERTY(bool bindCurrentDevice READ bindCurrentDevice
               WRITE setBindCurrentDevice NOTIFY bindCurrentDeviceChanged)
    // 已绑定设备数量（用于 UI 显示 / 角标）
    Q_PROPERTY(int boundDeviceCount READ boundDeviceCount
               NOTIFY boundDevicesChanged)

public:
    explicit DebugCheckboxStatusControl(QObject *parent = nullptr);
    ~DebugCheckboxStatusControl() override;

    // ---- 初始化 ----
    // 启动时调用一次：确保数据库文件、表结构存在；如首次启动则插入默认值。
    // 重复调用幂等：表已存在则直接复用。
    Q_INVOKABLE void initialize();

    // ---- CheckBox 状态便捷属性（封装通用 getStatus/setStatus）----
    bool hexSend() const             { return getBoolValue(KHexSend, false); }
    void setHexSend(bool v);
    bool hexReceiveSpaced() const    { return getBoolValue(KHexReceiveSpaced, false); }
    void setHexReceiveSpaced(bool v);
    bool hexReceiveNoSpace() const   { return getBoolValue(KHexReceiveNoSpace, false); }
    void setHexReceiveNoSpace(bool v);
    bool timestampEnabled() const    { return getBoolValue(KTimestampEnabled, false); }
    void setTimestampEnabled(bool v);
    bool bindCurrentDevice() const   { return getBoolValue(KBindCurrentDevice, false); }
    void setBindCurrentDevice(bool v);

    // ---- 已绑定设备列表 ----
    // 返回 [{name, address, boundTime}, ...]，按绑定时间倒序
    Q_INVOKABLE QVariantList boundDevices() const;
    int boundDeviceCount() const     { return boundDevices().size(); }

    // ---- 绑定 / 解绑 ----
    // 新增或更新一条绑定（同地址已存在则刷新 name 与 bound_time）
    Q_INVOKABLE bool bindDevice(const QString &name, const QString &address);
    // 按地址删除绑定；不存在也返回 true（幂等）
    Q_INVOKABLE bool unbindDevice(const QString &address);
    // 判断某地址是否已绑定（用于 BleScanner 自动连接判定）
    Q_INVOKABLE bool isBound(const QString &address) const;
    // 取所有已绑定地址（小写归一化），供 BleScanner 注入自动连接候选集
    Q_INVOKABLE QStringList boundAddresses() const;

signals:
    void hexSendChanged();
    void hexReceiveSpacedChanged();
    void hexReceiveNoSpaceChanged();
    void timestampEnabledChanged();
    void bindCurrentDeviceChanged();
    // 绑定列表变化时发出（BindPage / BleScanner / 角标均可订阅）
    void boundDevicesChanged();
    // 数据库读写失败（QML 可选择 toast 提示）
    void errorOccurred(const QString &message);

private:
    // CheckBox 状态键名（数据库 Table A 主键），统一在此集中定义，
    // 避免散落字符串拼写错误
    static inline const char *const KHexSend            = "hex_send";
    static inline const char *const KHexReceiveSpaced   = "hex_receive_spaced";
    static inline const char *const KHexReceiveNoSpace  = "hex_receive_no_space";
    static inline const char *const KTimestampEnabled   = "timestamp_enabled";
    static inline const char *const KBindCurrentDevice  = "bind_current_device";

    // 数据库 / 连接名常量
    static inline const char *const kDbName       = "debug_checkbox_status_master_db";
    static inline const char *const kConnName     = "debug_checkbox_status_control";
    static inline const char *const kCheckboxTbl  = "debug_checkbox_status";
    static inline const char *const kBoundTbl     = "bound_devices";

    // ---- 数据库操作 ----
    bool openDatabase();                 // 打开/创建 .db + 建表（幂等）
    void closeDatabase();                // 关闭并清连接缓存（析构时调用）
    bool ensureCheckboxTable();          // 建 Table A（debug_checkbox_status）
    bool ensureBoundDevicesTable();      // 建 Table B（bound_devices）
    // 通用读写：key → bool；找不到时返回 fallback
    bool getBoolValue(const char *key, bool fallback) const;
    // 通用写入：key → "0"/"1"；成功返回 true
    bool setBoolValue(const char *key, bool v);

    // 小写归一化地址（Android 上有时会大写，统一小写避免重复绑定）
    static QString normalizeAddress(const QString &address);

    // 当前打开的数据库连接（首次 initialize 后有效）
    QSqlDatabase *m_db = nullptr;
    bool m_initialized = false;
};
