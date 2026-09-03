#include "SQLite.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

// ============================================================================
// SqliteWarehouse 实现
// ============================================================================

namespace {
const QString kPrefix = QStringLiteral("[SqliteWarehouse] ");
QString g_warehouseDir;

// 解析 QML 端 JSON.stringify(数组) 产生的 JSON 字符串 → QStringList。
// 解析失败（非数组/坏 JSON）时打印原因并返回空列表（上层会继续报错）。
QStringList jsonToStringList(const QString &json, const char *what)
{
    QStringList list;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        qWarning().noquote() << kPrefix << "JSON 解析失败" << what << ":"
                             << pe.errorString() << "原文:" << json;
        return list;
    }
    if (!doc.isArray()) {
        qWarning().noquote() << kPrefix << what << "不是 JSON 数组:" << json;
        return list;
    }
    const QJsonArray arr = doc.array();
    list.reserve(arr.size());
    for (const QJsonValue &v : arr)
        list.append(v.toString());
    return list;
}

// 解析 QML 端以 \x1F 连接的多值字符串 → QStringList。
// 说明：\x1F（Unit Separator）几乎不可能出现在指令名称/内容等业务文本里；
// 该通道不涉及 JSON 解析，规避 qmlcache 真机上 JSON.stringify 结果错乱的问题。
QStringList splitBySep(const QString &joined)
{
    if (joined.isEmpty())
        return QStringList();
    return joined.split(QChar(0x1F), Qt::KeepEmptyParts);
}


// 默认目录：桌面开发环境用可执行文件旁的 sqlite_warehouse/（方便直接查看 .db）；
// Android/iOS 用应用数据目录（可写）。
QString defaultDir()
{
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    return base + QStringLiteral("/sqlite_warehouse");
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/sqlite_warehouse");
#endif
}
} // namespace

void SqliteWarehouse::setWarehouseDir(const QString &dir)
{
    const QString d = dir.trimmed();
    g_warehouseDir = d.isEmpty() ? defaultDir() : QDir::cleanPath(d);
}

QString SqliteWarehouse::warehouseDir()
{
    if (g_warehouseDir.isEmpty())
        g_warehouseDir = defaultDir();
    return g_warehouseDir;
}

bool SqliteWarehouse::ensureDirectory() const
{
    if (QDir().mkpath(warehouseDir()))
        return true;
    qWarning().noquote() << kPrefix << "创建仓库目录失败: " << warehouseDir();
    return false;
}

// ---------------------------------------------------------------- 构造/析构 --
SqliteWarehouse::SqliteWarehouse(QObject *parent)
    : QObject(parent)
{
}

SqliteWarehouse::~SqliteWarehouse()
{
    // 退出时关闭并注销本次运行打开的所有连接（不删除数据库文件）
    for (const QString &name : std::as_const(m_openDbs)) {
        const QString conn = connectionNameFor(name);
        QSqlDatabase db = QSqlDatabase::database(conn, /*open=*/false);
        if (db.isValid() && db.isOpen())
            db.close();
        db = QSqlDatabase(); // 释放引用后再注销
        QSqlDatabase::removeDatabase(conn);
    }
    m_openDbs.clear();
}

// ---------------------------------------------------------------- 私有工具 --
QString SqliteWarehouse::filePathFor(const QString &dbName) const
{
    return warehouseDir() + QLatin1Char('/') + dbName + QStringLiteral(".db");
}

QString SqliteWarehouse::connectionNameFor(const QString &dbName) const
{
    return QStringLiteral("sqlite_wh_conn_") + dbName;
}

bool SqliteWarehouse::nameIsValid(const QString &dbName, QString *err) const
{
    if (dbName.trimmed().isEmpty() || dbName.contains(QLatin1Char('/'))
        || dbName.contains(QLatin1Char('\\')) || dbName.contains(QLatin1Char('"'))
        || dbName == QStringLiteral(".") || dbName == QStringLiteral("..")) {
        if (err)
            *err = QStringLiteral("非法的数据库名: \"%1\"").arg(dbName);
        return false;
    }
    return true;
}

