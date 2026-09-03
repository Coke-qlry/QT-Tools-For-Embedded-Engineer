#ifndef SQLITE_H
#define SQLITE_H

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QSet>
#include <type_traits>
#include <utility>

// ============================================================================
// SqliteWarehouse —— 通用、可复用的 SQLite「配置仓库」封装
//
// 设计模型（简单直观，一个“库” = 一个 .db 文件 + 一张配置表）：
//   1) 先 create_sqlite_warehouse("库名")            → 生成 库名.db 文件
//   2) 再 create_sqlite_wh_config("库名", 参数名...) → 定义这张表的字段
//      （参数可任意多个、任意内容，例如蓝牙指令的名称/注释/内容…）
//   3) 之后 add / delete / find / change 都围绕这张表进行
//
// 【C++ 变参调用示例】（参数个数不受限制，按 create 时字段顺序一一对应）
//   create_sqlite_warehouse("ble_command_config");
//   create_sqlite_wh_config("ble_command_config", "command_name", "command_self");
//   add_sqlite_wh_config("ble_command_config", "读取电量", "AT+VBAT?");
//   delete_sqlite_wh_config("ble_command_config", "读取电量", "AT+VBAT?");  // 删某一行
//   delete_sqlite_wh_all("ble_command_config");      // 清空该库所有数据(保留结构)
//   delete_sqlite_all();                             // 删除所有已创建库 → 恢复初始
//   QStringList r1 = find_sqlite_wh_config_DS("ble_command_config", "电量"); // 动态/模糊查找
//   QStringList r2 = find_sqlite_wh_config_EM("ble_command_config",
//                                             "command_name", "读取电量");   // 精确查找
//   change_sqlite_wh_config("ble_command_config", "command_self",
//                           "AT+VBAT?", "AT+VBAT?\r\n");                     // 修改值
//
// 【QML 调用示例】QML 无法传“任意数量参数”，多值统一编码进一个字符串传入。
// 推荐使用 _sep 系列（分隔符 = \x1F，指令文本几乎不可能含该字符）：
//   sqliteWarehouse.create_sqlite_wh_config_sep(
//       "ble_command_config", "command_name\u001Fcommand_self");
//   sqliteWarehouse.add_sqlite_wh_config_sep(
//       "ble_command_config", "读取电量\u001FAT+VBAT?");
//   var lines = sqliteWarehouse.find_sqlite_wh_config_DS("ble_command_config", "电量");
//   var rows  = sqliteWarehouse.sqlite_wh_records("ble_command_config");
// 说明：真机实测(Qt 6.10 qmlcache/Android)两种“数组类”传参都不可靠：
//   * JS 数组实参经 QStringList / QVariantList 形参进 C++ 时内容丢失（收到单个空串）；
//   * JSON.stringify([...]) 的求值结果在部分机型/缓存下也会被算错（收到 ["…"] 变 [""]）。
// 而普通字符串实参（如库名）各环境均可靠，故这里优先暴露 _sep 分隔符版，
// 多值在 QML 端仅用字符串拼接产生（不带任何 JSON 解析），C++ 端按分隔符拆回。
// _json 版（JSON.stringify 编码）仍保留，供确实需要 JSON 语义的调用方使用。
//
// 说明：
//   * 数据库文件保存在 warehouseDir()（Android/iOS 为应用数据目录，
//     桌面开发机默认可执行文件旁的 sqlite_warehouse/ 目录），可用
//     SqliteWarehouse::setWarehouseDir() 在启动时修改。
//   * 未先 create_sqlite_warehouse 就使用该库的其它接口时，会在
//     qDebug/qWarning 输出「请先创建数据库」报错并返回失败。
//   * 所有接口都是增删改查的“整值文本”操作，任何数据类型都会先转为
//     QString 存储；所有值均通过 SQL 参数绑定写入，无注入问题。
// ============================================================================

class SqliteWarehouse : public QObject
{
    Q_OBJECT

public:
    explicit SqliteWarehouse(QObject *parent = nullptr);
    ~SqliteWarehouse() override;

    // ---- 全局路径配置（默认即可用，可选修改） ----
    static void setWarehouseDir(const QString &dir);   // 设置 .db 存放目录
    static QString warehouseDir();                     // 当前 .db 存放目录

    // ---------- 1. 创建数据库（若已存在则直接复用，不会重复创建） ----------
    Q_INVOKABLE bool create_sqlite_warehouse(const QString &dbName);

    // ---------- 2. 配置参数结构（动态字段个数） ----------
    // QML 入口 A（_sep 版，推荐）：多个字段名用 \x1F 连接成单字符串传入，
    // 避开 qmlcache 真机上 JS 数组 / JSON.stringify 内容错乱的兼容性问题。
    Q_INVOKABLE bool create_sqlite_wh_config_sep(const QString &dbName,
                                                 const QString &joinedFields);
    // QML 入口 B（_json 版）：字段数组先用 JSON.stringify 编码成字符串传入，
    // 解析后交给 createSqliteConfigFields。
    Q_INVOKABLE bool create_sqlite_wh_config_json(const QString &dbName,
                                                  const QString &fieldsJson);
    // C++ 变参模板版：create_sqlite_wh_config("库名", "字段1", "字段2", ...)
    // 注意：模板内部绝不能以同名非限定形式转发（如 create_sqlite_wh_config(dbName, fields)），
    // 否则与自身重载决议会选中模板自身造成无限递归（真机上曾因此栈溢出），
    // 统一转调私有唯一实现 createSqliteConfigFields。
    template <typename... Args>
    bool create_sqlite_wh_config(const QString &dbName, Args &&...args)
    {
        QStringList fields;
        (fields.append(sqliteValueToString(std::forward<Args>(args))), ...);
        return createSqliteConfigFields(dbName, fields);
    }

