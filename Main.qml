import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import BLE_SAR.Ble

// =====================================================================
// BLE_SAR - 仿 nRF Connect 界面（中文差异化版本）
// 界面通过全局对象 bleManager 调用 C++ 后端，实现扫描 / 广播 / 连接。
// 权限通过 bleManager.permissions（BlePermissions）在 Android 上动态申请。
//
// 页面架构：本文件仅保留「外壳」——主题色、跨页共享状态、设备模型、
// 共享逻辑函数、底部导航与蓝牙信号对接；各功能页面按界面分类拆分为
// 独立 QML 组件（模块 BLE_SAR 内自动注册类型），通过 app 句柄访问共享部分：
//   - ScanPage.qml      扫描页（含过滤对话框与过滤逻辑）
//   - AdvertisePage.qml 广播页（含广播参数输入）
//   - TerminalPage.qml  调试页（含服务/特征目录、导出/定时对话框）
//   - CommandPage.qml   指令页（含指令数据模型与增删改查）
//   - AboutPage.qml     关于页
// 后续改动页面时只需修改对应文件；改动共享逻辑时修改本文件。
// =====================================================================

Window {
    id: root
    width: 440
    height: 860
    minimumWidth: 380
    minimumHeight: 640
    visible: true
    title: qsTr("BLE SAR")

    // ---------------- 主题色 ----------------
    readonly property color cBg:        "#0F1116"   // 窗口背景
    readonly property color cSurface:   "#1A1D24"   // 卡片/列表背景
    readonly property color cSurface2:  "#232733"   // 悬浮/按压背景
    readonly property color cAccent:    "#00A9CE"   // Nordic 青色
    readonly property color cAccentDark:"#00849E"
    readonly property color cText:      "#E8EBF0"
    readonly property color cSubtext:   "#8A93A3"
    readonly property color cDivider:   "#2A2E38"
    readonly property color cGreen:     "#2ECC71"
    readonly property color cRed:       "#E74C3C"
    readonly property color cOrange:    "#F39C12"

    // ---------------- 状态 ----------------
    property int currentTab: 0          // 0=扫描 1=广播 2=调试 3=指令 4=关于
    property bool scanning: false       // 由 bleManager.scanningChanged 驱动
    property bool advertising: false    // 由 bleManager.advertisingChanged 驱动
    property bool peripheralConnected: false // 外设被其它设备连接状态
    property bool sortByName: false     // true=按名称排序，false=按RSSI排序
    property string targetAddress: ""   // 当前连接目标设备地址
    property string connectingAddress: "" // 正在连接中(尚未成功)的设备地址，空=当前无进行中的连接
    property var pendingAction: null    // 权限授权成功后待执行的动作
    // 连接失败后短暂抑制后续补充信号的时刻戳(毫秒)：Android 上连接失败可能
    // errorOccurred 与 disconnected 先后到达，避免弹两次提示
    property var suppressErrUntil: 0

    // ---- 扫描过滤条件（ScanPage 读写，本文件提供状态与重建逻辑）----
    property int filterMinRssi: -127    // 最小 RSSI 阈值（-127 = 不过滤）
    property string filterAddress: ""   // MAC 地址包含关键字
    property string filterName: ""      // 蓝牙名称包含关键字
    // 任一过滤条件生效即为过滤状态（驱动过滤提示条显示）
    property bool filterActive: filterMinRssi > -127
                              || filterAddress !== ""
                              || filterName !== ""
    // 过滤条件摘要（用于过滤提示条展示）
    readonly property string filterSummaryText: {
        var parts = []
        if (root.filterMinRssi > -127) parts.push("RSSI>" + root.filterMinRssi)
        if (root.filterAddress !== "") parts.push("MAC:" + root.filterAddress)
        if (root.filterName !== "") parts.push("名称:" + root.filterName)
        return parts.join(" · ")
    }

    // ---------------- 启动自动扫描 ----------------
    // 打开程序进入界面后，蓝牙扫描权限就绪即自动扫描一次：
    // 已有权限 → 直接扫描；未授权 → 自动弹窗申请，授权成功后立即开始扫描
    Component.onCompleted: {
        // 自定义指令库：启动即准备 ble_command_config（首次运行时自动建库
        // 并写入出厂默认指令；之后每次启动加载用户上一次保存的配置）
        commandPage.initFromSqlite()
        autoScanTimer.start()
        // 页面就绪后刷新一次系统蓝牙名称：已有权限时立即显示真实名称，
        // 未授权时保持占位名，权限授予后由 onPermissionGranted 再次刷新。
        // （避免在 app 启动早期、权限弹窗前执行蓝牙 JNI，防止首次安装闪退）
        bleManager.refreshLocalDeviceName()
    }
    Timer {
        id: autoScanTimer
        interval: 300
        onTriggered: root.startScan()
    }
    // 连接超时兜底：15 秒内既未成功也未收到失败通知时提示并复位，
    // 防止异常情况下按钮一直停在黄色“连接中”
    Timer {
        id: connectTimeoutTimer
        interval: 15000
        onTriggered: root.failConnect(root.connectingAddress,
            "连接超时：请确认设备已开机且靠近本机，未被其它手机占用，然后重试")
    }

    // =================================================================
    // 共享逻辑函数（各页面通过 app 句柄调用）
    // =================================================================

    // ---- 权限辅助 ----
    // 发起操作前检查对应权限；未授权则动态申请，申请成功后再执行动作
    function ensurePermission(perm, action) {
        if (bleManager.hasPermission(perm)) { action(); return }
        // 保存待执行动作，收到 permissionGranted 后执行
        root.pendingAction = action
        bleManager.requestPermission(perm)
    }

    // ---- 扫描 ----
    function startScan() {
        ensurePermission(BlePermissions.ScanPermission, function () {
            deviceModel.clear()
            viewModel.clear()
            bleManager.startScan(10000)
        })
    }
    function stopScan() {
        bleManager.stopScan()
    }
    function clearDevices() {
        deviceModel.clear()
        viewModel.clear()
        bleManager.clearDevices()
    }
    // 按名称 或 RSSI 排序（名称优先，空名称放最后），作用于显示模型
    function sortDevices() {
        root.sortByName = !root.sortByName
        root.rebuildViewModel()
        root.showToast(root.sortByName ? "已按名称排序" : "已按信号强度排序")
    }
    // 通过地址连接/断开设备（列表经过滤/排序后索引不再可靠，改用地址）
    function connectToDeviceByAddress(address) {
        if (!address) return
        var it = null
        for (var i = 0; i < deviceModel.count; i++) {
            if (deviceModel.get(i).address === address) { it = deviceModel.get(i); break }
        }
        if (!it) return
        if (it.connected) { bleManager.disconnectFromDevice(); return }
        if (it.connecting) return                      // 该设备已在连接中，忽略重复点击
        if (root.connectingAddress !== "") {           // 已有其它设备正在连接
            root.showToast("请等待当前设备连接完成")
            return
        }
        // 进入「连接中」状态：按钮变黄色，直到成功 / 失败 / 超时
        root.targetAddress = address
        root.connectingAddress = address
        root.setDeviceConnecting(address, true)
        connectTimeoutTimer.restart()                  // 超时兜底，防止一直“连接中”
        ensurePermission(BlePermissions.ConnectPermission, function () {
            bleManager.connectToDevice(root.targetAddress)
        })
    }

    // ---- 过滤 ----
    // 设备项是否满足当前过滤条件
    function deviceMatches(it) {
        if (root.filterMinRssi > -127 && it.rssi < root.filterMinRssi) return false
        if (root.filterAddress !== ""
            && it.address.toLowerCase().indexOf(root.filterAddress.toLowerCase()) < 0)
            return false
        if (root.filterName !== ""
            && (it.name === "N/A"
                || it.name.toLowerCase().indexOf(root.filterName.toLowerCase()) < 0))
            return false
        return true
    }
    // 从原始设备模型重建显示模型（过滤 + 排序）
    function rebuildViewModel() {
        var arr = []
        for (var i = 0; i < deviceModel.count; i++) {
            var it = deviceModel.get(i)
            if (root.deviceMatches(it)) arr.push(it)
        }
        arr.sort(function (a, b) {
            if (root.sortByName) {
                // N/A 视为空名称，排序时排最后
                var na = (a.name === "N/A" ? "" : (a.name || "")).toLowerCase()
                var nb = (b.name === "N/A" ? "" : (b.name || "")).toLowerCase()
                if (na === nb) return b.rssi - a.rssi
                if (na === "") return 1
                if (nb === "") return -1
                return na < nb ? -1 : (na > nb ? 1 : 0)
            }
            return b.rssi - a.rssi   // 按 RSSI 降序（信号强在前）
        })
        viewModel.clear()
        for (var j = 0; j < arr.length; j++) viewModel.append(arr[j])
    }

    // ---- 连接状态同步（ScanPage 展示）----
    // 连接状态变化时同步原始模型与显示模型（同时复位连接中标记）
    function updateDeviceConnected(address, v) {
        for (var i = 0; i < deviceModel.count; i++) {
            if (deviceModel.get(i).address === address)
                deviceModel.set(i, { connected: v, connecting: false })
        }
        for (var j = 0; j < viewModel.count; j++) {
            if (viewModel.get(j).address === address)
                viewModel.set(j, { connected: v, connecting: false })
        }
    }
    // 标记某设备是否处于「连接中」状态（同步两个模型，驱动按钮变黄色）
    function setDeviceConnecting(address, v) {
        for (var i = 0; i < deviceModel.count; i++) {
            if (deviceModel.get(i).address === address)
                deviceModel.set(i, { connecting: v })
        }
        for (var j = 0; j < viewModel.count; j++) {
            if (viewModel.get(j).address === address)
                viewModel.set(j, { connecting: v })
        }
    }
    // 复位「连接中」状态（当前无进行中的连接时忽略）
    function clearConnecting() {
        if (root.connectingAddress === "") return
        root.setDeviceConnecting(root.connectingAddress, false)
        root.connectingAddress = ""
        connectTimeoutTimer.stop()
    }
    // 连接失败统一收口：复位连接中状态并提示，随后短暂抑制
    // 同一次失败触发的补充信号（errorOccurred / disconnected 先后到达）
    function failConnect(address, reason) {
        if (root.connectingAddress === "") return
        var addr = (address && address !== "") ? address : root.connectingAddress
        if (addr !== root.connectingAddress) return     // 与当前连接目标不一致，忽略
        root.setDeviceConnecting(addr, false)
        root.updateDeviceConnected(addr, false)
        root.connectingAddress = ""
        connectTimeoutTimer.stop()
        root.suppressErrUntil = Date.now() + 2500
        root.showToast(reason)
    }

    // ---- 连接/收发日志（打印到指令页日志文本框，供连接后观察）----
    // Central 连接成功：绿字信息块 + 设备信息（名称/MAC/信号强度取扫描缓存）
    function logConnected() {
        var nm = "N/A", rssi = "?"
        for (var i = 0; i < deviceModel.count; i++) {
            var it = deviceModel.get(i)
            if (it.address === root.targetAddress) {
                if (it.name !== "" && it.name !== "N/A") nm = it.name
                rssi = it.rssi
                break
            }
        }
        commandPage.appendLog("[BLE-SAR]:已连接到蓝牙[" + nm + "]，相关信息如下:",
                              root.cGreen)
        commandPage.appendLog("[" + nm + "]: MAC地址 " + root.targetAddress
                              + " · 信号强度 " + rssi + " dBm", root.cAccent)
    }
    // 服务发现完成：把各服务 UUID 补充打印到设备信息区
    function logServices(services) {
        var list = (typeof services === "object" && services) ? services : []
        if (list.length === 0) return
        commandPage.appendLog("[BLE-SAR]:已发现 " + list.length
                              + " 个服务，UUID 如下:", root.cAccent)
        for (var i = 0; i < list.length; i++) {
            var svc = list[i]
            commandPage.appendLog("  • " + svc.uuid
                                  + ((svc.name && svc.name !== "")
                                     ? " (" + svc.name + ")" : ""), root.cSubtext)
        }
    }
    // Central 断开连接日志
    function logDisconnected() {
        commandPage.appendLog("[BLE-SAR]:已断开与[" + root.targetAddress + "]的连接",
                              root.cOrange)
    }
    // 外设（Peripheral）被对方连接 / 断开日志
    function logPeripheral(v) {
        if (v) {
            commandPage.appendLog("[BLE-SAR]:已连接到蓝牙["
                                  + bleManager.localDeviceName + "]，相关信息如下:",
                                  root.cGreen)
            commandPage.appendLog("[" + bleManager.localDeviceName
                                  + "]: 外设模式 · 等待对端设备写入数据", root.cAccent)
        } else {
            commandPage.appendLog("[BLE-SAR]:对方设备已断开连接", root.cOrange)
        }
    }

    // ---- 轻提示（全局 Toast）----
    function showToast(msg) {
        toastText.text = msg
        toast.visible = true
        toastTimer.restart()
    }

    // RSSI 转 0~4 格信号
    function rssiLevel(rssi) {
        if (rssi >= -55) return 4
        if (rssi >= -65) return 3
        if (rssi >= -75) return 2
        if (rssi >= -85) return 1
        return 0
    }
    // 信号图标点亮格数：极弱(< -85)也至少点亮 1 格(红色)，
    // 让用户直观看到“有信号但很弱”，而不是整组变灰
    function rssiBars(rssi) {
        var lv = rssiLevel(rssi)
        return lv > 0 ? lv : 1
    }
    // 信号强度等级颜色：强(>= -65)=绿，中(-85~-65)=黄，弱(<-85)=红
    function rssiColor(rssi) {
        if (rssi >= -65) return root.cGreen
        if (rssi >= -85) return root.cOrange
        return root.cRed
    }

    // =================================================================
    // 设备列表模型（由 C++ 后端扫描结果填充）
    //   deviceModel: 原始数据（扫描填充，不随排序/过滤改变）
    //   viewModel  : 显示数据（过滤 + 排序后的子集，ScanPage 展示）
    // 字段: name / address / rssi / connected / connecting / isConnectable
    //   connecting 表示“正在连接(尚未成功)”，驱动按钮变黄色“连接中”
    // =================================================================
    ListModel { id: modelDevice }
    ListModel { id: modelView }
    // 模型别名：供子页面经 app.deviceModel / app.viewModel 访问，
    // 页面内仍可逐字沿用拆分前的 id 引用写法，行为保持一致
    property alias deviceModel: modelDevice
    property alias viewModel: modelView

    // ---------------- 全局背景 ----------------
    Rectangle {
        anchors.fill: parent
        color: root.cBg
    }

    // =================================================================
    // 主界面：五个独立页面组件 + 底部导航
    // 各页面组件内部自带独立顶栏/对话框/专属逻辑，经 app 句柄
    // 访问本文件的主题色、模型、共享函数与 Toast。
    // =================================================================
    Item {
        id: mainView
        anchors.fill: parent

        ScanPage {
            id: scanPage
            app: root
            visible: root.currentTab === 0
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: bottomNav.top
        }
        AdvertisePage {
            id: advertPage
            app: root
            visible: root.currentTab === 1
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: bottomNav.top
        }
        TerminalPage {
            id: terminalPage
            app: root
            visible: root.currentTab === 2
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: bottomNav.top
        }
        CommandPage {
            id: commandPage
            app: root
            visible: root.currentTab === 3
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: bottomNav.top
        }
        AboutPage {
            id: aboutPage
            app: root
            visible: root.currentTab === 4
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: bottomNav.top
        }

        // ==================== 底部导航 ====================
        Rectangle {
            id: bottomNav
            width: parent.width
            height: 60
            anchors.bottom: parent.bottom
            color: root.cSurface
            z: 10
            Rectangle {
                width: parent.width
                height: 1
                color: root.cDivider
                anchors.top: parent.top
            }
            Row {
                anchors.fill: parent
                Repeater {
                    model: [
                        { label: "扫描(Scan)",    icon: "📡" },
                        { label: "广播(Advert)",  icon: "📢" },
                        { label: "调试(Debug)",   icon: "🔧" },
                        { label: "指令(Commands)", icon: "⚙" },
                        { label: "关于(About)",   icon: "ℹ" }
                    ]
                    delegate: Item {
                        width: bottomNav.width / 5
                        height: bottomNav.height
                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.currentTab = index
                        }
                        Column {
                            anchors.centerIn: parent
                            spacing: 2
                            Text {
                                width: bottomNav.width / 5
                                text: modelData.icon
                                font.pixelSize: 18
                                color: root.currentTab === index ? root.cAccent : root.cSubtext
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            Text {
                                width: bottomNav.width / 5 - 8
                                text: modelData.label
                                font.pixelSize: 11
                                color: root.currentTab === index ? root.cAccent : root.cSubtext
                                font.bold: root.currentTab === index
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }
        }
    }

    // =================================================================
    // 与 C++ 后端信号对接：扫描结果 / 状态 / 权限
    // =================================================================
    Connections {
        target: bleManager
        // 扫描到新设备：按地址去重后加入原始模型，再重建显示模型
        function onDeviceFound(name, address, rssi, isLe) {
            for (var i = 0; i < deviceModel.count; i++)
                if (deviceModel.get(i).address === address) return
            deviceModel.append({ name: name, address: address, rssi: rssi,
                                 connected: false, connecting: false,
                                 isConnectable: isLe })
            root.rebuildViewModel()
        }
        // 扫描 / 广播 / 连接 状态变化驱动 UI
        function onScanningChanged(v) {
            root.scanning = v
            if (v) root.showToast("开始扫描…")
            else root.showToast("扫描已停止")
        }
        function onAdvertisingChanged(v) {
            root.advertising = v
            root.showToast(v ? "正在广播" : "已停止广播")
            if (!v) root.peripheralConnected = false
        }
        // 外设被其它设备连接 / 断开
        function onPeripheralConnectedChanged(v) {
            root.peripheralConnected = v
            // 连接信息同步到指令页日志文本框（绿字 + 设备信息）
            root.logPeripheral(v)
            root.showToast(v ? "有设备已连接，可收发数据" : "设备已断开连接")
            // 作为从设备被其它主设备连接后自动跳转到调试终端
            if (v) root.currentTab = 2
        }
        // 外设收到已连接设备写入的数据
        function onPeripheralDataReceived(data) {
            root.showToast("收到数据: " + String(data))
        }
        // Central 连接成功后服务发现完成：补充打印服务 UUID 到指令页日志
        function onServicesDiscovered(services) {
            root.logServices(services)
        }
        // 广播 / 扫描 / 连接 错误或提示（含广播名称截断提示）
        function onErrorOccurred(message) {
            // 正处于连接中时收到的错误按「连接失败」处理并展示具体原因
            if (root.connectingAddress !== "") {
                root.failConnect(root.connectingAddress,
                                 "连接失败：" + message)
                return
            }
            // 连接失败收口后 2.5 秒内的补充错误信号直接忽略，避免重复弹窗
            if (Date.now() < root.suppressErrUntil) return
            root.showToast(message)
        }
        function onConnectedChanged(v) {
            if (v) {
                // 连接成功：复位「连接中」标记，再按原逻辑更新并跳转调试终端
                connectTimeoutTimer.stop()
                if (root.connectingAddress !== "") {
                    root.setDeviceConnecting(root.connectingAddress, false)
                    root.connectingAddress = ""
                }
                root.updateDeviceConnected(root.targetAddress, true)
                // 连接信息打印到指令页日志文本框（绿字）
                root.logConnected()
                root.showToast("已连接设备")
                root.currentTab = 2
            } else {
                // 断开分两类：
                //  1) 连接尚未成功即失败（仍处于连接中）→ 弹失败提示
                //  2) 已连接设备主动/被动断开 → 按原逻辑提示
                if (root.connectingAddress !== "") {
                    root.failConnect(root.connectingAddress,
                        "连接失败：请确认设备已开机且在附近，然后重试")
                    return
                }
                // 连接失败收口后残留的断连信号直接忽略，避免重复弹窗
                if (Date.now() < root.suppressErrUntil) return
                root.updateDeviceConnected(root.targetAddress, false)
                // 断开日志同步到指令页日志文本框
                root.logDisconnected()
                root.showToast("已断开连接")
            }
        }
    }

    // 收发数据行镜像：BleTerminal 每追加一条记录（格式与调试文本框完全一致，
    // 已按 十六进制/时间戳 开关处理）同步到指令页日志文本框，实现两个文本框
    // 显示逻辑一致：收到(RX)用青色、本机发送(TX)用绿色。
    Connections {
        target: bleManager.terminal
        function onDataLogged(display, receive) {
            commandPage.appendLog((receive ? "[接收]: " : "[发送]: ") + display,
                                  receive ? root.cAccent : root.cGreen)
        }
    }

    // 权限申请结果：授权成功后执行先前保存的待执行动作
    Connections {
        target: bleManager.permissions
        function onPermissionGranted() {
            // 授予权限后刷新系统蓝牙名称（Android 上读取它需要 BLUETOOTH_CONNECT）
            bleManager.refreshLocalDeviceName()
            if (root.pendingAction) {
                var act = root.pendingAction
                root.pendingAction = null
                act()
            }
        }
        function onPermissionDenied(permission, message) {
            root.pendingAction = null
            // 连接权限被拒：复位「连接中」状态并给出明确引导
            if (permission === BlePermissions.ConnectPermission) {
                root.clearConnecting()
                root.showToast("连接权限被拒绝，无法连接设备。"
                               + "请重试；若持续被拒请到系统设置开启「附近设备」权限。")
                return
            }
            // Android 上若用户勾选“不再询问”，需到系统设置手动开启；
            // 此处明确提示广播/扫描权限的用途，并鼓励重试。
            var hint = ""
            if (permission === BlePermissions.AdvertisePermission)
                hint = "广播权限被拒绝，无法作为外设广播。请重试，若持续被拒请到系统设置开启「附近设备」权限。"
            else
                hint = "扫描权限被拒绝，无法搜索设备。请重试。"
            root.showToast(hint)
        }
    }

    // =================================================================
    // 轻提示 (Toast)
    // =================================================================
    Rectangle {
        id: toast
        visible: false
        width: toastText.implicitWidth + 40
        height: 38
        radius: 19
        color: "#E02A2E38"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 90
        z: 100
        Text {
            id: toastText
            anchors.centerIn: parent
            color: "white"
            font.pixelSize: 13
        }
        Timer {
            id: toastTimer
            interval: 2000
            onTriggered: toast.visible = false
        }
    }
}