QString SqliteWarehouse::escId(const QString &identifier) const
{
    QString t = identifier;
    t.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + t + QLatin1Char('"');
}

QString SqliteWarehouse::formatRow(const QString &dbName, const QStringList &values) const
{
    QStringList parts;
    parts << QStringLiteral("[%1]").arg(dbName);
    for (const QString &v : values)
        parts << QStringLiteral("[%1]").arg(v);
    return parts.join(QLatin1Char('-'));
}

bool SqliteWarehouse::openWarehouse(const QString &dbName, QString *err) const
{
    if (m_openDbs.contains(dbName))
        return true;

    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        if (err)
            *err = QStringLiteral("当前 Qt 环境未加载 QSQLITE 驱动");
        return false;
    }
    if (!ensureDirectory())
        return false;

    const QString conn = connectionNameFor(dbName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(filePathFor(dbName));
    if (!db.open()) {
        const QString last = db.lastError().text();
        if (err)
            *err = QStringLiteral("打开数据库失败: %1").arg(last);
        db = QSqlDatabase();               // 释放引用后再注销，避免告警
        QSqlDatabase::removeDatabase(conn);
        return false;
    }
    m_openDbs.insert(dbName);
    return true;
}

bool SqliteWarehouse::databaseFor(const QString &dbName, QSqlDatabase &db, QString *err) const
{
    // 库必须先被 create_sqlite_warehouse 创建过（文件存在）
    if (!QFile::exists(filePathFor(dbName))) {
        if (err)
            *err = QStringLiteral("请先创建数据库: \"%1\"").arg(dbName);
        return false;
    }
    if (!m_openDbs.contains(dbName)) {
        if (!openWarehouse(dbName, err))      // m_openDbs 为 mutable，可在此登记连接
            return false;
    }
    db = QSqlDatabase::database(connectionNameFor(dbName));
    return db.isValid() && db.isOpen();
}

bool SqliteWarehouse::configTableFields(const QSqlDatabase &db, QStringList *fields,
                                        QString *err) const
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("PRAGMA table_info(%1)").arg(escId(QLatin1String(kConfigTable))));
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    fields->clear();
    while (q.next())
        fields->append(q.value(1).toString());   // 列 1 为字段名
    return true;
}

bool SqliteWarehouse::createConfigTable(const QSqlDatabase &db, const QStringList &fields,
                                        bool *created, QString *err) const
{
    if (created)
        *created = false;

    QStringList existing;
    if (!configTableFields(db, &existing, err))
        return false;

    if (!existing.isEmpty()) {
        // 已存在：字段完全一致(忽略顺序)则视为“已配置好”，直接可用
        QStringList a = existing, b = fields;
        a.sort();
        b.sort();
        if (a == b)
            return true;   // 已存在且一致
        if (err)
            *err = QStringLiteral("配置结构与已有表不一致：现有[%1]，传入[%2]；"
                                  "如需重建请先调用 delete_sqlite_wh_all 或 delete_sqlite_all")
                       .arg(existing.join(QLatin1String(", ")), fields.join(QLatin1String(", ")));
        return false;
    }

    // 新建表：所有字段均为 TEXT，值统一文本化存储
    QString sql = QStringLiteral("CREATE TABLE %1 (").arg(escId(QLatin1String(kConfigTable)));
    for (int i = 0; i < fields.size(); ++i) {
        if (i > 0)
            sql += QStringLiteral(", ");
        sql += escId(fields.at(i)) + QStringLiteral(" TEXT");
    }
    sql += QLatin1Char(')');

    QSqlQuery q(db);
    if (!q.exec(sql)) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    if (created)
        *created = true;
    return true;
}

// ================================================================= 公开 API ==

