#pragma once

// =====================================================================
// bleterminal.h - 调试终端模块（Debug Terminal）
// 供「调试终端」页使用，提供 BLE 数据收发、十六进制显示/发送、
// 定时发送、接收数据导出等能力。
// 作为 BleManager 的子模块持有（与 BlePermissions 同模式），
// QML 通过 bleManager.terminal 访问。
//
// 角色说明：
//   - 作为 Central（主动连接从设备）：连接后自动发现“所有服务/特征”，
//     界面可选择写入(发送)特征、主动读取特征、开启/关闭指定特征通知，
//     实现真正的双向收发；
//   - 作为 Peripheral（广播被其它主设备连接）：直接调用
//     BleManager::sendPeripheralData / 接收 peripheralDataReceived。
//
// 导出目录：
//   - 桌面平台：QML 使用系统 FolderDialog 选择目录后调用 setExportTarget()；
//   - Android：调用 pickFolder() 弹出系统文件夹选择器(SAF)，选中目录后
//     保存为 content:// 树 URI，可长期授权并在重启后继续写入。
// =====================================================================

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class BleManager;
class QTimer;

class BleTerminal : public QObject
{
    Q_OBJECT
    // 接收区显示文本（原文/十六进制由 hexReceive 开关决定）
    Q_PROPERTY(QString receivedText READ receivedText NOTIFY receivedTextChanged)
    // 会话是否可用（主动连接从设备 / 外设被其它设备连接）
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // 十六进制接收 / 发送开关
    Q_PROPERTY(bool hexReceive READ hexReceive WRITE setHexReceive
               NOTIFY hexReceiveChanged)
    Q_PROPERTY(bool hexSend READ hexSend WRITE setHexSend
               NOTIFY hexSendChanged)
    // 定时发送状态（开启后由 C++ 定时器驱动，持续到点自动关闭）
    Q_PROPERTY(bool timerEnabled READ timerEnabled WRITE setTimerEnabled
               NOTIFY timerEnabledChanged)
    // 时间戳开关：开启后接收区逐条显示收发记录，
    // 每条带 [hh:mm:ss.zzz] RX/TX 前缀（毫秒级）
    Q_PROPERTY(bool timestampEnabled READ timestampEnabled
               WRITE setTimestampEnabled NOTIFY timestampEnabledChanged)
    // 自定义导出目录：桌面为真实路径；Android SAF 为 content:// 树 URI
    Q_PROPERTY(QString exportTarget READ exportTarget WRITE setExportTarget
               NOTIFY exportTargetChanged)
    // 导出目录的人性化显示名（Android 上把 content URI 解析为可读名称）
    Q_PROPERTY(QString exportDisplay READ exportDisplay
               NOTIFY exportDisplayChanged)
    // 自动轮询读取开关（默认开启）：对“可读但未开通知”的特征周期性读取，
    // 使只支持 Read（没有 Notify）的数据特征也能自动收到并显示数据，
    // 无需用户手动去「服务与特征」里点“读取”。
    Q_PROPERTY(bool autoReadEnabled READ autoReadEnabled
               WRITE setAutoReadEnabled NOTIFY autoReadEnabledChanged)

public:
    explicit BleTerminal(BleManager *manager, QObject *parent = nullptr);

    QString receivedText() const { return m_receivedText; }
    bool active() const { return m_active; }
    bool hexReceive() const { return m_hexReceive; }
    void setHexReceive(bool on);
    bool hexSend() const { return m_hexSend; }
    void setHexSend(bool on);
    bool timerEnabled() const { return m_timerEnabled; }
    void setTimerEnabled(bool on);
    bool timestampEnabled() const { return m_timestampEnabled; }
    void setTimestampEnabled(bool on);
    QString exportTarget() const { return m_exportTarget; }
    void setExportTarget(const QString &target);
    QString exportDisplay() const { return m_exportDisplay; }
    bool autoReadEnabled() const { return m_autoReadEnabled; }
    void setAutoReadEnabled(bool on);

