#include "bleterminal.h"

#include "blemanager.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLowEnergyCharacteristic>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_ANDROID
// QAndroidIntent / QAndroidActivityResultReceiver / QtAndroidPrivate::startActivity
// 均来自 QtCore 私有头（Qt6 中必须链接 Qt6::CorePrivate）
#include <QtCore/private/qandroidextras_p.h>
#include <QJniEnvironment>
#include <QJniObject>
#endif

// [BLE-DBG] 调试日志总开关：置 1 恢复输出，置 0 静默（等效注释全部调试打印）。
// 排查蓝牙连接 / 收发问题时改成 1 重新编译即可重新看到日志。
#define BLE_DBG_ENABLED 0
#if BLE_DBG_ENABLED
#  define BLE_DBG_LOG() qDebug()
#else
#  define BLE_DBG_LOG() QNoDebug()
#endif

namespace {

#ifdef Q_OS_ANDROID
// Android 系统目录选择器(SAF, ACTION_OPEN_DOCUMENT_TREE)回调
constexpr int kPickFolderRequestCode = 0x5354; // "ST" (Storage Tree)

class FolderPickReceiver : public QAndroidActivityResultReceiver
{
public:
    BleTerminal *terminal = nullptr;

    void handleActivityResult(int receiverRequestCode, int resultCode,
                              const QJniObject &data) override
    {
        if (receiverRequestCode != kPickFolderRequestCode) {
            delete this;
            return;
        }
        QString treeUri;
        if (resultCode == -1 /* Activity.RESULT_OK */ && data.isValid()) {
            const QJniObject uri = data.callObjectMethod(
                QStringLiteral("getData").toLatin1().constData(),
                "()Landroid/net/Uri;");
            if (uri.isValid())
                treeUri = uri.toString();
        }
        // handleActivityResult 运行在 Android 主线程，投递回 Qt 线程处理
        if (terminal) {
            QMetaObject::invokeMethod(terminal, "onFolderPickedResult",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, treeUri));
        }
        delete this;
    }
};

// 便捷：调用 Java 桥的"字符串入参 -> 字符串返回"静态方法
// （带显式签名的 callStaticObjectMethod 为 C 变参版本，需传原始句柄）
QString androidCallStaticString(const char *className, const char *method,
                                const QString &arg)
{
    const QJniObject res = QJniObject::callStaticObjectMethod(
        className, method, "(Ljava/lang/String;)Ljava/lang/String;",
        QJniObject::fromString(arg).object<jstring>());
    if (!res.isValid())
        return QString();
    return res.toString();
}
#endif // Q_OS_ANDROID

} // namespace

BleTerminal::BleTerminal(BleManager *manager, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
{
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &BleTerminal::onTimerTick);

    // 自动配置看门狗：服务细节发现偶发失败/超时时也能完成配置流程。
    // 间隔需覆盖 CCCD 订阅写入的 8s 超时，避免订阅尚未完成就提前收尾，
    // 导致通知没开成功、只能手动点"读取"。
    m_configTimer = new QTimer(this);
    m_configTimer->setSingleShot(true);
    m_configTimer->setInterval(8000);
    connect(m_configTimer, &QTimer::timeout, this, &BleTerminal::finishAutoConfig);

    // 自动轮询读取定时器：对"可读但未开通知"的特征周期性读取。
    // 有些 BLE 设备的数据特征只有 Read（不支持 Notify），这种特征
    // 只能由主机主动读，不轮询的话数据永远不会到达界面。
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(250);
    connect(m_pollTimer, &QTimer::timeout, this, &BleTerminal::onPollTick);

    if (m_manager) {
        // Central 模式下监听服务/特征发现结果，自动配置写入目标与通知
        connect(m_manager, &BleManager::servicesDiscovered,
                this, &BleTerminal::onServicesDiscovered);
        connect(m_manager, &BleManager::detailsDiscovered,
                this, &BleTerminal::onDetailsDiscovered);
        // 通知使能最终结果：同步目录状态，失败时回退轮询读取
        connect(m_manager, &BleManager::notifyChanged,
                this, &BleTerminal::onNotifyChanged);
    }

    // 恢复上次选择的导出目录（Android 上 SAF 权限已持久化，重启仍可写入）
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("BLE_SAR"), QStringLiteral("BLE_SAR"));
    m_exportTarget = settings.value(QStringLiteral("terminal/exportTarget"))
                         .toString();
    updateExportDisplay();
}