// ------------------------------------------------------------ 1. 创建数据库 --
bool SqliteWarehouse::create_sqlite_warehouse(const QString &dbName)
{
    const QString name = dbName.trimmed();
    QString err;
    if (!nameIsValid(name, &err)) {
        qWarning().noquote() << kPrefix << err;
        return false;
    }

    if (m_openDbs.contains(name)) {   // 本进程已创建过
        qInfo().noquote() << kPrefix << "数据库已创建并在使用中，直接复用:" << name;
        return true;
    }

    const bool existed = QFile::exists(filePathFor(name));
    if (!openWarehouse(name, &err)) {
        qWarning().noquote() << kPrefix << "创建数据库失败:" << name << "→" << err;
        return false;
    }
    qInfo().noquote() << kPrefix
                      << (existed ? QStringLiteral("数据库已存在，直接复用：")
                                  : QStringLiteral("数据库创建成功："))
                      << name << "→ 文件:" << filePathFor(name);
    return true;
}

// ------------------------------------------------------------ 2. 配置结构 --
// 公开 API（QML 入口，_json 版）：QML 端 JSON.stringify(["字段1","字段2",...])
// 成字符串传入，此处解析还原为 QStringList 再交给唯一实现。
// 背景：JS 数组实参在真机（Qt 6.10 qmlcache/Android）上无论走 QStringList 还是
// QVariantList 形参都会内容丢失，故统一改用“字符串传参 + JSON 编码”。
bool SqliteWarehouse::create_sqlite_wh_config_json(const QString &dbName,
                                                   const QString &fieldsJson)
{
    qInfo().noquote() << kPrefix << "create_sqlite_wh_config_json 原始入参 db=["
                      << dbName << "] json=[" << fieldsJson << "]";
    return createSqliteConfigFields(dbName, jsonToStringList(fieldsJson, "create_sqlite_wh_config"));
}

// _sep 版：字段名用 \x1F 连接成单字符串（QML 端字符串拼接产生，不经 JSON 解析），
// 此处按分隔符拆回 QStringList 再交给唯一实现。
bool SqliteWarehouse::create_sqlite_wh_config_sep(const QString &dbName,
                                                  const QString &joinedFields)
{
    qInfo().noquote() << kPrefix << "create_sqlite_wh_config_sep 原始入参 db=["
                      << dbName << "] joined=[" << joinedFields << "]";
    return createSqliteConfigFields(dbName, splitBySep(joinedFields));
}

// 唯一实现：QML 的 _json 入口与 C++ 变参模板版都转调这里
bool SqliteWarehouse::createSqliteConfigFields(const QString &dbName, const QStringList &fields)
{
    const QString name = dbName.trimmed();
    qInfo().noquote() << kPrefix << "create_sqlite_wh_config 收到字段共" << fields.size()
                      << "个: [" << fields.join(QStringLiteral("], [")) << "]";
    QString err;

    // 字段预处理：trim、去空、去重、校验非法字符
    QStringList cleaned;
    for (const QString &f : fields) {
        const QString ft = f.trimmed();
        if (ft.isEmpty()) {
            qWarning().noquote() << kPrefix << "字段名不能为空";
            return false;
        }
        if (ft.contains(QLatin1Char('"'))) {
            qWarning().noquote() << kPrefix << "字段名不能包含双引号: " << ft;
            return false;
        }
        if (cleaned.contains(ft)) {
            qWarning().noquote() << kPrefix << "字段名重复，忽略重复项: " << ft;
            continue;
        }
        cleaned.append(ft);
    }
    if (cleaned.isEmpty()) {
        qWarning().noquote() << kPrefix << "create_sqlite_wh_config 至少需要一个字段名";
        return false;
    }

    QSqlDatabase db;
    if (!databaseFor(name, db, &err)) {
        qWarning().noquote() << kPrefix << "配置参数结构失败 -" << err;
        return false;
    }
    bool created = false;
    if (!createConfigTable(db, cleaned, &created, &err)) {
        qWarning().noquote() << kPrefix << "配置参数结构失败 -" << err;
        return false;
    }
    if (created)
        qInfo().noquote() << kPrefix << "配置参数结构创建成功：库[" << name
                          << "] 字段[" << cleaned.join(QStringLiteral(", ")) << "]";
    else
        qInfo().noquote() << kPrefix << "配置参数结构已存在且一致，直接复用：库[" << name << "]";
    return true;
}

