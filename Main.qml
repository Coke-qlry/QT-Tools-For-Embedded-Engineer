import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BLE_SAR.Ble

// =====================================================================
// BLE_SAR - 仿 nRF Connect 界面（中文差异化版本）
// 界面通过全局对象 bleManager 调用 C++ 后端，实现扫描 / 广播 / 连接。
// 权限通过 bleManager.permissions（BlePermissions）在 Android 上动态申请。
//
// 页面架构：扫描 / 广播 / 指令 / 关于 四个功能相互独立，
// 各自拥有独立顶栏与专属操作，底部导航负责切换。
//   - 扫描页：右上角「过滤 / 排序 / 清除」+ 右下角悬浮扫描按钮，
//             支持按 RSSI 阈值、MAC 地址、蓝牙名称过滤设备。
//   - 广播页 / 指令页 / 关于页：独立页面，顶栏带返回按钮。
// =====================================================================

Window {
    id: root
    width: 440
    height: 860
    minimumWidth: 380
    minimumHeight: 640
    visible: true
    title: qsTr("BLE SAR - 仿 nRF Connect")

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
    property int currentTab: 0          // 0=扫描 1=广播 2=指令 3=关于
    property bool scanning: false       // 由 bleManager.scanningChanged 驱动
    property bool advertising: false    // 由 bleManager.advertisingChanged 驱动
    property bool peripheralConnected: false // 外设被其它设备连接状态
    property int lastFound: -1          // 指令查找的起始游标
    property bool sortByName: false     // true=按名称排序，false=按RSSI排序
    property string targetAddress: ""   // 当前连接目标设备地址
    property var pendingAction: null    // 权限授权成功后待执行的动作

    // ---- 扫描过滤条件 ----
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
    Component.onCompleted: autoScanTimer.start()
    Timer {
        id: autoScanTimer
        interval: 300
        onTriggered: root.startScan()
    }

    // =================================================================
    // 主界面功能接口（调用 C++ 后端 bleManager）
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
        root.targetAddress = address
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
    // 应用过滤对话框中的条件
    function applyFilter() {
        root.filterMinRssi = filterDialog.rssiSel
        root.filterAddress = filterMacField.text.trim()
        root.filterName = filterNameField.text.trim()
        root.rebuildViewModel()
        filterDialog.close()
        root.showToast(root.filterActive ? "已应用过滤条件" : "已清除过滤条件")
    }
    // 重置全部过滤条件（含对话框内输入）
    function resetFilter() {
        root.filterMinRssi = -127
        root.filterAddress = ""
        root.filterName = ""
        filterDialog.rssiSel = -127
        filterMacField.text = ""
        filterNameField.text = ""
        root.rebuildViewModel()
        root.showToast("已重置过滤条件")
    }
    // 清除过滤条件（提示条上的 ✕）
    function clearFilter() {
        root.filterMinRssi = -127
        root.filterAddress = ""
        root.filterName = ""
        root.rebuildViewModel()
        root.showToast("已清除过滤条件")
    }
    // 连接状态变化时同步原始模型与显示模型
    function updateDeviceConnected(address, v) {
        for (var i = 0; i < deviceModel.count; i++) {
            if (deviceModel.get(i).address === address)
                deviceModel.set(i, { connected: v })
        }
        for (var j = 0; j < viewModel.count; j++) {
            if (viewModel.get(j).address === address)
                viewModel.set(j, { connected: v })
        }
    }

    // ---- 广播 ----
    function startAdvertise() {
        ensurePermission(BlePermissions.AdvertisePermission, function () {
            // 名称为空时使用默认名 BLE_SAR；超长截断由 C++ 端完成并回调提示
            bleManager.startAdvertise(advertNameField.text.trim() || "BLE_SAR",
                                      advertUuidField.text.trim(),
                                      parseInt(advertIntervalField.text, 10) || 100)
        })
    }
    function stopAdvertise() {
        bleManager.stopAdvertise()
    }

    // ---- 轻提示 ----
    function showToast(msg) {
        toastText.text = msg
        toast.visible = true
        toastTimer.restart()
    }

    // ---- 指令页：增删改查 ----
    // 新增：弹出空表单
    function openAddDialog() {
        editDialog.isNew = true
        editDialog.titleText = "新增指令 (Add)"
        editNameField.text = ""
        editCmdField.text = ""
        editDialog.open()
    }
    // 修改：选中后单击右上角修改图标
    function openEditDialog() {
        var idx = commandList.currentIndex
        if (idx < 0) { showToast("请先选中一条指令"); return }
        var it = instructionModel.get(idx)
        editDialog.isNew = false
        editDialog.titleText = "修改指令 (Edit)"
        editNameField.text = it.name
        editCmdField.text = it.command
        editDialog.open()
    }
    // 保存（新增/修改共用）
    function saveEdit() {
        var n = editNameField.text.trim()
        var c = editCmdField.text.trim()
        if (n === "" && c === "") { showToast("名称与内容不能同时为空"); return }
        var isNew = editDialog.isNew
        if (isNew) {
            instructionModel.append({ name: n, command: c, note: "" })
        } else {
            instructionModel.set(commandList.currentIndex, { name: n, command: c, note: "" })
        }
        editDialog.close()
        showToast(isNew ? "已新增指令" : "已保存修改")
    }
    // 删除按钮：短按删选中
    function deleteSelectedCommand() {
        var idx = commandList.currentIndex
        if (idx < 0) { showToast("请先选中一条指令"); return }
        instructionModel.remove(idx)
        commandList.currentIndex = Math.min(idx, instructionModel.count - 1)
        showToast("已删除选中指令")
    }
    // 删除按钮：长按 2 秒删全部
    function deleteAllCommands() {
        instructionModel.clear()
        commandList.currentIndex = -1
        showToast("已删除全部指令")
    }
    // 查找：输入关键字后跳转定位
    function locateCommands(keyword) {
        keyword = keyword.trim()
        if (keyword === "") return
        var total = instructionModel.count
        var start = (lastFound + 1) % Math.max(total, 1)
        for (var i = 0; i < total; i++) {
            var idx = (start + i) % total
            var it = instructionModel.get(idx)
            if (it.name.indexOf(keyword) >= 0 || it.command.indexOf(keyword) >= 0 || it.note.indexOf(keyword) >= 0) {
                lastFound = idx
                commandList.currentIndex = idx
                commandList.positionViewAtIndex(idx, ListView.Center)
                showToast("已定位: " + it.name)
                return
            }
        }
        showToast("未找到包含「" + keyword + "」的指令")
    }

    // RSSI 转 0~4 格信号
    function rssiLevel(rssi) {
        if (rssi >= -55) return 4
        if (rssi >= -65) return 3
        if (rssi >= -75) return 2
        if (rssi >= -85) return 1
        return 0
    }

    // =================================================================
    // 设备列表模型（由 C++ 后端扫描结果填充）
    //   deviceModel: 原始数据（扫描填充，不随排序/过滤改变）
    //   viewModel  : 显示数据（过滤 + 排序后的子集）
    // 字段: name / address / rssi / connected / isConnectable
    // =================================================================
    ListModel { id: deviceModel }
    ListModel { id: viewModel }

    // =================================================================
    // 自定义指令模型（字段: name/command/note）
    // =================================================================
    ListModel {
        id: instructionModel
        Component.onCompleted: {
            instructionModel.append({ name: "读取电量",  command: "AT+VBAT?",    note: "返回当前电池电压" })
            instructionModel.append({ name: "读取SAR值", command: "AT+SAR?",     note: "查询SAR检测结果" })
            instructionModel.append({ name: "启动扫描",  command: "AT+SCAN=1",   note: "" })
            instructionModel.append({ name: "停止扫描",  command: "AT+SCAN=0",   note: "" })
            instructionModel.append({ name: "设置阈值",  command: "AT+THRESH=100", note: "设置SAR阈值(μW/g)" })
            instructionModel.append({ name: "重启设备",  command: "AT+RST",      note: "软重启" })
        }
    }

    // ---------------- 全局背景 ----------------
    Rectangle {
        anchors.fill: parent
        color: root.cBg
    }

    // =================================================================
    // 主界面：四个相互独立的页面（扫描 / 广播 / 指令 / 关于）
    // 每页拥有独立顶栏，扫描页额外带悬浮扫描按钮
    // =================================================================
    Item {
        id: mainView
        anchors.fill: parent

        // ==================== 扫描页 ====================
        Item {
            id: scanPage
            visible: root.currentTab === 0
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: bottomNav.top

            // -------- 扫描页顶栏 --------
            Rectangle {
                id: scanHeader
                width: parent.width
                height: 56
                color: root.cSurface
                z: 10
                Rectangle {
                    width: parent.width
                    height: 1
                    color: root.cDivider
                    anchors.bottom: parent.bottom
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "扫描 (Scan)"
                    color: root.cText
                    font.pixelSize: 16
                    font.bold: true
                }
                // 过滤（仅扫描页显示）
                ToolButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 58
                    implicitHeight: 52
                    onClicked: filterDialog.open()
                    contentItem: Column {
                        spacing: 2
                        anchors.centerIn: parent
                        Text {
                            text: "⏳"
                            font.pixelSize: 15
                            color: root.filterActive ? root.cAccent : root.cText
                            horizontalAlignment: Text.AlignHCenter
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                        Text {
                            text: "过滤(Filter)"
                            font.pixelSize: 9
                            color: root.filterActive ? root.cAccent : root.cText
                            horizontalAlignment: Text.AlignHCenter
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                    }
                }
                // 排序（仅扫描页显示）
                ToolButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 66
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 58
                    implicitHeight: 52
                    onClicked: root.sortDevices()
                    contentItem: Column {
                        spacing: 2
                        anchors.centerIn: parent
                        Text {
                            text: "⇅"
                            font.pixelSize: 15
                            color: root.cText
                            horizontalAlignment: Text.AlignHCenter
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                        Text {
                            text: "排序(Sort)"
                            font.pixelSize: 9
                            color: root.cText
                            horizontalAlignment: Text.AlignHCenter
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                    }
                }
                // 清除（仅扫描页显示）
                ToolButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 124
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 58
                    implicitHeight: 52
                    onClicked: root.clearDevices()
                    contentItem: Column {
                        spacing: 2
                        anchors.centerIn: parent
                        Text {
                            text: "✕"
                            font.pixelSize: 15
                            color: root.cText
                            horizontalAlignment: Text.AlignHCenter
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                        Text {
                            text: "清除(Clear)"
                            font.pixelSize: 9
                            color: root.cText
                            horizontalAlignment: Text.AlignHCenter
                            anchors.horizontalCenter: parent.horizontalCenter
                        }
                    }
                }
            }

            // -------- 过滤激活提示条 --------
            Rectangle {
                id: filterBanner
                visible: root.filterActive
                width: parent.width
                height: 36
                color: root.cAccentDark
                anchors.top: scanHeader.bottom
                z: 5
                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    Text {
                        text: "过滤:"
                        color: "#BFE8FB"
                        font.pixelSize: 12
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: root.filterSummaryText
                        color: "white"
                        font.pixelSize: 12
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: "✕ 清除"
                    color: "#BFE8FB"
                    font.pixelSize: 12
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.clearFilter()
                    }
                }
            }

            // -------- 状态条 --------
            Rectangle {
                id: statusBanner
                width: parent.width
                height: 44
                color: root.scanning ? root.cAccentDark : root.cSurface
                visible: root.scanning || deviceModel.count === 0
                anchors.top: filterBanner.visible ? filterBanner.bottom : scanHeader.bottom
                Row {
                    anchors.centerIn: parent
                    spacing: 10
                    BusyIndicator {
                        width: 18
                        height: 18
                        running: root.scanning
                        visible: root.scanning
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.scanning ? qsTr("正在扫描… (Scanning)") : qsTr("点击右下角按钮开始扫描 (Scan)")
                        color: root.cText
                        font.pixelSize: 13
                    }
                }
            }

            // -------- 设备列表（显示模型：过滤 + 排序） --------
            ListView {
                id: deviceList
                anchors.top: statusBanner.visible ? statusBanner.bottom
                                                  : (filterBanner.visible ? filterBanner.bottom : scanHeader.bottom)
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                clip: true
                model: viewModel
                spacing: 1
                ScrollBar.vertical: ScrollBar {}

                delegate: Rectangle {
                    width: deviceList.width
                    height: 72
                    color: mouseArea.pressed ? root.cSurface2 : root.cSurface

                    Rectangle {
                        width: 10
                        height: 10
                        radius: 5
                        color: model.connected ? root.cGreen : root.cSubtext
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 36
                        anchors.right: rightCol.left
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: model.name
                            color: root.cText
                            font.pixelSize: 15
                            font.bold: true
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: model.address
                            color: root.cSubtext
                            font.pixelSize: 12
                        }
                    }

                    Row {
                        id: rightCol
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10

                        // RSSI
                        Row {
                            spacing: 6
                            Rectangle {
                                width: 24
                                height: 30
                                anchors.verticalCenter: parent.verticalCenter
                                color: "transparent"
                                Repeater {
                                    model: 4
                                    Rectangle {
                                        width: 3
                                        height: 6 + (index + 1) * 6
                                        radius: 1
                                        anchors.bottom: parent.bottom
                                        x: index * 6
                                        color: (index < rssiLevel(model.rssi))
                                               ? (model.rssi >= -65 ? root.cGreen : root.cOrange)
                                               : root.cDivider
                                    }
                                }
                            }
                            Text {
                                text: model.rssi
                                color: model.rssi >= -65 ? root.cGreen :
                                       model.rssi >= -85 ? root.cOrange : root.cRed
                                font.pixelSize: 12
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        // 连接按钮
                        Rectangle {
                            width: 66
                            height: 30
                            radius: 15
                            color: model.connected ? root.cDivider : root.cAccent
                            anchors.verticalCenter: parent.verticalCenter
                            Text {
                                anchors.centerIn: parent
                                text: model.connected ? "已连接" : "连接(Conn)"
                                color: "white"
                                font.pixelSize: 11
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: root.connectToDeviceByAddress(model.address)
                            }
                        }
                    }

                    // 整行点击也可连接
                    MouseArea {
                        id: mouseArea
                        anchors.fill: parent
                        onClicked: root.connectToDeviceByAddress(model.address)
                    }

                    Rectangle {
                        width: parent.width
                        height: 1
                        color: root.cDivider
                        anchors.bottom: parent.bottom
                    }
                }
            }

            // -------- 悬浮扫描按钮 (FAB) —— 仅扫描页显示 --------
            Rectangle {
                width: 60
                height: 60
                radius: 30
                color: root.scanning ? root.cRed : root.cAccent
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 16
                z: 20
                Column {
                    anchors.centerIn: parent
                    spacing: 1
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: root.scanning ? "■" : "▶"
                        color: "white"
                        font.pixelSize: 16
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: root.scanning ? "停止(Stop)" : "扫描(Scan)"
                        color: "white"
                        font.pixelSize: 10
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        if (root.scanning) root.stopScan()
                        else root.startScan()
                    }
                }
            }
        }

        // ==================== 广播页 ====================
        Item {
            id: advertPage
            visible: root.currentTab === 1
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: bottomNav.top

            // -------- 广播页顶栏 --------
            Rectangle {
                id: advertHeader
                width: parent.width
                height: 56
                color: root.cSurface
                z: 10
                Rectangle {
                    width: parent.width
                    height: 1
                    color: root.cDivider
                    anchors.bottom: parent.bottom
                }
                // 返回
                ToolButton {
                    anchors.left: parent.left
                    anchors.leftMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 44
                    implicitHeight: 44
                    onClicked: root.currentTab = 0
                    contentItem: Text {
                        text: "‹"
                        font.pixelSize: 30
                        color: root.cText
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 52
                    anchors.verticalCenter: parent.verticalCenter
                    text: "广播 (Advertiser)"
                    color: root.cText
                    font.pixelSize: 16
                    font.bold: true
                }
            }

            // -------- 广播内容 --------
            Column {
                anchors.top: advertHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 16
                spacing: 12
                Text {
                    text: qsTr("广播 (Advertiser)")
                    color: root.cText
                    font.pixelSize: 20
                    font.bold: true
                }
                // 广播状态指示 + 外设连接状态
                Column {
                    width: parent.width
                    spacing: 6
                    Row {
                        spacing: 8
                        Rectangle {
                            width: 10
                            height: 10
                            radius: 5
                            color: root.advertising ? root.cGreen : root.cSubtext
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: root.advertising ? "正在广播 (Advertising)" : "未广播 (Idle)"
                            color: root.advertising ? root.cGreen : root.cSubtext
                            font.pixelSize: 13
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    Row {
                        spacing: 8
                        visible: root.advertising   // 仅在广播中显示连接状态
                        Rectangle {
                            width: 10
                            height: 10
                            radius: 5
                            color: root.peripheralConnected ? root.cGreen : root.cSubtext
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: root.peripheralConnected
                                  ? "已连接：可收发数据" : "等待其它设备连接…"
                            color: root.peripheralConnected ? root.cGreen : root.cSubtext
                            font.pixelSize: 13
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }
                Rectangle {
                    width: parent.width
                    height: 44
                    radius: 22
                    color: root.advertising ? root.cRed : root.cAccent
                    anchors.horizontalCenter: parent.horizontalCenter
                    Text {
                        anchors.centerIn: parent
                        text: root.advertising ? qsTr("停止广播 (Stop)") : qsTr("开始广播 (Start)")
                        color: "white"
                        font.pixelSize: 15
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.advertising ? root.stopAdvertise() : root.startAdvertise()
                    }
                }
                GridLayout {
                    width: parent.width
                    columns: 2
                    rowSpacing: 10
                    columnSpacing: 12

                    Text { text: "广播名称"; color: root.cSubtext; font.pixelSize: 14 }
                    TextField {
                        id: advertNameField
                        Layout.fillWidth: true
                        height: 36
                        text: "BLE_SAR"
                        placeholderText: "BLE_SAR（≤29 字节，超长自动截断）"
                        placeholderTextColor: root.cSubtext
                        color: root.cText
                        font.pixelSize: 13
                        background: Rectangle { color: root.cSurface2; radius: 4; border.color: root.cDivider }
                    }

                    Text { text: "Service UUID"; color: root.cSubtext; font.pixelSize: 14 }
                    TextField {
                        id: advertUuidField
                        Layout.fillWidth: true
                        height: 36
                        placeholderText: "0000feb1-0000-1000-8000-00805f9b34fb"
                        placeholderTextColor: root.cSubtext
                        color: root.cAccent
                        font.pixelSize: 12
                        background: Rectangle { color: root.cSurface2; radius: 4; border.color: root.cDivider }
                    }

                    Text { text: "广播间隔(ms)"; color: root.cSubtext; font.pixelSize: 14 }
                    TextField {
                        id: advertIntervalField
                        Layout.fillWidth: true
                        height: 36
                        text: "100"
                        placeholderText: "100"
                        inputMethodHints: Qt.ImhDigitsOnly
                        placeholderTextColor: root.cSubtext
                        color: root.cText
                        font.pixelSize: 13
                        background: Rectangle { color: root.cSurface2; radius: 4; border.color: root.cDivider }
                    }
                }
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("说明：点击「开始广播」后本机作为真实可连接的 BLE 外设广播（内置 GATT 服务），其它设备可扫描到并连接，连接后可收发数据。广播名称默认 BLE_SAR、可编辑，名称超过 29 字节会自动截断。Service UUID 可留空。")
                    color: root.cSubtext
                    font.pixelSize: 12
                }
            }
        }

        // ==================== 指令页 ====================
        Item {
            id: commandPage
            visible: root.currentTab === 2
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: bottomNav.top

            // -------- 指令页顶栏 --------
            Rectangle {
                id: cmdHeader
                width: parent.width
                height: 56
                color: root.cSurface
                z: 10
                Rectangle {
                    width: parent.width
                    height: 1
                    color: root.cDivider
                    anchors.bottom: parent.bottom
                }

                // 返回
                ToolButton {
                    anchors.left: parent.left
                    anchors.leftMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 44
                    implicitHeight: 44
                    onClicked: root.currentTab = 0
                    contentItem: Text {
                        text: "‹"
                        font.pixelSize: 30
                        color: root.cText
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 52
                    anchors.verticalCenter: parent.verticalCenter
                    text: "自定义指令"
                    color: root.cText
                    font.pixelSize: 16
                    font.bold: true
                }

                // -------- 右上角：增 删 改 查 --------
                ToolButton {
                    id: addBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 60
                    implicitHeight: 52
                    onClicked: root.openAddDialog()
                    contentItem: Column {
                        spacing: 2
                        anchors.centerIn: parent
                        Text {
                            text: "＋"
                            width: 60
                            height: 18
                            font.pixelSize: 15
                            color: root.cAccent
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        Text {
                            text: "增(Add)"
                            width: 60
                            height: 12
                            font.pixelSize: 9
                            color: root.cText
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
                ToolButton {
                    id: editBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 68
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 60
                    implicitHeight: 52
                    onClicked: root.openEditDialog()
                    contentItem: Column {
                        spacing: 2
                        anchors.centerIn: parent
                        Text {
                            text: "✎"
                            width: 60
                            height: 18
                            font.pixelSize: 15
                            color: root.cAccent
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        Text {
                            text: "改(Edit)"
                            width: 60
                            height: 12
                            font.pixelSize: 9
                            color: root.cText
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
                ToolButton {
                    id: delBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 128
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 60
                    implicitHeight: 52

                    // 长按 2 秒 = 全部删除；短按 = 删除选中
                    onPressed: {
                        delHoldTimer.restart()
                        delBtn.colorOverlay = true
                    }
                    onReleased: {
                        if (delHoldTimer.running) { delHoldTimer.stop(); root.deleteSelectedCommand() }
                        delBtn.colorOverlay = false
                    }
                    onCanceled: {
                        delHoldTimer.stop()
                        delBtn.colorOverlay = false
                    }
                    property bool colorOverlay: false

                    Timer {
                        id: delHoldTimer
                        interval: 2000
                        onTriggered: {
                            delBtn.colorOverlay = false
                            root.deleteAllCommands()
                        }
                    }
                    contentItem: Column {
                        spacing: 2
                        anchors.centerIn: parent
                        Text {
                            text: "🗑"
                            width: 60
                            height: 18
                            font.pixelSize: 15
                            color: delBtn.colorOverlay ? root.cRed : root.cText
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        Text {
                            text: "删(Del)"
                            width: 60
                            height: 12
                            font.pixelSize: 9
                            color: root.cText
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    ToolTip.visible: delBtn.hovered
                    ToolTip.delay: 600
                    ToolTip.text: "短按: 删除选中指令\n长按2秒: 删除全部指令"
                }
                ToolButton {
                    id: findBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 188
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 60
                    implicitHeight: 52
                    onClicked: findDialog.open()
                    contentItem: Column {
                        spacing: 2
                        anchors.centerIn: parent
                        Text {
                            text: "🔍"
                            width: 60
                            height: 18
                            font.pixelSize: 15
                            color: root.cAccent
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        Text {
                            text: "查(Find)"
                            width: 60
                            height: 12
                            font.pixelSize: 9
                            color: root.cText
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }

            // -------- 指令列表 --------
            ListView {
                id: commandList
                anchors.top: cmdHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                clip: true
                model: instructionModel
                spacing: 1
                ScrollBar.vertical: ScrollBar {}
                highlightFollowsCurrentItem: true
                highlightMoveDuration: 150
                highlight: Rectangle {
                    color: Qt.rgba(0, 0.66, 0.81, 0.22)
                    radius: 4
                }

                delegate: Rectangle {
                    width: commandList.width
                    height: 68
                    color: mouseArea2.pressed ? root.cSurface2 : root.cSurface

                    MouseArea {
                        id: mouseArea2
                        anchors.fill: parent
                        onClicked: {
                            commandList.currentIndex = index      // 选中
                            root.lastFound = index
                        }
                    }

                    // 序号徽标
                    Rectangle {
                        width: 28
                        height: 28
                        radius: 14
                        color: commandList.currentIndex === index ? root.cAccent : root.cSurface2
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            anchors.centerIn: parent
                            text: index + 1
                            color: "white"
                            font.pixelSize: 12
                            font.bold: true
                        }
                    }

                    // 名称 / 指令内容 / 备注
                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 56
                        anchors.right: parent.right
                        anchors.rightMargin: 40
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: model.name
                            color: root.cText
                            font.pixelSize: 15
                            font.bold: true
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: model.command
                            color: root.cAccent
                            font.pixelSize: 13
                            font.family: "Consolas"
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            visible: model.note !== ""
                            text: model.note
                            color: root.cSubtext
                            font.pixelSize: 11
                        }
                    }

                    // 右侧选中标记
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        text: commandList.currentIndex === index ? "●" : ""
                        color: root.cAccent
                        font.pixelSize: 12
                    }

                    Rectangle {
                        width: parent.width
                        height: 1
                        color: root.cDivider
                        anchors.bottom: parent.bottom
                    }
                }
            }
        }

        // ==================== 关于页 ====================
        Item {
            id: aboutPage
            visible: root.currentTab === 3
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: bottomNav.top

            // -------- 关于页顶栏 --------
            Rectangle {
                id: aboutHeader
                width: parent.width
                height: 56
                color: root.cSurface
                z: 10
                Rectangle {
                    width: parent.width
                    height: 1
                    color: root.cDivider
                    anchors.bottom: parent.bottom
                }
                // 返回
                ToolButton {
                    anchors.left: parent.left
                    anchors.leftMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 44
                    implicitHeight: 44
                    onClicked: root.currentTab = 0
                    contentItem: Text {
                        text: "‹"
                        font.pixelSize: 30
                        color: root.cText
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 52
                    anchors.verticalCenter: parent.verticalCenter
                    text: "关于 (About)"
                    color: root.cText
                    font.pixelSize: 16
                    font.bold: true
                }
            }

            // -------- 关于内容 --------
            Column {
                anchors.centerIn: parent
                spacing: 8
                Rectangle {
                    width: 80
                    height: 80
                    radius: 18
                    color: root.cAccent
                    anchors.horizontalCenter: parent.horizontalCenter
                    Text {
                        anchors.centerIn: parent
                        text: "B"
                        color: "white"
                        font.bold: true
                        font.pixelSize: 44
                    }
                }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "BLE-SAR"; color: root.cText; font.pixelSize: 22; font.bold: true }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Version 1.0.0"; color: root.cSubtext; font.pixelSize: 13 }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "做您最舒心的蓝牙调试助手"; color: root.cSubtext; font.pixelSize: 13 }
            }
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
                        { label: "指令(Commands)", icon: "⚙" },
                        { label: "关于(About)",   icon: "ℹ" }
                    ]
                    delegate: Item {
                        width: bottomNav.width / 4
                        height: bottomNav.height
                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.currentTab = index
                        }
                        Column {
                            anchors.centerIn: parent
                            spacing: 2
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.icon
                                font.pixelSize: 18
                                color: root.currentTab === index ? root.cAccent : root.cSubtext
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.label
                                font.pixelSize: 11
                                color: root.currentTab === index ? root.cAccent : root.cSubtext
                                font.bold: root.currentTab === index
                            }
                        }
                    }
                }
            }
        }
    }

    // =================================================================
    // 过滤对话框（扫描页：按 RSSI / MAC 地址 / 蓝牙名称过滤）
    // 注意：header 属性是 Dialog 专有（Dialog 继承自 Popup），
    // 若用 Popup 声明会导致 QML 加载失败、程序启动闪退。
    // =================================================================
    Dialog {
        id: filterDialog
        anchors.centerIn: parent
        modal: true
        width: Math.min(root.width - 32, 400)
        padding: 16

        // 信号强度阈值选择（对话框内临时值，应用时写入 root）
        property int rssiSel: -127
        onOpened: {
            filterDialog.rssiSel = root.filterMinRssi
            filterMacField.text = root.filterAddress
            filterNameField.text = root.filterName
        }

        background: Rectangle { color: root.cSurface; radius: 12 }
        header: Rectangle {
            width: parent.width
            height: 48
            color: "transparent"
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: "设备过滤 (Filter)"
                color: root.cText
                font.pixelSize: 16
                font.bold: true
            }
        }

        contentItem: Column {
            spacing: 14
            // 信号强度阈值
            Text { text: "信号强度 (RSSI)"; color: root.cSubtext; font.pixelSize: 13 }
            Row {
                spacing: 6
                Repeater {
                    model: [
                        { label: "全部",  v: -127 },
                        { label: ">-50",  v: -50 },
                        { label: ">-60",  v: -60 },
                        { label: ">-70",  v: -70 },
                        { label: ">-80",  v: -80 },
                        { label: ">-90",  v: -90 }
                    ]
                    Rectangle {
                        width: 50
                        height: 30
                        radius: 15
                        color: filterDialog.rssiSel === modelData.v ? root.cAccent : root.cSurface2
                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: "white"
                            font.pixelSize: 12
                            font.bold: filterDialog.rssiSel === modelData.v
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: filterDialog.rssiSel = modelData.v
                        }
                    }
                }
            }
            // MAC 地址过滤
            Text { text: "MAC 地址包含"; color: root.cSubtext; font.pixelSize: 13 }
            TextField {
                id: filterMacField
                width: parent.width
                height: 36
                placeholderText: "如: A4:C1:38 (留空不过滤)"
                placeholderTextColor: root.cSubtext
                color: root.cText
                font.pixelSize: 13
                background: Rectangle { color: root.cSurface2; radius: 4; border.color: root.cDivider }
            }
            // 名称过滤
            Text { text: "蓝牙名称包含"; color: root.cSubtext; font.pixelSize: 13 }
            TextField {
                id: filterNameField
                width: parent.width
                height: 36
                placeholderText: "如: BLE (留空不过滤)"
                placeholderTextColor: root.cSubtext
                color: root.cText
                font.pixelSize: 13
                background: Rectangle { color: root.cSurface2; radius: 4; border.color: root.cDivider }
            }
            // 操作按钮
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 16
                Button {
                    text: "重置 (Reset)"
                    onClicked: root.resetFilter()
                    contentItem: Text { text: parent.text; color: root.cSubtext; font.pixelSize: 14 }
                    background: Rectangle { color: root.cSurface2; radius: 16; width: 130; height: 32 }
                }
                Button {
                    text: "应用 (Apply)"
                    onClicked: root.applyFilter()
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 14; font.bold: true }
                    background: Rectangle { color: root.cAccent; radius: 16; width: 130; height: 32 }
                }
            }
        }
    }

    // =================================================================
    // 编辑对话框（新增 / 修改 共用）
    // =================================================================
    Dialog {
        id: editDialog
        anchors.centerIn: parent
        modal: true
        width: 360
        padding: 16
        property bool isNew: true
        property string titleText: ""

        background: Rectangle { color: root.cSurface; radius: 12 }
        header: Rectangle {
            width: parent.width
            height: 48
            color: "transparent"
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: editDialog.titleText
                color: root.cText
                font.pixelSize: 16
                font.bold: true
            }
        }

        contentItem: Column {
            spacing: 12
            TextField {
                id: editNameField
                width: parent.width
                placeholderText: "指令名称 (如: 读取电量)"
                placeholderTextColor: root.cSubtext
                color: root.cText
                font.pixelSize: 14
                background: Rectangle {
                    color: root.cSurface2
                    radius: 4
                    border.color: root.cDivider
                }
            }
            TextField {
                id: editCmdField
                width: parent.width
                placeholderText: "指令内容 (如: AT+VBAT?)"
                placeholderTextColor: root.cSubtext
                color: root.cAccent
                font.pixelSize: 14
                background: Rectangle {
                    color: root.cSurface2
                    radius: 4
                    border.color: root.cDivider
                }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 16
                Button {
                    text: "取消 (Cancel)"
                    onClicked: editDialog.close()
                    contentItem: Text { text: parent.text; color: root.cSubtext; font.pixelSize: 14 }
                    background: Rectangle { color: root.cSurface2; radius: 16; width: 130; height: 32 }
                }
                Button {
                    text: "确定 (OK)"
                    onClicked: root.saveEdit()
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 14; font.bold: true }
                    background: Rectangle { color: root.cAccent; radius: 16; width: 130; height: 32 }
                }
            }
        }
    }

    // =================================================================
    // 查找对话框
    // =================================================================
    Dialog {
        id: findDialog
        anchors.centerIn: parent
        modal: true
        width: 360
        padding: 16
        onOpened: findField.forceActiveFocus()

        background: Rectangle { color: root.cSurface; radius: 12 }
        header: Rectangle {
            width: parent.width
            height: 48
            color: "transparent"
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: "查找指令 (Find)"
                color: root.cText
                font.pixelSize: 16
                font.bold: true
            }
        }

        contentItem: Column {
            spacing: 12
            TextField {
                id: findField
                width: parent.width
                placeholderText: "输入名称/内容关键字…"
                placeholderTextColor: root.cSubtext
                color: root.cText
                font.pixelSize: 14
                onAccepted: {
                    root.locateCommands(findField.text)
                    findDialog.close()
                }
                background: Rectangle {
                    color: root.cSurface2
                    radius: 4
                    border.color: root.cDivider
                }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 16
                Button {
                    text: "取消 (Cancel)"
                    onClicked: findDialog.close()
                    contentItem: Text { text: parent.text; color: root.cSubtext; font.pixelSize: 14 }
                    background: Rectangle { color: root.cSurface2; radius: 16; width: 130; height: 32 }
                }
                Button {
                    text: "查找 (Find)"
                    onClicked: {
                        root.locateCommands(findField.text)
                        findDialog.close()
                    }
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 14; font.bold: true }
                    background: Rectangle { color: root.cAccent; radius: 16; width: 130; height: 32 }
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
                                 connected: false, isConnectable: isLe })
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
            root.showToast(v ? "有设备已连接，可收发数据" : "设备已断开连接")
        }
        // 外设收到已连接设备写入的数据
        function onPeripheralDataReceived(data) {
            root.showToast("收到数据: " + String(data))
        }
        // 广播 / 扫描 / 连接 错误或提示（含广播名称截断提示）
        function onErrorOccurred(message) {
            root.showToast(message)
        }
        function onConnectedChanged(v) {
            // 依据连接状态更新两个设备模型对应行
            root.updateDeviceConnected(root.targetAddress, v)
            root.showToast(v ? "已连接设备" : "已断开连接")
        }
    }

    // 权限申请结果：授权成功后执行先前保存的待执行动作
    Connections {
        target: bleManager.permissions
        function onPermissionGranted() {
            if (root.pendingAction) {
                var act = root.pendingAction
                root.pendingAction = null
                act()
            }
        }
        function onPermissionDenied(permission, message) {
            root.pendingAction = null
            // Android 上若用户勾选“不再询问”，需到系统设置手动开启；
            // 此处明确提示广播/扫描权限的用途，并鼓励重试。
            var hint = ""
            if (permission === BlePermissions.AdvertisePermission)
                hint = "广播权限被拒绝，无法作为外设广播。请重试，若持续被拒请到系统设置开启「附近设备」权限。"
            else if (permission === BlePermissions.ScanPermission)
                hint = "扫描权限被拒绝，无法搜索设备。请重试。"
            else
                hint = "连接权限被拒绝，无法连接设备。请重试。"
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
