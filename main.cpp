#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>

#include <QDir>
#include <QDateTime>
#include <QDebug>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>

#include "ble/blemanager.h"
#include "ble/blepermissions.h"

#ifdef Q_OS_ANDROID
#include <android/log.h>
#endif

namespace {

// 应用日志文件：Android 上位于
//   /sdcard/Android/data/<packageName>/files/qt_log.txt
// 可用 `adb pull /sdcard/Android/data/org.qtproject.example.appBLE_SAR/files/qt_log.txt`
// 导出，或直接在手机上用文件管理器打开查看。
QFile *g_logFile = nullptr;
QMutex g_logMutex;

void installFileLog(const QString &dirPath)
{
    QDir().mkpath(dirPath);
    const QString path = dirPath + QStringLiteral("/qt_log.txt");
    g_logFile = new QFile(path);
    if (g_logFile->open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(g_logFile);
        ts << "\n================ 应用启动 "
           << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz")
           << " ================\n";
        ts.flush();
    } else {
        delete g_logFile;
        g_logFile = nullptr;
    }
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context);

    // 1) 输出到 logcat（tag=Qt）/ 控制台：
    //    Qt Creator 中需在「项目 → 运行 → 勾选 Enable Qt output」才能看到；
    //    也可用 `adb logcat -s Qt` 查看。
#ifdef Q_OS_ANDROID
    const char *tag = "Qt";
    switch (type) {
    case QtDebugMsg:    __android_log_print(ANDROID_LOG_DEBUG, tag, "%s", msg.toUtf8().constData()); break;
    case QtInfoMsg:     __android_log_print(ANDROID_LOG_INFO,  tag, "%s", msg.toUtf8().constData()); break;
    case QtWarningMsg:  __android_log_print(ANDROID_LOG_WARN,  tag, "%s", msg.toUtf8().constData()); break;
    case QtCriticalMsg: __android_log_print(ANDROID_LOG_ERROR, tag, "%s", msg.toUtf8().constData()); break;
    case QtFatalMsg:    __android_log_print(ANDROID_LOG_FATAL, tag, "%s", msg.toUtf8().constData()); break;
    }
#else
    fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
    fflush(stderr);
#endif

    // 2) 追加写入日志文件（闪退/退出前的内容也能保留下来）
    QMutexLocker locker(&g_logMutex);
    if (g_logFile && g_logFile->isOpen()) {
        QTextStream ts(g_logFile);
        ts << QDateTime::currentDateTime().toString("hh:mm:ss.zzz") << " ";
        switch (type) {
        case QtDebugMsg:    ts << "D"; break;
        case QtInfoMsg:     ts << "I"; break;
        case QtWarningMsg:  ts << "W"; break;
        case QtCriticalMsg: ts << "E"; break;
        case QtFatalMsg:    ts << "F"; break;
        }
        ts << "| " << msg << "\n";
        ts.flush();
    }
}

} // namespace

int main(int argc, char *argv[])
{
    // 使用 Fusion（非原生）样式：支持自定义 Button 的 background/contentItem，
    // 否则在 Windows 原生样式下自定义深色按钮会被忽略并输出大量警告。
    qputenv("QT_QUICK_CONTROLS_STYLE", "Fusion");

    QGuiApplication app(argc, argv);

    // 安装日志处理器：日志同时输出到 logcat 与文件（见文件头注释），
    // 便于排查「首次启动闪退、界面看不到调试信息」这类问题。
    qInstallMessageHandler(messageHandler);
    installFileLog(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    qInfo() << "==== BLE-SAR 启动 ====";
    qInfo() << "应用数据目录:" << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    // BLE 功能后端：扫描 / 广播 / GATT 连接（模块化，见 ble/ 目录）
    // QML 中通过全局对象 bleManager 访问，例如：
    //   bleManager.startScan();                                    // 开始扫描
    //   bleManager.stopScan();                                     // 停止扫描
    //   bleManager.connectToDevice(address);                       // 连接设备
    //   bleManager.startAdvertise("BLE SAR", serviceUuid, 100);    // 开始广播
    //   bleManager.stopAdvertise();                                // 停止广播
    BleManager bleManager;
    qInfo() << "BLE 后端初始化完成";

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("bleManager", &bleManager);
    // 注册权限枚举类型，QML 中可用 bleManager.permissions.requestPermission(
    // BlePermissions.ConnectPermission) 等方式请求权限
    qmlRegisterType<BlePermissions>("BLE_SAR.Ble", 1, 0, "BlePermissions");

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() {
            // 记录失败原因到日志文件（QML 引擎的具体错误会随上面的
            // qWarning/qCritical 一并写入），再退出，便于查看 qt_log.txt 定位。
            qCritical() << "QML 对象创建失败，应用即将退出（详见上方 QML 错误日志）";
            QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);
    engine.loadFromModule("BLE_SAR", "Main");
    qInfo() << "QML 加载请求已发出";

    const int ret = app.exec();
    qInfo() << "事件循环退出，main 返回" << ret;
    return ret;
}