// ------------------------------------------------------------ 3. 插入数据 --
// 公开 API（QML 入口，_json 版）：见 create_sqlite_wh_config_json 的说明。
bool SqliteWarehouse::add_sqlite_wh_config_json(const QString &dbName,
                                                const QString &valuesJson)
{
    return insertSqliteValues(dbName, jsonToStringList(valuesJson, "add_sqlite_wh_config"));
}

// _sep 版：多个值用 \x1F 连接成单字符串传入（QML 端字符串拼接产生，不经 JSON 解析）
bool SqliteWarehouse::add_sqlite_wh_config_sep(const QString &dbName,
                                               const QString &joinedValues)
{
    qInfo().noquote() << kPrefix << "add_sqlite_wh_config_sep 原始入参 db=["
                      << dbName << "] joined=[" << joinedValues << "]";
    return insertSqliteValues(dbName, splitBySep(joinedValues));
}

// 唯一实现：add 的 QStringList 公开版与变参模板版都转调这里
bool SqliteWarehouse::insertSqliteValues(const QString &dbName, const QStringList &values)
{
    const QString name = dbName.trimmed();
    QString err;
    QSqlDatabase db;
    if (!databaseFor(name, db, &err)) {
        qWarning().noquote() << kPrefix << "插入失败 -" << err;
        return false;
    }

    QStringList fields;
    if (!configTableFields(db, &fields, &err) || fields.isEmpty()) {
        qWarning().noquote() << kPrefix
                             << "插入失败 - 该库尚未通过 create_sqlite_wh_config 配置参数结构";
        return false;
    }
    if (values.size() != fields.size()) {
        qWarning().noquote() << kPrefix << "插入失败 - 参数个数不匹配：库["
                             << name << "]字段有" << fields.size() << "个，本次传入"
                             << values.size() << "个";
        return false;
    }

    QString sql = QStringLiteral("INSERT INTO %1 (").arg(escId(QLatin1String(kConfigTable)));
    for (int i = 0; i < fields.size(); ++i) {
        if (i > 0)
            sql += QStringLiteral(", ");
        sql += escId(fields.at(i));
    }
    sql += QStringLiteral(") VALUES (");
    for (int i = 0; i < values.size(); ++i) {
        if (i > 0)
            sql += QStringLiteral(", ");
        sql += QStringLiteral(":v%1").arg(i);
    }
    sql += QLatin1Char(')');

    QSqlQuery q(db);
    q.prepare(sql);
    for (int i = 0; i < values.size(); ++i)
        q.bindValue(QStringLiteral(":v%1").arg(i), values.at(i));
    if (!q.exec()) {
        qWarning().noquote() << kPrefix << "插入失败 -" << q.lastError().text();
        return false;
    }
    qInfo().noquote() << kPrefix << "插入成功：库[" << name << "] 数据["
                      << values.join(QStringLiteral("]-[")) << "]";
    return true;
}

// ------------------------------------------------------------ 4. 删除 ------
// 公开 API（QML 入口，_json 版）：见 create_sqlite_wh_config_json 的说明。
bool SqliteWarehouse::delete_sqlite_wh_config_json(const QString &dbName,
                                                   const QString &valuesJson)
{
    return deleteSqliteRows(dbName, jsonToStringList(valuesJson, "delete_sqlite_wh_config"));
}

