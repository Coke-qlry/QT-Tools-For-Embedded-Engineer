#include "debug_checkbox_status_control.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QVariantMap>
#include <QDebug>

// 复用 SqliteWarehouse 的仓库目录策略，保证桌面 / Android 都落在可写位置
#include "../sqlite/SQLite.h"

namespace {

// 取出 SqliteWarehouse::warehouseDir() 的目录路径；
// 该目录已经由 SqliteWarehouse 自身确保存在
QString warehouseDirectory()
{
    return SqliteWarehouse::warehouseDir();
}

} // namespace

DebugCheckboxStatusControl::DebugCheckboxStatusControl(QObject *parent)
    : QObject(parent)
{
}

DebugCheckboxStatusControl::~DebugCheckboxStatusControl()
{
    closeDatabase();
}

void DebugCheckboxStatusControl::initialize()
{
    if (m_initialized) return;
    if (!openDatabase()) {
        emit errorOccurred(QStringLiteral("CheckBox 状态仓库初始化失败"));
        return;
    }
    m_initialized = true;
}

bool DebugCheckboxStatusControl::openDatabase()
{
    // 1) 保证仓库目录存在（与 SqliteWarehouse 保持一致的落盘位置）
    const QString dir = warehouseDirectory();
    QDir().mkpath(dir);
    const QString filePath = QDir(dir).filePath(
        QString::fromUtf8(kDbName) + QStringLiteral(".db"));

    // 2) 创建/复用独立连接（与 SqliteWarehouse 的连接互不干扰）
    {
        QSqlDatabase existing = QSqlDatabase::contains(QString::fromUtf8(kConnName))
                                ? QSqlDatabase::database(QString::fromUtf8(kConnName))
                                : QSqlDatabase();
        m_db = new QSqlDatabase(existing);
    }
    if (!m_db->isOpen()) {
        *m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                           QString::fromUtf8(kConnName));
        m_db->setDatabaseName(filePath);
        if (!m_db->open()) {
            const QString err = m_db->lastError().text();
            qWarning() << "[DebugCheckboxStatusControl] 打开数据库失败:" << err
                       << "path=" << filePath;
            delete m_db;
            m_db = nullptr;
            return false;
        }
    }
    // 3) 建表（已存在则 PRAGMA 检查字段一致；本模块表结构由我们自己保证）
    if (!ensureCheckboxTable() || !ensureBoundDevicesTable()) {
        m_db->close();
        QSqlDatabase::removeDatabase(QString::fromUtf8(kConnName));
        delete m_db;
        m_db = nullptr;
        return false;
    }
    return true;
}

void DebugCheckboxStatusControl::closeDatabase()
{
    if (m_db) {
        if (m_db->isOpen()) m_db->close();
        // 注意：removeDatabase 必须在连接未再被引用时调用，否则 Qt 会断言
        QSqlDatabase::removeDatabase(QString::fromUtf8(kConnName));
        delete m_db;
        m_db = nullptr;
    }
}

bool DebugCheckboxStatusControl::ensureCheckboxTable()
{
    if (!m_db || !m_db->isOpen()) return false;
    QSqlQuery q(*m_db);
    const QString sql = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS %1 ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ")").arg(QString::fromUtf8(kCheckboxTbl));
    if (!q.exec(sql)) {
        qWarning() << "[DebugCheckboxStatusControl] 建 CheckBox 表失败:"
                   << q.lastError().text();
        return false;
    }
    return true;
}

bool DebugCheckboxStatusControl::ensureBoundDevicesTable()
{
    if (!m_db || !m_db->isOpen()) return false;
    QSqlQuery q(*m_db);
    const QString sql = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS %1 ("
        "  address    TEXT PRIMARY KEY,"
        "  name       TEXT NOT NULL,"
        "  bound_time INTEGER NOT NULL"
        ")").arg(QString::fromUtf8(kBoundTbl));
    if (!q.exec(sql)) {
        qWarning() << "[DebugCheckboxStatusControl] 建 bound_devices 表失败:"
                   << q.lastError().text();
        return false;
    }
    return true;
}

bool DebugCheckboxStatusControl::getBoolValue(const char *key, bool fallback) const
{
    if (!m_db || !m_db->isOpen() || !key) return fallback;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("SELECT value FROM %1 WHERE key = ?")
                  .arg(QString::fromUtf8(kCheckboxTbl)));
    q.addBindValue(QString::fromUtf8(key));
    if (!q.exec()) return fallback;
    if (!q.next()) return fallback;
    const QString v = q.value(0).toString();
    return v == QStringLiteral("1") || v.compare(QStringLiteral("true"),
                                                  Qt::CaseInsensitive) == 0;
}