    // ---------- 3. 插入数据（值的个数/顺序必须与 create 字段一致） ----------
    // QML 入口 A（_sep 版，推荐）：多个值用 \x1F 连接成单字符串传入。
    Q_INVOKABLE bool add_sqlite_wh_config_sep(const QString &dbName,
                                              const QString &joinedValues);
    // QML 入口 B（_json 版）：JSON.stringify 编码成字符串传入。
    Q_INVOKABLE bool add_sqlite_wh_config_json(const QString &dbName,
                                               const QString &valuesJson);
    template <typename... Args>
    bool add_sqlite_wh_config(const QString &dbName, Args &&...args)
    {
        QStringList values;
        (values.append(sqliteValueToString(std::forward<Args>(args))), ...);
        return insertSqliteValues(dbName, values);
    }

    // ---------- 4. 删除（三个级别） ----------
    // 删除某一行/若干行：传入的值按字段顺序等值匹配（都满足才删）。
    //   例：delete_sqlite_wh_config("库名", "读取电量", "AT+VBAT?")
    Q_INVOKABLE bool delete_sqlite_wh_config_json(const QString &dbName,
                                                  const QString &valuesJson);
    template <typename... Args>
    bool delete_sqlite_wh_config(const QString &dbName, Args &&...args)
    {
        QStringList values;
        (values.append(sqliteValueToString(std::forward<Args>(args))), ...);
        return deleteSqliteRows(dbName, values);
    }
    // 清空指定库的全部数据（保留库与结构，可继续 add）
    Q_INVOKABLE bool delete_sqlite_wh_all(const QString &dbName);
    // 删除所有已创建的数据文件与连接 → 完全恢复初始状态
    Q_INVOKABLE bool delete_sqlite_all();

    // ---------- 5. 查找 ----------
    // DS = Dynamic Search 动态/模糊查找：关键字只要与「某字段名」或
    // 「某行的某个值」完全一致或包含其片段，就整行命中并打印/返回，
    // 格式为 [库名]-[值1]-[值2]-...，例如 [ble_command_config]-[2026]-[09031944]
    Q_INVOKABLE QStringList find_sqlite_wh_config_DS(const QString &dbName,
                                                     const QString &keyword);
    // EM = Exact Match 精确查找：指定“字段名 + 值”，只有该字段完全等于该值的行命中
    Q_INVOKABLE QStringList find_sqlite_wh_config_EM(const QString &dbName,
                                                     const QString &fieldName,
                                                     const QString &value);

    // ---------- 6. 修改 ----------
    // 把某字段下所有等于 oldValue 的值改为 newValue（先定位再替换）
    Q_INVOKABLE bool change_sqlite_wh_config(const QString &dbName,
                                             const QString &fieldName,
                                             const QString &oldValue,
                                             const QString &newValue);

    // ---------- 7. 附加常用只读接口（方便取数展示/调试） ----------
    Q_INVOKABLE bool sqlite_wh_exists(const QString &dbName) const;       // 库是否已创建
    Q_INVOKABLE QStringList sqlite_wh_warehouses() const;                // 所有已创建库名
    Q_INVOKABLE QStringList sqlite_wh_fields(const QString &dbName);     // 该库的字段(参数名)列表
    Q_INVOKABLE QStringList sqlite_wh_all(const QString &dbName);        // 该库全部数据行([库名]-...)
    Q_INVOKABLE QVariantList sqlite_wh_records(const QString &dbName);   // 结构化结果 [{字段:值},...]

private:
    // 任意参数 → QString（char* 按 UTF-8 解码，QString 直接使用，其它经 QVariant 转换）
    template <typename T>
    static QString sqliteValueToString(const T &v)
    {
        if constexpr (std::is_same_v<std::decay_t<T>, const char *>
                      || std::is_same_v<std::decay_t<T>, char *>
                      || std::is_same_v<std::decay_t<T>, QByteArray>) {
            return QString::fromUtf8(v);
        } else if constexpr (std::is_convertible_v<T, QString>) {
            return QString(v);
        } else {
            return QVariant::fromValue(v).toString();
        }
    }

    // 供公开 QStringList 版与变参模板版共同转调的唯一实现（避免同名重载自递归）
    bool createSqliteConfigFields(const QString &dbName, const QStringList &fields);
    bool insertSqliteValues(const QString &dbName, const QStringList &values);
    bool deleteSqliteRows(const QString &dbName, const QStringList &values);

    QString filePathFor(const QString &dbName) const;        // 库名 → .db 文件完整路径
    QString connectionNameFor(const QString &dbName) const;  // 库名 → Qt 连接名
    bool nameIsValid(const QString &dbName, QString *err) const;
    bool openWarehouse(const QString &dbName, QString *err) const;      // 打开连接（文件须已存在或允许创建）
    bool databaseFor(const QString &dbName, QSqlDatabase &db,           // 校验“已创建”后取可用连接
                     QString *err) const;
    bool configTableFields(const QSqlDatabase &db, QStringList *fields, // PRAGMA 读取当前字段
                           QString *err) const;
    bool createConfigTable(const QSqlDatabase &db, const QStringList &fields,
                           bool *created, QString *err) const;               // 建表(已存在则校验一致)
    QString escId(const QString &identifier) const;                      // "a" → "a" 双引号转义
    QString formatRow(const QString &dbName, const QStringList &values) const; // [库]-[v1]-[v2]...
    bool ensureDirectory() const;                                        // 确保仓库目录存在

    mutable QSet<QString> m_openDbs;   // 本次运行已建立连接的库名（连接名见 connectionNameFor）
    inline static const char *const kConfigTable = "config";  // 每个库内唯一的数据表名
};

#endif // SQLITE_H