// 唯一实现：delete 的 QStringList 公开版与变参模板版都转调这里
bool SqliteWarehouse::deleteSqliteRows(const QString &dbName, const QStringList &values)
{
    const QString name = dbName.trimmed();
    QString err;
    QSqlDatabase db;
    if (!databaseFor(name, db, &err)) {
        qWarning().noquote() << kPrefix << "删除行失败 -" << err;
        return false;
    }

    QStringList fields;
    if (!configTableFields(db, &fields, &err) || fields.isEmpty()) {
        qWarning().noquote() << kPrefix
                             << "删除行失败 - 该库尚未通过 create_sqlite_wh_config 配置参数结构";
        return false;
    }
    if (values.isEmpty()) {
        qWarning().noquote() << kPrefix << "删除行失败 - 至少需要传入一个用于定位的值";
        return false;
    }
    if (values.size() > fields.size()) {
        qWarning().noquote() << kPrefix << "删除行失败 - 传入值个数(" << values.size()
                             << ")不能超过字段个数(" << fields.size() << ")";
        return false;
    }

    // 按传入顺序与字段一一等值匹配（AND），全部满足才删除
    QString sql = QStringLiteral("DELETE FROM %1 WHERE ").arg(escId(QLatin1String(kConfigTable)));
    for (int i = 0; i < values.size(); ++i) {
        if (i > 0)
            sql += QStringLiteral(" AND ");
        sql += escId(fields.at(i)) + QStringLiteral(" = :v%1").arg(i);
    }
    QSqlQuery q(db);
    q.prepare(sql);
    for (int i = 0; i < values.size(); ++i)
        q.bindValue(QStringLiteral(":v%1").arg(i), values.at(i));
    if (!q.exec()) {
        qWarning().noquote() << kPrefix << "删除行失败 -" << q.lastError().text();
        return false;
    }
    qInfo().noquote() << kPrefix << "删除行成功：库[" << name << "] 删除"
                      << q.numRowsAffected() << "行";
    return true;
}

bool SqliteWarehouse::delete_sqlite_wh_all(const QString &dbName)
{
    const QString name = dbName.trimmed();
    QString err;
    QSqlDatabase db;
    if (!databaseFor(name, db, &err)) {
        qWarning().noquote() << kPrefix << "清空数据失败 -" << err;
        return false;
    }

    QStringList fields;
    if (!configTableFields(db, &fields, &err)) {
        qWarning().noquote() << kPrefix << "清空数据失败 -" << err;
        return false;
    }
    if (fields.isEmpty()) {
        qInfo().noquote() << kPrefix << "库[" << name << "]尚未配置结构，无数据可清空";
        return true;
    }

    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("DELETE FROM %1").arg(escId(QLatin1String(kConfigTable))))) {
        qWarning().noquote() << kPrefix << "清空数据失败 -" << q.lastError().text();
        return false;
    }
    qInfo().noquote() << kPrefix << "库[" << name << "] 数据已全部清空，删除"
                      << q.numRowsAffected() << "行（结构保留）";
    return true;
}

bool SqliteWarehouse::delete_sqlite_all()
{
    // 1) 关闭并注销本次运行打开的所有连接
    for (const QString &name : std::as_const(m_openDbs)) {
        const QString conn = connectionNameFor(name);
        QSqlDatabase db = QSqlDatabase::database(conn, /*open=*/false);
        if (db.isValid() && db.isOpen())
            db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(conn);
    }
    m_openDbs.clear();

    // 2) 删除仓库目录下所有 .db 文件（含历史遗留文件）
    QDir dir(warehouseDir());
    int removed = 0;
    if (dir.exists()) {
        const QStringList files =
            dir.entryList(QStringList() << QStringLiteral("*.db"), QDir::Files);
        for (const QString &f : files) {
            if (QFile::remove(dir.filePath(f)))
                ++removed;
        }
    }
    qInfo().noquote() << kPrefix << "已删除" << removed
                      << "个数据库文件，恢复初始状态（目录:" << warehouseDir() << "）";
    return true;
}