// ---------------------------------------------------------------------
// 十六进制工具
// ---------------------------------------------------------------------
QByteArray BleTerminal::parseHex(const QString &text, bool *ok)
{
    if (ok)
        *ok = false;

    QString cleaned;
    cleaned.reserve(text.size());
    for (const QChar &ch : text) {
        if (ch.digitValue(16) >= 0) {           // 0-9 A-F a-f
            cleaned += ch;
        } else if (!(ch.isSpace()
                     || ch == QLatin1Char(',')
                     || ch == QLatin1Char(';')
                     || ch == QLatin1Char(':')
                     || ch == QLatin1Char('-')
                     || ch == QLatin1Char('_'))) {
            return QByteArray();                 // 非法字符
        }
    }
    if (cleaned.isEmpty() || cleaned.size() % 2 != 0)
        return QByteArray();

    QByteArray out(cleaned.size() / 2, Qt::Uninitialized);
    for (int i = 0; i < cleaned.size(); i += 2) {
        out[i / 2] = char((cleaned[i].digitValue(16) << 4)
                          | cleaned[i + 1].digitValue(16));
    }
    if (ok)
        *ok = true;
    return out;
}

QString BleTerminal::bytesToHex(const QByteArray &data)
{
    QString out;
    out.reserve(data.size() * 3);
    for (int i = 0; i < data.size(); ++i) {
        if (i > 0)
            out += QLatin1Char(' ');
        out += QStringLiteral("%1")
                   .arg(uchar(data.at(i)), 2, 16, QLatin1Char('0'))
                   .toUpper();
    }
    return out;
}

// ---------------------------------------------------------------------
// 连接状态
// ---------------------------------------------------------------------
void BleTerminal::setHexReceive(bool on)
{
    if (m_hexReceive != on) {
        m_hexReceive = on;
        emit hexReceiveChanged();
    }
}

void BleTerminal::setHexSend(bool on)
{
    if (m_hexSend != on) {
        m_hexSend = on;
        emit hexSendChanged();
    }
}

void BleTerminal::setTimestampEnabled(bool on)
{
    if (m_timestampEnabled != on) {
        m_timestampEnabled = on;
        emit timestampEnabledChanged();
    }
}

void BleTerminal::setCentralConnected(bool connected)
{
    if (m_central == connected)
        return;
    m_central = connected;
    stopPolling();
    m_pollLastValue.clear();
    m_lastWrittenPayload.clear();   // 连接断开/切换后不再过滤任何到达数据
    if (connected) {
        // 服务发现由 BleConnection 连接成功后自动发起，这里只监听其结果
        // （servicesDiscovered 触发自动发现特征详情/自动开通知的流程）。
        m_configPendingServices = 0;
        m_configTotalServices = 0;
        m_configNotifyCount = 0;
        m_configFinished = false;
    } else {
        // 断开：清空服务/特征目录与收发目标，终止自动配置跟踪
        m_targetService.clear();
        m_targetChar.clear();
        m_catalog.clear();
        m_defaultTxFound = false;
        m_configPendingServices = 0;
        m_configFinished = true;
        if (m_configTimer)
            m_configTimer->stop();
        stopTimerSend();
        emit catalogChanged();
    }
    updateActive();
}

void BleTerminal::setPeripheralConnected(bool connected)
{
    if (m_peripheral == connected)
        return;
    m_peripheral = connected;
    if (!connected)
        stopTimerSend();
    updateActive();
}

void BleTerminal::updateActive()
{
    const bool a = m_central || m_peripheral;
    if (m_active != a) {
        m_active = a;
        emit activeChanged();
    }
}

// ---------------------------------------------------------------------
// 服务 / 特征目录（Central 模式）
// ---------------------------------------------------------------------
void BleTerminal::onServicesDiscovered(const QVariantList &services)
{
    if (!m_central || !m_manager)
        return;
    m_catalog.clear();
    m_targetService.clear();
    m_targetChar.clear();
    m_defaultTxFound = false;
    m_pollLastValue.clear();
    emit catalogChanged();

    // 自动配置跟踪：等所有服务细节发现完、通知全部开启后再提示一次，
    // 让用户明确"无需手动展开服务与特征即可收发"。
    m_configTotalServices = services.size();
    m_configPendingServices = services.size();
    m_configNotifyCount = 0;
    if (m_configTimer)
        m_configTimer->start();

    // 关键：发现"所有"服务的特征详情（而不是只处理首个服务），
    // 避免写入特征与通知特征不在同一服务时无法双向通信。
    for (const QVariant &sv : services) {
        const QString uuid = sv.toMap().value(QStringLiteral("uuid")).toString();
        if (!uuid.isEmpty())
            m_manager->discoverDetails(uuid);
    }

    if (m_configPendingServices <= 0)
        finishAutoConfig();
}