    // ---- 由 BleManager 转发调用 ----
    void setCentralConnected(bool connected);
    void setPeripheralConnected(bool connected);
    void onCentralData(const QString &serviceUuid, const QString &charUuid,
                       const QByteArray &data);
    void onPeripheralData(const QByteArray &data);

    // ---- QML 调用接口（数据收发 / 导出 / 定时发送）----
    Q_INVOKABLE void clearReceived();
    Q_INVOKABLE void sendText(const QString &text);
    // 强制按 UTF-8 文本发送（忽略 hexSend 开关）。供指令页等发送固定
    // ASCII 指令(如 AT+VBAT?)的场景使用，避免“十六进制发送”开关误伤。
    Q_INVOKABLE void sendAsciiText(const QString &text);
    // 导出接收数据为 txt 到当前自定义目录（exportTarget）。
    // 返回保存位置（路径或 content:// URI）；失败返回空字符串并发出 errorOccurred。
    Q_INVOKABLE QString saveReceived(const QString &fileName);
    // 启动定时发送：unit 时间单位(秒/毫秒/分)，interval 间隔数，duration 持续数
    // （<=0 表示一直发送）。payload 为本次要发送的内容。
    Q_INVOKABLE void startTimerSend(const QString &unit, double interval,
                                    double duration, const QString &payload);
    Q_INVOKABLE void stopTimerSend();

    // ---- QML 调用接口（服务 / 特征目录，Central 模式双向收发）----
    // 返回全部已发现特征（跨服务）的扁平目录，每项包含：
    //   serviceUuid / serviceName / charUuid / charName / properties /
    //   writable / readable / notifiable / notifyOn / isSendTarget
    Q_INVOKABLE QVariantList charCatalog() const;
    // 指定写入(发送)目标特征（须支持 Write/WriteNoResponse）
    Q_INVOKABLE void setSendTarget(const QString &serviceUuid,
                                   const QString &charUuid);
    // 主动读取指定特征（结果作为收到的数据显示在接收区）
    Q_INVOKABLE void readNow(const QString &serviceUuid,
                             const QString &charUuid);
    // 开启 / 关闭指定特征的通知（Notify/Indicate）
    Q_INVOKABLE void toggleNotify(const QString &serviceUuid,
                                  const QString &charUuid, bool enable);

    // ---- QML 调用接口（导出目录 + 空间检测）----
    // Android：弹出系统目录选择器(SAF)，选中后自动写入 exportTarget。
    // 非 Android 返回 false，由 QML 改用 FolderDialog。
    Q_INVOKABLE bool pickFolder();
    // 导出前空间检测：返回 { totalBytes, freeBytes, neededBytes, enough }
    // totalBytes/freeBytes 为目标目录所在卷的总/可用空间（Android 无真实
    // 路径时按外部共享存储统计）；neededBytes 为当前接收数据所需大小。
    Q_INVOKABLE QVariantMap storageInfo() const;