// ------------------------------------------------------------ 5. 查找 ------
QStringList SqliteWarehouse::find_sqlite_wh_config_DS(const QString &dbName,
                                                      const QString &keyword)
{
    const QString name = dbName.trimmed();
    QStringList out;
    QString err;
    QSqlDatabase db;
    if (!databaseFor(name, db, &err)) {
        qWarning().noquote() << kPrefix << "动态查找失败 -" << err;
        return out;
    }
    QStringList fields;
    if (!configTableFields(db, &fields, &err) || fields.isEmpty()) {
        qWarning().noquote() << kPrefix
                             << "动态查找失败 - 该库尚未通过 create_sqlite_wh_config 配置参数结构";
        return out;
    }

    const QString kw = keyword.trimmed();
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT * FROM %1").arg(escId(QLatin1String(kConfigTable))))) {
        qWarning().noquote() << kPrefix << "动态查找失败 -" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        bool hit = false;
        for (const QString &f : fields) {                       // 字段名含关键字
            if (f.contains(kw, Qt::CaseInsensitive)) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            for (int i = 0; i < fields.size(); ++i) {           // 某个值含关键字
                if (q.value(i).toString().contains(kw, Qt::CaseInsensitive)) {
                    hit = true;
                    break;
                }
            }
        }
        if (!hit)
            continue;
        QStringList vals;
        for (int i = 0; i < fields.size(); ++i)
            vals.append(q.value(i).toString());
        const QString line = formatRow(name, vals);
        out.append(line);
        qDebug().noquote() << line;
    }
    qInfo().noquote() << kPrefix << "库[" << name << "] 动态查找关键字["
                      << kw << "] 命中" << out.size() << "条";
    return out;
}

QStringList SqliteWarehouse::find_sqlite_wh_config_EM(const QString &dbName,
                                                      const QString &fieldName,
                                                      const QString &value)
{
    const QString name = dbName.trimmed();
    const QString field = fieldName.trimmed();
    QStringList out;
    QString err;
    QSqlDatabase db;
    if (!databaseFor(name, db, &err)) {
        qWarning().noquote() << kPrefix << "精确查找失败 -" << err;
        return out;
    }
    QStringList fields;
    if (!configTableFields(db, &fields, &err) || fields.isEmpty()) {
        qWarning().noquote() << kPrefix
                             << "精确查找失败 - 该库尚未通过 create_sqlite_wh_config 配置参数结构";
        return out;
    }
    if (field.isEmpty() || !fields.contains(field)) {
        qWarning().noquote() << kPrefix << "精确查找失败 - 字段["
                             << field << "]不存在，可用字段为["
                             << fields.join(QStringLiteral(", ")) << "]";
        return out;
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT * FROM %1 WHERE %2 = :v")
                  .arg(escId(QLatin1String(kConfigTable)), escId(field)));
    q.bindValue(QStringLiteral(":v"), value);
    if (!q.exec()) {
        qWarning().noquote() << kPrefix << "精确查找失败 -" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        QStringList vals;
        for (int i = 0; i < fields.size(); ++i)
            vals.append(q.value(i).toString());
        const QString line = formatRow(name, vals);
        out.append(line);
        qDebug().noquote() << line;
    }
    qInfo().noquote() << kPrefix << "库[" << name << "] 精确查找 字段["
                      << field << "] = [" << value << "] 命中" << out.size() << "条";
    return out;
}

// ------------------------------------------------------------ 6. 修改 ------
bool SqliteWarehouse::change_sqlite_wh_config(const QString &dbName, const QString &fieldName,
                                              const QString &oldValue, const QString &newValue)
{
    const QString name = dbName.trimmed();
    const QString field = fieldName.trimmed();
    QString err;
    QSqlDatabase db;
    if (!databaseFor(name, db, &err)) {
        qWarning().noquote() << kPrefix << "修改失败 -" << err;
        return false;
    }
    QStringList fields;
    if (!configTableFields(db, &fields, &err) || fields.isEmpty()) {
        qWarning().noquote() << kPrefix
                             << "修改失败 - 该库尚未通过 create_sqlite_wh_config 配置参数结构";
        return false;
    }
    if (!fields.contains(field)) {
        qWarning().noquote() << kPrefix << "修改失败 - 字段[" << field
                             << "]不存在，可用字段为[" << fields.join(QStringLiteral(", ")) << "]";
        return false;
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE %1 SET %2 = :new WHERE %2 = :old")
                  .arg(escId(QLatin1String(kConfigTable)), escId(field)));
    q.bindValue(QStringLiteral(":new"), newValue);
    q.bindValue(QStringLiteral(":old"), oldValue);
    if (!q.exec()) {
        qWarning().noquote() << kPrefix << "修改失败 -" << q.lastError().text();
        return false;
    }
    qInfo().noquote() << kPrefix << "修改成功：库[" << name << "] 字段["
                      << field << "] 将 [" << oldValue << "] 改为 [" << newValue
                      << "]，共更新" << q.numRowsAffected() << "行";
    return true;
}