void BleTerminal::onDetailsDiscovered(const QString &serviceUuid)
{
    if (!m_central || !m_manager)
        return;

    // 取服务显示名（自定义 BLE 服务通常没有标准名称）
    QString serviceName;
    const QVariantList svcs = m_manager->services();
    for (const QVariant &sv : svcs) {
        const QVariantMap s = sv.toMap();
        if (s.value(QStringLiteral("uuid")).toString() == serviceUuid) {
            serviceName = s.value(QStringLiteral("name")).toString();
            break;
        }
    }

    const QVariantList chars = m_manager->characteristics(serviceUuid);
    bool added = false;
    for (const QVariant &cv : chars) {
        const QVariantMap c = cv.toMap();
        const QString uuid = c.value(QStringLiteral("uuid")).toString();
        if (uuid.isEmpty())
            continue;
        const int props = c.value(QStringLiteral("properties")).toInt();

        CharEntry e;
        e.serviceUuid = serviceUuid;
        e.serviceName = serviceName;
        e.charUuid = uuid;
        e.charName = c.value(QStringLiteral("name")).toString();
        e.properties = props;
        m_catalog.append(e);
        added = true;

        // 自动发送目标：全部特征中首个可写特征（用户可再手动指定）
        if (!m_defaultTxFound
            && (props & (QLowEnergyCharacteristic::Write
                         | QLowEnergyCharacteristic::WriteNoResponse))) {
            m_targetService = serviceUuid;
            m_targetChar = uuid;
            m_defaultTxFound = true;
        }
        // 自动使能通知：让设备能够主动上报，打通"接收"侧通路；
        // 界面可在「服务与特征」中单独关闭某个特征的通知。
        // 注意：目录里的 notifyOn 必须等 CCCD 写入的真正结果（notifyChanged）
        // 再置位，不能在这里提前标 true —— 否则通知开启失败（例如 Qt 在
        // Android 上读不到 CCCD）时，可读特征既收不到推送、又被轮询排除，
        // 数据就彻底进不来了（只能手动点"读取"）。
        if (props & (QLowEnergyCharacteristic::Notify
                     | QLowEnergyCharacteristic::Indicate)) {
            m_manager->enableNotify(serviceUuid, uuid, true);
            ++m_configNotifyCount;   // 请求数，实际是否生效以 notifyChanged 为准
        }
        BLE_DBG_LOG() << "[BLE-DBG] [term] 收录特征: uuid =" << uuid
                 << " name =" << e.charName
                 << " 可读 =" << bool(props & QLowEnergyCharacteristic::Read)
                 << " 可写 =" << bool(props
                                      & (QLowEnergyCharacteristic::Write
                                         | QLowEnergyCharacteristic::WriteNoResponse))
                 << " 可通知 =" << bool(props
                                        & (QLowEnergyCharacteristic::Notify
                                           | QLowEnergyCharacteristic::Indicate));
    }
    if (added)
        emit catalogChanged();

    // 一个服务细节发现完成：计数推进，全部完成则触发自动配置完成提示
    --m_configPendingServices;
    if (m_configPendingServices <= 0)
        finishAutoConfig();
}

void BleTerminal::finishAutoConfig()
{
    if (m_configFinished)
        return;
    m_configFinished = true;
    if (m_configTimer)
        m_configTimer->stop();
    if (m_central) {
        BLE_DBG_LOG() << "[BLE-DBG] [term] ==== 自动配置完成: 服务数 ="
                 << m_configTotalServices
                 << " 已请求开启通知的特征数 =" << m_configNotifyCount
                 << "(以各特征 notifyChanged 回执为准)";
        // 细节发现完成即开始自动轮询：让"只有 Read、没有 Notify"的特征
        // 也能自动把数据读回来并显示，无需用户去点"读取"。
        startPolling();
        emit autoConfigured(qMax(1, m_configTotalServices), m_configNotifyCount);
    }
}

// ---------------------------------------------------------------------
// 自动轮询读取（解决只读型数据特征：BLE Read 只能主机主动拉取）
// ---------------------------------------------------------------------
namespace {
// 判断是否为"信息类"标准服务：这类服务一般只用于查询设备信息/电量，
// 即使可读也不参与数据轮询，避免无意义地反复读取浪费链路带宽。
bool isInfoServiceUuid(const QString &uuid)
{
    // Qt 返回格式为 00001800-0000-1000-8000-00805f9b34fb
    const QString shortId = uuid.mid(4, 4).toUpper();
    static const QStringList infoIds = {
        QStringLiteral("1800"), // Generic Access（设备名/外观等）
        QStringLiteral("1801"), // Generic Attribute
        QStringLiteral("180A"), // Device Information（厂商/固件等）
        QStringLiteral("180F"), // Battery Service
        QStringLiteral("1812"), // Human Interface Device
        QStringLiteral("1816"), // Cycling Speed and Cadence
    };
    return infoIds.contains(shortId);
}
} // namespace

void BleTerminal::setAutoReadEnabled(bool on)
{
    if (m_autoReadEnabled == on)
        return;
    m_autoReadEnabled = on;
    if (on)
        startPolling();
    else
        stopPolling();
    emit autoReadEnabledChanged();
}