bool DebugCheckboxStatusControl::setBoolValue(const char *key, bool v)
{
    if (!m_db || !m_db->isOpen() || !key) return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO %1(key, value) VALUES(?, ?)"
        " ON CONFLICT(key) DO UPDATE SET value = excluded.value")
        .arg(QString::fromUtf8(kCheckboxTbl)));
    q.addBindValue(QString::fromUtf8(key));
    q.addBindValue(v ? QStringLiteral("1") : QStringLiteral("0"));
    if (!q.exec()) {
        qWarning() << "[DebugCheckboxStatusControl] 写 CheckBox 状态失败 key="
                   << key << ":" << q.lastError().text();
        return false;
    }
    return true;
}

void DebugCheckboxStatusControl::setHexSend(bool v)
{
    if (setBoolValue(KHexSend, v)) emit hexSendChanged();
}

void DebugCheckboxStatusControl::setHexReceiveSpaced(bool v)
{
    if (setBoolValue(KHexReceiveSpaced, v)) emit hexReceiveSpacedChanged();
}

void DebugCheckboxStatusControl::setHexReceiveNoSpace(bool v)
{
    if (setBoolValue(KHexReceiveNoSpace, v)) emit hexReceiveNoSpaceChanged();
}

void DebugCheckboxStatusControl::setTimestampEnabled(bool v)
{
    if (setBoolValue(KTimestampEnabled, v)) emit timestampEnabledChanged();
}

void DebugCheckboxStatusControl::setBindCurrentDevice(bool v)
{
    if (setBoolValue(KBindCurrentDevice, v)) emit bindCurrentDeviceChanged();
}

QString DebugCheckboxStatusControl::normalizeAddress(const QString &address)
{
    return address.trimmed().toLower();
}

QVariantList DebugCheckboxStatusControl::boundDevices() const
{
    QVariantList list;
    if (!m_db || !m_db->isOpen()) return list;
    QSqlQuery q(*m_db);
    // 按绑定时间倒序，最近绑定的在最上面（与「依次向下」展示顺序一致）
    q.prepare(QStringLiteral(
        "SELECT address, name, bound_time FROM %1 ORDER BY bound_time DESC")
        .arg(QString::fromUtf8(kBoundTbl)));
    if (!q.exec()) {
        qWarning() << "[DebugCheckboxStatusControl] 读 bound_devices 失败:"
                   << q.lastError().text();
        return list;
    }
    while (q.next()) {
        QVariantMap m;
        m.insert(QStringLiteral("address"),
                 q.value(0).toString());
        m.insert(QStringLiteral("name"),
                 q.value(1).toString());
        m.insert(QStringLiteral("boundTime"),
                 q.value(2).toLongLong());
        list.append(m);
    }
    return list;
}

bool DebugCheckboxStatusControl::bindDevice(const QString &name,
                                            const QString &address)
{
    if (!m_db || !m_db->isOpen()) return false;
    const QString addr = normalizeAddress(address);
    if (addr.isEmpty()) return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO %1(address, name, bound_time) VALUES(?, ?, ?)"
        " ON CONFLICT(address) DO UPDATE SET"
        "   name = excluded.name,"
        "   bound_time = excluded.bound_time")
        .arg(QString::fromUtf8(kBoundTbl)));
    q.addBindValue(addr);
    q.addBindValue(name.isEmpty() ? QStringLiteral("N/A") : name);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        qWarning() << "[DebugCheckboxStatusControl] 写入绑定失败:"
                   << q.lastError().text();
        return false;
    }
    emit boundDevicesChanged();
    return true;
}

bool DebugCheckboxStatusControl::unbindDevice(const QString &address)
{
    if (!m_db || !m_db->isOpen()) return false;
    const QString addr = normalizeAddress(address);
    if (addr.isEmpty()) return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("DELETE FROM %1 WHERE address = ?")
                  .arg(QString::fromUtf8(kBoundTbl)));
    q.addBindValue(addr);
    if (!q.exec()) {
        qWarning() << "[DebugCheckboxStatusControl] 解绑失败:"
                   << q.lastError().text();
        return false;
    }
    emit boundDevicesChanged();
    return true;
}

bool DebugCheckboxStatusControl::isBound(const QString &address) const
{
    if (!m_db || !m_db->isOpen()) return false;
    const QString addr = normalizeAddress(address);
    if (addr.isEmpty()) return false;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM %1 WHERE address = ? LIMIT 1")
                  .arg(QString::fromUtf8(kBoundTbl)));
    q.addBindValue(addr);
    if (!q.exec() || !q.next()) return false;
    return true;
}

QStringList DebugCheckboxStatusControl::boundAddresses() const
{
    QStringList out;
    if (!m_db || !m_db->isOpen()) return out;
    QSqlQuery q(*m_db);
    q.prepare(QStringLiteral("SELECT address FROM %1")
                  .arg(QString::fromUtf8(kBoundTbl)));
    if (!q.exec()) {
        qWarning() << "[DebugCheckboxStatusControl] 读已绑定地址失败:"
                   << q.lastError().text();
        return out;
    }
    while (q.next()) out.append(q.value(0).toString());
    return out;
}