// ------------------------------------------------------------ 7. 附加只读 --
bool SqliteWarehouse::sqlite_wh_exists(const QString &dbName) const
{
    const QString name = dbName.trimmed();
    if (name.isEmpty() || !nameIsValid(name, nullptr))
        return false;
    return m_openDbs.contains(name) || QFile::exists(filePathFor(name));
}

QStringList SqliteWarehouse::sqlite_wh_warehouses() const
{
    QDir dir(warehouseDir());
    QStringList names;
    if (!dir.exists())
        return names;
    const QStringList files =
        dir.entryList(QStringList() << QStringLiteral("*.db"), QDir::Files);
    for (const QString &f : files) {
        QString n = f;
        if (n.endsWith(QStringLiteral(".db")))
            n.chop(3);
        names.append(n);
    }
    names.sort();
    return names;
}

QStringList SqliteWarehouse::sqlite_wh_fields(const QString &dbName)
{
    const QString name = dbName.trimmed();
    QStringList out;
    QString err;
    QSqlDatabase db;
    if (!databaseFor(name, db, &err)) {
        qWarning().noquote() << kPrefix << "读取字段失败 -" << err;
        return out;
    }
    if (!configTableFields(db, &out, &err)) {
        qWarning().noquote() << kPrefix << "读取字段失败 -" << err;
        return out;
    }
    return out;
}

QStringList SqliteWarehouse::sqlite_wh_all(const QString &dbName)
{
    const QString name = dbName.trimmed();
    QStringList out;
    QString err;
    QSqlDatabase db;
    if (!databaseFor(name, db, &err)) {
        qWarning().noquote() << kPrefix << "读取全部数据失败 -" << err;
        return out;
    }
    QStringList fields;
    if (!configTableFields(db, &fields, &err)) {
        qWarning().noquote() << kPrefix << "读取全部数据失败 -" << err;
        return out;
    }
    if (fields.isEmpty())
        return out;

    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT * FROM %1").arg(escId(QLatin1String(kConfigTable))))) {
        qWarning().noquote() << kPrefix << "读取全部数据失败 -" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        QStringList vals;
        for (int i = 0; i < fields.size(); ++i)
            vals.append(q.value(i).toString());
        out.append(formatRow(name, vals));
    }
    qInfo().noquote() << kPrefix << "库[" << name << "] 共" << out.size() << "行数据";
    return out;
}

QVariantList SqliteWarehouse::sqlite_wh_records(const QString &dbName)
{
    const QString name = dbName.trimmed();
    QVariantList out;
    QString err;
    QSqlDatabase db;
    if (!databaseFor(name, db, &err)) {
        qWarning().noquote() << kPrefix << "读取结构化数据失败 -" << err;
        return out;
    }
    QStringList fields;
    if (!configTableFields(db, &fields, &err)) {
        qWarning().noquote() << kPrefix << "读取结构化数据失败 -" << err;
        return out;
    }
    if (fields.isEmpty())
        return out;

    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT * FROM %1").arg(escId(QLatin1String(kConfigTable))))) {
        qWarning().noquote() << kPrefix << "读取结构化数据失败 -" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        QVariantMap row;
        for (int i = 0; i < fields.size(); ++i)
            row.insert(fields.at(i), q.value(i).toString());
        out.append(row);
    }
    return out;
}