void BleTerminal::startPolling()
{
    if (!m_central || !m_autoReadEnabled || !m_pollTimer)
        return;
    if (!m_pollTimer->isActive()) {
        QStringList cands;
        for (const CharEntry &e : m_catalog) {
            if (isPollCandidate(e.serviceUuid, e.charUuid))
                cands << QStringLiteral("%1|%2").arg(e.serviceUuid, e.charUuid);
        }
        BLE_DBG_LOG() << "[BLE-DBG] [term] 启动自动轮询读取(250ms), 轮询候选:"
                 << (cands.isEmpty()
                         ? QStringLiteral("(空 - 无只读特征需要轮询)")
                         : cands.join(QLatin1Char(';')));
        m_pollTimer->start();
    }
}

void BleTerminal::stopPolling()
{
    if (m_pollTimer)
        m_pollTimer->stop();
}

bool BleTerminal::isPollCandidate(const QString &serviceUuid,
                                  const QString &charUuid) const
{
    for (const CharEntry &e : m_catalog) {
        if (e.serviceUuid == serviceUuid && e.charUuid == charUuid) {
            // 必须可读、当前无通知通道、且非信息类标准服务。
            // 注意：发送目标特征仍保留在轮询范围内——很多透传/UART 设备
            // 只有一个“可读可写”的数据特征，主机写入目标就是设备的上报
            // 通道，若整特征排除会导致收不到从机数据。写入后“读回自己刚
            // 写入内容”造成的回显假象，改在 onCentralData 入口按内容过滤。
            return bool(e.properties & QLowEnergyCharacteristic::Read)
                   && !e.notifyOn && !isInfoServiceUuid(serviceUuid);
        }
    }
    return false;
}

void BleTerminal::onPollTick()
{
    if (!m_central || !m_autoReadEnabled || !m_manager)
        return;
    if (m_catalog.isEmpty()) {
        stopPolling();
        return;
    }
    for (const CharEntry &e : m_catalog) {
        if (!(e.properties & QLowEnergyCharacteristic::Read))
            continue;                       // 不可读
        if (e.notifyOn)
            continue;                       // 已有通知通道，不用轮询
        if (isInfoServiceUuid(e.serviceUuid))
            continue;                       // 跳过电量/设备信息等
        m_manager->readCharacteristic(e.serviceUuid, e.charUuid);
    }
}

void BleTerminal::onNotifyChanged(const QString &serviceUuid,
                                  const QString &charUuid,
                                  bool enabled, bool success)
{
    BLE_DBG_LOG() << "[BLE-DBG] [term] 通知(CCCD)最终结果: service =" << serviceUuid
             << " char =" << charUuid
             << " 请求开启 =" << enabled
             << " 实际成功 =" << success;
    for (CharEntry &e : m_catalog) {
        if (e.serviceUuid == serviceUuid && e.charUuid == charUuid) {
            // 以 CCCD 写入的最终结果为准更新目录（写入失败即视为未开启）
            e.notifyOn = success && enabled;
            break;
        }
    }
    emit catalogChanged();
    // 通知未生效的特征若可读，立即纳入轮询读取，保证数据仍能自动到达
    if (m_central && m_autoReadEnabled)
        startPolling();
}

QVariantList BleTerminal::charCatalog() const
{
    QVariantList out;
    for (const CharEntry &e : m_catalog) {
        QVariantMap item;
        item.insert(QStringLiteral("serviceUuid"), e.serviceUuid);
        item.insert(QStringLiteral("serviceName"), e.serviceName);
        item.insert(QStringLiteral("charUuid"), e.charUuid);
        item.insert(QStringLiteral("charName"), e.charName);
        item.insert(QStringLiteral("properties"), e.properties);
        item.insert(QStringLiteral("writable"),
                    bool(e.properties
                         & (QLowEnergyCharacteristic::Write
                            | QLowEnergyCharacteristic::WriteNoResponse)));
        item.insert(QStringLiteral("readable"),
                    bool(e.properties & QLowEnergyCharacteristic::Read));
        item.insert(QStringLiteral("notifiable"),
                    bool(e.properties
                         & (QLowEnergyCharacteristic::Notify
                            | QLowEnergyCharacteristic::Indicate)));
        item.insert(QStringLiteral("notifyOn"), e.notifyOn);
        item.insert(QStringLiteral("isSendTarget"),
                    e.serviceUuid == m_targetService
                        && e.charUuid == m_targetChar);
        out.append(item);
    }
    return out;
}

void BleTerminal::setSendTarget(const QString &serviceUuid,
                                const QString &charUuid)
{
    bool writable = false;
    for (const CharEntry &e : m_catalog) {
        if (e.serviceUuid == serviceUuid && e.charUuid == charUuid) {
            writable = bool(e.properties
                            & (QLowEnergyCharacteristic::Write
                               | QLowEnergyCharacteristic::WriteNoResponse));
            break;
        }
    }
    if (!writable) {
        emit errorOccurred(QStringLiteral("所选特征不支持写入，无法作为发送目标"));
        return;
    }
    if (m_targetService == serviceUuid && m_targetChar == charUuid)
        return;
    m_targetService = serviceUuid;
    m_targetChar = charUuid;
    emit catalogChanged();
}