    // ---- 十六进制工具（可复用）----
    // 解析文本为字节流：忽略空格/逗号/分号等分隔符；
    // 含非法字符或字节数为奇数时返回空并置 ok=false。
    static QByteArray parseHex(const QString &text, bool *ok);
    // 字节流转十六进制显示串：大写、空格分隔
    static QString bytesToHex(const QByteArray &data);

signals:
    void receivedTextChanged();
    // 每追加一条收发记录行时发出：display 已按 hexReceive/hexSend 与时间戳
    // 选项格式化（与 receivedText 同源），receive=true 表示收到(RX)，
    // false 表示本机发送(TX)。供指令页等其它视图与调试文本框同步展示。
    void dataLogged(const QString &display, bool receive);
    void activeChanged();
    void hexReceiveChanged();
    void hexSendChanged();
    void timerEnabledChanged();
    void timestampEnabledChanged();
    void errorOccurred(const QString &message);
    // 服务/特征目录变化（连接、发现完成、目标/通知状态变更后发出）
    void catalogChanged();
    // Central 连接后服务/特征自动配置完成（自动发现、自动选发送目标、
    // 自动开启全部通知特征），通知界面无需用户手动操作即可收发。
    void autoConfigured(int serviceCount, int notifyEnabled);
    // 导出目录属性变化
    void exportTargetChanged();
    void exportDisplayChanged();
    // 自动轮询读取开关变化
    void autoReadEnabledChanged();
    // Android SAF 选择完成（target 为 content URI / 空表示用户取消）
    void folderPicked(const QString &target, const QString &display);

private slots:
    void onServicesDiscovered(const QVariantList &services);
    void onDetailsDiscovered(const QString &serviceUuid);
    // 通知使能最终结果：同步目录开关状态；失败时把该特征回退为轮询读取
    void onNotifyChanged(const QString &serviceUuid, const QString &charUuid,
                         bool enabled, bool success);
    // 自动轮询读取：对可读但无通知的特征定时读取
    void onPollTick();
    void onTimerTick();
    // Android 目录选择结果（从 Android 主线程队列投递到 Qt 线程）
    void onFolderPickedResult(const QString &treeUri);

private:
    void updateActive();
    // 自动配置完成：发出 autoConfigured（带看门狗兜底，避免某服务
    // 细节发现失败时永远不完成）
    void finishAutoConfig();
    // 轮询读取的启停
    void startPolling();
    void stopPolling();
    // 该特征是否需要走“自动轮询读取”（可读、无通知、非信息类服务）
    bool isPollCandidate(const QString &serviceUuid,
                         const QString &charUuid) const;
    void appendReceived(const QByteArray &raw);
    // 回显本机发送的字节（发送成功后在接收区显示）
    void appendSent(const QByteArray &bytes);
    // 追加一条日志：receive=true 为收到(RX)，false 为本机发送回显(TX)；
    // 时间戳开启时逐条以 [hh:mm:ss.zzz] RX/TX 前缀换行显示
    void appendLog(const QString &display, bool receive);
    void sendPayload(const QByteArray &bytes);
    // 目录展示名刷新（Android 解析 content URI；其余直接用路径）
    void updateExportDisplay();
    void persistExportTarget();
    // 从接收数据估算导出所需字节数
    qint64 neededBytes() const;
    static QString volumeProbePath();

    struct CharEntry {
        QString serviceUuid;
        QString serviceName;
        QString charUuid;
        QString charName;
        int properties = 0;
        bool notifyOn = false;   // 是否已使能通知
    };

    BleManager *m_manager = nullptr;
    QTimer *m_timer = nullptr;
    // 自动配置跟踪（本次连接的待发现服务数 / 已自动开启的通知计数）
    QTimer *m_configTimer = nullptr;
    int m_configPendingServices = 0;
    int m_configTotalServices = 0;
    int m_configNotifyCount = 0;
    bool m_configFinished = true;
    // 自动轮询读取：对可读但无通知的特征定时读取（默认开启）
    QTimer *m_pollTimer = nullptr;
    bool m_autoReadEnabled = true;
    // 轮询读取去重：每个特征最近一次读到的值（连续相同则不再重复显示）
    QHash<QString, QByteArray> m_pollLastValue;
    QString m_receivedText;
    // Central 模式下写入（发送）目标特征
    QString m_targetService;
    QString m_targetChar;
    // 最近一次本机写入内容（用于在 onCentralData 过滤"写后被轮询读回"的回显假象）
    QString m_lastWrittenService;
    QString m_lastWrittenChar;
    QByteArray m_lastWrittenPayload;
    bool m_defaultTxFound = false;   // 是否已自动选中首个可写特征
    bool m_central = false;
    bool m_peripheral = false;
    bool m_active = false;
    bool m_hexReceive = false;
    bool m_hexSend = false;
    bool m_timerEnabled = false;
    bool m_timestampEnabled = false;
    int m_timerIntervalMs = 1000;
    qint64 m_timerTotalMs = -1;      // 定时总时长，<0 表示一直发送
    qint64 m_timerElapsedMs = 0;
    QByteArray m_timerPayload;
    // 全部已发现特征目录（跨服务扁平存储）
    QList<CharEntry> m_catalog;
    // 自定义导出目录（路径 或 content:// URI）
    QString m_exportTarget;
    QString m_exportDisplay;
};