void BleTerminal::readNow(const QString &serviceUuid, const QString &charUuid)
{
    if (!m_central || !m_manager) {
        emit errorOccurred(QStringLiteral("请先连接蓝牙"));
        return;
    }
    bool readable = false;
    for (const CharEntry &e : m_catalog) {
        if (e.serviceUuid == serviceUuid && e.charUuid == charUuid) {
            readable = bool(e.properties & QLowEnergyCharacteristic::Read);
            break;
        }
    }
    if (!readable) {
        emit errorOccurred(QStringLiteral("所选特征不支持读取"));
        return;
    }
    BLE_DBG_LOG() << "[BLE-DBG] [term] ======== 手动点击「读取」按钮 ========  char ="
             << charUuid;
    m_manager->readCharacteristic(serviceUuid, charUuid);
}

void BleTerminal::toggleNotify(const QString &serviceUuid,
                               const QString &charUuid, bool enable)
{
    if (!m_central || !m_manager) {
        emit errorOccurred(QStringLiteral("请先连接蓝牙"));
        return;
    }
    bool found = false;
    bool notifiable = false;
    for (const CharEntry &e : m_catalog) {
        if (e.serviceUuid == serviceUuid && e.charUuid == charUuid) {
            found = true;
            notifiable = bool(e.properties
                              & (QLowEnergyCharacteristic::Notify
                                 | QLowEnergyCharacteristic::Indicate));
            break;
        }
    }
    if (!found) {
        emit errorOccurred(QStringLiteral("特征不存在，请重新连接设备"));
        return;
    }
    if (!notifiable) {
        emit errorOccurred(QStringLiteral("该特征不支持通知(Notify)"));
        return;
    }
    // 注意：这里不预先置 notifyOn。开启失败的特征若被误标为"通知已开"，
    // 会把可读特征的自动轮询回退一并堵死；目录状态统一以 enableNotify 的
    // 最终结果（notifyChanged 回调）为准，保证失败的特征能回退轮询读取。
    m_manager->enableNotify(serviceUuid, charUuid, enable);
}

// ---------------------------------------------------------------------
// 数据收发
// ---------------------------------------------------------------------
void BleTerminal::appendReceived(const QByteArray &raw)
{
    if (raw.isEmpty())
        return;
    // 仅对新收到的数据按当前 hexReceive 开关决定显示格式；
    // 之前已收到的数据保持原样（不做二次转换）。
    const QString display = m_hexReceive ? bytesToHex(raw)
                                         : QString::fromUtf8(raw);
    appendLog(display, true);
}

void BleTerminal::appendSent(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    // 发送回显按发送侧格式(hexSend)展示，避免二进制字节直接按 UTF-8 显示成乱码
    const QString display = m_hexSend ? bytesToHex(bytes)
                                      : QString::fromUtf8(bytes);
    appendLog(display, false);
}

void BleTerminal::appendLog(const QString &display, bool receive)
{
    if (display.isEmpty())
        return;
    // 限制总长度，防止长时间收发导致界面卡顿（保留最近数据）
    const int maxLen = 200000;
    const QString tag = receive ? QStringLiteral("[接收]:")
                                : QStringLiteral("[发送]:");
    // 每条收发记录独占一行。时间戳开启时行首带 [hh:mm:ss.zzz]；
    // 关闭时间戳时仍保留 [接收]/[发送] 标记分行显示，便于区分来源。
    if (!m_receivedText.isEmpty()
        && !m_receivedText.endsWith(QLatin1Char('\n')))
        m_receivedText += QLatin1Char('\n');
    QString line;
    if (m_timestampEnabled) {
        // 格式 [hh:mm:ss.zzz] [接收]: <数据> / [hh:mm:ss.zzz] [发送]: <数据>
        line = QStringLiteral("[%1] %2 %3")
            .arg(QDateTime::currentDateTime()
                     .toString(QStringLiteral("hh:mm:ss.zzz")),
                 tag, display);
    } else {
        // 格式 [接收]: <数据> / [发送]: <数据>
        line = tag + QLatin1Char(' ') + display;
    }
    if (m_receivedText.size() + line.size() > maxLen)
        m_receivedText = m_receivedText.right(maxLen - line.size());
    m_receivedText += line;
    emit receivedTextChanged();
    // 同步告知其它视图（如指令页日志框）本条收发记录，
    // 使其与调试文本框使用同一格式化逻辑展示
    emit dataLogged(display, receive);
}

void BleTerminal::onCentralData(const QString &serviceUuid,
                                const QString &charUuid,
                                const QByteArray &data)
{
    // 过滤"写入回读"假象：BLE 读请求按协议返回特征当前值。发送目标特征
    // 若同时可读（很多透传设备只有一个可读可写数据特征，轮询必须读它），
    // 写入指令后轮询立刻读回的内容正是刚写入的指令本身，看起来像设备端
    // "回显"，而设备往往并没有回显逻辑。
    //
    // 过滤范围刻意收窄：仅当该特征"未开启通知"时才执行——此时数据只能
    // 来自 Read 回包（轮询或手动读取），不存在设备主动推送，读到与刚写入
    // 相同的内容即可判定为写后读回。若特征开了 Notify/Indicate，到达数据
    // 是设备主动推送（真上报或设备的真回显），一律原样显示，不做过滤。
    bool notifyActive = false;
    for (const CharEntry &e : m_catalog) {
        if (e.serviceUuid == serviceUuid && e.charUuid == charUuid) {
            notifyActive = e.notifyOn;
            break;
        }
    }
    if (!notifyActive && !m_lastWrittenPayload.isEmpty()
        && serviceUuid == m_lastWrittenService
        && charUuid == m_lastWrittenChar
        && data == m_lastWrittenPayload) {
        const QString key = serviceUuid + QLatin1Char('|') + charUuid;
        m_pollLastValue.insert(key, data);
        return;
    }
    // 轮询读取去重：对走"自动轮询"的特征，连续两次读到相同值说明
    // 设备端数据未更新（如电池电量这类只读静态值），不再重复刷屏；
    // 只有值变化才显示。有通知通道的特征不在此列，每包都正常显示。
    if (m_autoReadEnabled && isPollCandidate(serviceUuid, charUuid)) {
        const QString key = serviceUuid + QLatin1Char('|') + charUuid;
        if (m_pollLastValue.value(key) == data) {
            BLE_DBG_LOG() << "[BLE-DBG] [term] 轮询读到相同值，已去重不显示: char ="
                     << charUuid << " len =" << data.size();
            return;
        }
        m_pollLastValue.insert(key, data);
    }
    appendReceived(data);
}

void BleTerminal::onPeripheralData(const QByteArray &data)
{
    appendReceived(data);
}

void BleTerminal::clearReceived()
{
    if (m_receivedText.isEmpty())
        return;
    m_receivedText.clear();
    emit receivedTextChanged();
}

void BleTerminal::sendText(const QString &text)
{
    if (!m_active) {
        emit errorOccurred(QStringLiteral("请先连接蓝牙"));
        return;
    }
    if (text.trimmed().isEmpty()) {
        emit errorOccurred(QStringLiteral("发送内容为空"));
        return;
    }

    QByteArray bytes;
    if (m_hexSend) {
        bool ok = false;
        bytes = parseHex(text, &ok);
        if (!ok) {
            emit errorOccurred(
                QStringLiteral("十六进制格式错误：仅允许 0-9 A-F 及空格等分隔符，"
                               "且字节数为偶数"));
            return;
        }
    } else {
        bytes = text.toUtf8();
    }
    sendPayload(bytes);
}

void BleTerminal::sendAsciiText(const QString &text)
{
    if (!m_active) {
        emit errorOccurred(QStringLiteral("请先连接蓝牙"));
        return;
    }
    if (text.trimmed().isEmpty()) {
        emit errorOccurred(QStringLiteral("发送内容为空"));
        return;
    }
    sendPayload(text.toUtf8());
}

void BleTerminal::sendPayload(const QByteArray &bytes)
{
    if (!m_manager || bytes.isEmpty())
        return;
    // 发送内容始终回显（与时间戳开关无关）；不开时间戳时以
    // "[发送]: <数据>" 分行显示，避免发送的数据"看不到"。
    if (m_peripheral) {
        // Peripheral 角色：向已连接的 Central 设备下发
        m_manager->sendPeripheralData(bytes);
        appendSent(bytes);
        return;
    }
    // Central 角色：写入所选发送特征
    if (m_targetChar.isEmpty()) {
        emit errorOccurred(
            QStringLiteral("尚未确定写入特征，请在「服务与特征」中选择一个可写特征作为发送目标"));
        return;
    }
    m_manager->writeCharacteristic(m_targetService, m_targetChar, bytes);
    // 记录最近一次本机写入：轮询读回目标特征时若内容与此完全相同，
    // 判定为"写后回读"而非设备上报数据，在 onCentralData 中过滤。
    m_lastWrittenService = m_targetService;
    m_lastWrittenChar = m_targetChar;
    m_lastWrittenPayload = bytes;
    // 发送内容始终回显到文本框（与时间戳开关无关，行内是否带时间
    // 戳由 appendLog 按 m_timestampEnabled 决定），含定时发送每次触发。
    appendSent(bytes);
}

// ---------------------------------------------------------------------
// 导出目录（自定义目录，可持久化）
// ---------------------------------------------------------------------
void BleTerminal::setExportTarget(const QString &target)
{
    QString t = target.trimmed();
    // 兼容 file:// 形式的目录 URL（桌面 QML FolderDialog 传原始 URL）
    if (t.startsWith(QLatin1String("file://"))) {
        const QUrl u(t);
        if (u.isLocalFile())
            t = u.toLocalFile();
    }
    if (m_exportTarget == t)
        return;
    m_exportTarget = t;
    persistExportTarget();
    updateExportDisplay();
    emit exportTargetChanged();
}

void BleTerminal::updateExportDisplay()
{
    QString d = m_exportTarget;
    if (d.startsWith(QLatin1String("content://"))) {
#ifdef Q_OS_ANDROID
        const QString friendly = androidCallStaticString(
            "org/qtproject/example/SafStorage", "displayPath", d);
        if (!friendly.isEmpty())
            d = friendly;
#else
        d.clear();
#endif
    }
    if (m_exportDisplay != d) {
        m_exportDisplay = d;
        emit exportDisplayChanged();
    }
}

void BleTerminal::persistExportTarget()
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("BLE_SAR"), QStringLiteral("BLE_SAR"));
    settings.setValue(QStringLiteral("terminal/exportTarget"), m_exportTarget);
    settings.sync();
}

bool BleTerminal::pickFolder()
{
#ifdef Q_OS_ANDROID
    auto *receiver = new FolderPickReceiver;
    receiver->terminal = this;

    const QJniObject action = QJniObject::fromString(
        QStringLiteral("android.intent.action.OPEN_DOCUMENT_TREE"));
    QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V",
                      action.object<jstring>());
    // Intent.FLAG_GRANT_READ_URI_PERMISSION(1) | FLAG_GRANT_WRITE_URI_PERMISSION(2)
    // | FLAG_GRANT_PERSISTABLE_URI_PERMISSION(64)
    intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", 1 | 2 | 64);

    const QAndroidIntent androidIntent(intent);
    QtAndroidPrivate::startActivity(androidIntent, kPickFolderRequestCode,
                                    receiver);
    return true;
#else
    Q_UNUSED(this);
    return false;
#endif
}

void BleTerminal::onFolderPickedResult(const QString &treeUri)
{
#ifdef Q_OS_ANDROID
    if (treeUri.isEmpty()) {
        emit errorOccurred(QStringLiteral("未选择目录，已取消"));
        return;
    }
    // 持久化 SAF 目录授权：重启应用后仍可直接写入所选目录
    QJniObject::callStaticMethod<void>(
        "org/qtproject/example/SafStorage", "persistPermission",
        "(Ljava/lang/String;)V", QJniObject::fromString(treeUri));

    setExportTarget(treeUri);
    // 无论是否变更都通知界面（setExportTarget 仅在有变化时才发信号）
    updateExportDisplay();
    emit exportTargetChanged();
    emit folderPicked(m_exportTarget, m_exportDisplay);
#else
    Q_UNUSED(treeUri);
#endif
}

QString BleTerminal::volumeProbePath()
{
    QString p = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    if (p.isEmpty())
        p = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (p.isEmpty())
        p = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return p;
}

qint64 BleTerminal::neededBytes() const
{
    // 实际写入内容为 UTF-8 文本，需占用字节数即编码后长度
    return qMax<qint64>(1, qint64(m_receivedText.toUtf8().size()));
}

QVariantMap BleTerminal::storageInfo() const
{
    QVariantMap map;
    qint64 total = -1;
    qint64 freeB = -1;
    const QString target = m_exportTarget;

#ifdef Q_OS_ANDROID
    if (target.startsWith(QLatin1String("content://"))) {
        // Android SAF 目录无真实路径可查，由 Java 端按所在卷返回统计
        const QString stat = androidCallStaticString(
            "org/qtproject/example/SafStorage", "storageBytes", target);
        const QStringList parts = stat.split(QLatin1Char(';'));
        if (parts.size() == 2) {
            total = parts.at(0).toLongLong();
            freeB = parts.at(1).toLongLong();
        }
    } else {
        const QStorageInfo info(target.isEmpty()
                                    ? volumeProbePath()
                                    : target);
        if (info.isValid()) {
            total = info.bytesTotal();
            freeB = info.bytesAvailable();
        }
    }
#else
    {
        const QStorageInfo info(target.isEmpty() ? volumeProbePath() : target);
        if (info.isValid()) {
            total = info.bytesTotal();
            freeB = info.bytesAvailable();
        }
    }
#endif

    const qint64 need = neededBytes();
    map.insert(QStringLiteral("totalBytes"), total);
    map.insert(QStringLiteral("freeBytes"), freeB);
    map.insert(QStringLiteral("neededBytes"), need);
    // 读不到空间信息时不拦截导出，否则可用空间不足时禁止导出
    map.insert(QStringLiteral("enough"),
               (total < 0 || freeB < 0) ? true : (freeB >= need));
    return map;
}

// ---------------------------------------------------------------------
// 导出
// ---------------------------------------------------------------------
QString BleTerminal::saveReceived(const QString &fileName)
{
    if (m_receivedText.trimmed().isEmpty()) {
        emit errorOccurred(QStringLiteral("接收区暂无数据，无法导出"));
        return QString();
    }
    QString name = fileName.trimmed();
    if (name.isEmpty()) {
        // 文件名留空时自动生成：BLE_SAR_年份_月日_时分
        // 例：BLE_SAR_2026_0903_1703（2026年09月03日 17:03）
        name = QStringLiteral("BLE_SAR_%1")
                   .arg(QDateTime::currentDateTime()
                            .toString(QStringLiteral("yyyy_MMdd_HHmm")));
    }
    if (!name.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive))
        name += QLatin1String(".txt");

    const QByteArray data = m_receivedText.toUtf8();

#ifdef Q_OS_ANDROID
    // Android SAF：所选目录为 content:// 树 URI，必须经由内容提供者写入
    if (m_exportTarget.startsWith(QLatin1String("content://"))) {
        // QJniObject 无 fromByteArray，需经 QJniEnvironment 构造 jbyteArray
        QJniEnvironment env;
        const jbyteArray payload = env->NewByteArray(data.size());
        if (data.size() > 0) {
            env->SetByteArrayRegion(
                payload, 0, data.size(),
                reinterpret_cast<const jbyte *>(data.constData()));
        }
        const QJniObject res = QJniObject::callStaticObjectMethod(
            "org/qtproject/example/SafStorage", "saveFile",
            "(Ljava/lang/String;Ljava/lang/String;[B)Ljava/lang/String;",
            QJniObject::fromString(m_exportTarget).object<jstring>(),
            QJniObject::fromString(name).object<jstring>(),
            payload);
        env->DeleteLocalRef(payload);
        if (res.isValid()) {
            const QString out = res.toString();
            if (!out.startsWith(QLatin1String("ERR:")))
                return out;   // 返回 content:// URI（界面可显示解析名）
            emit errorOccurred(out.mid(4));
            return QString();
        }
        emit errorOccurred(QStringLiteral("导出失败：无法访问所选目录"));
        return QString();
    }
#endif

    if (m_exportTarget.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先在「选择目录」中指定导出保存位置"));
        return QString();
    }
    QDir dir(m_exportTarget);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        emit errorOccurred(
            QStringLiteral("保存目录不存在且无法创建: %1").arg(m_exportTarget));
        return QString();
    }
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        emit errorOccurred(
            QStringLiteral("保存失败，无法写入文件: %1").arg(path));
        return QString();
    }
    f.write(data);
    f.close();
    return path;
}

// ---------------------------------------------------------------------
// 定时发送
// ---------------------------------------------------------------------
void BleTerminal::startTimerSend(const QString &unit, double interval,
                                 double duration, const QString &payload)
{
    if (!m_active) {
        emit errorOccurred(QStringLiteral("请先连接蓝牙"));
        return;
    }

    int unitMs = 1000;
    if (unit.contains(QStringLiteral("毫秒"))
        || unit.contains(QLatin1String("ms"), Qt::CaseInsensitive))
        unitMs = 1;
    else if (unit.contains(QStringLiteral("分")))
        unitMs = 60000;

    m_timerIntervalMs = qMax(10, int(interval * unitMs));
    m_timerTotalMs = duration > 0 ? qint64(duration * unitMs) : -1;
    m_timerElapsedMs = 0;

    bool ok = false;
    if (m_hexSend) {
        m_timerPayload = parseHex(payload, &ok);
        if (!ok) {
            emit errorOccurred(QStringLiteral("十六进制格式错误，定时发送未启动"));
            return;
        }
    } else {
        m_timerPayload = payload.toUtf8();
    }
    if (m_timerPayload.isEmpty()) {
        emit errorOccurred(QStringLiteral("发送内容为空，定时发送未启动"));
        return;
    }

    m_timer->start(m_timerIntervalMs);
    if (!m_timerEnabled) {
        m_timerEnabled = true;
        emit timerEnabledChanged();
    }
    sendPayload(m_timerPayload);
}

void BleTerminal::stopTimerSend()
{
    if (m_timer)
        m_timer->stop();
    if (m_timerEnabled) {
        m_timerEnabled = false;
        emit timerEnabledChanged();
    }
}

void BleTerminal::setTimerEnabled(bool on)
{
    if (!on)
        stopTimerSend();
    // 开启由 QML 弹窗流程经 startTimerSend 完成，这里只处理关闭
}

void BleTerminal::onTimerTick()
{
    if (!m_active) {
        stopTimerSend();
        return;
    }
    sendPayload(m_timerPayload);
    m_timerElapsedMs += m_timerIntervalMs;
    if (m_timerTotalMs > 0 && m_timerElapsedMs >= m_timerTotalMs)
        stopTimerSend();
}
