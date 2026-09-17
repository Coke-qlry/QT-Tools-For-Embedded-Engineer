import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

// =====================================================================
// 调试终端页（底部导航第 3 个 Tab）
// 连接后在此收发数据：服务与特征目录、接收/发送区、十六进制/时间戳、
// 定时发送、导出记录。本页自带 gattModel 与其全部专属函数与对话框；
// 主题色 / Toast / 共享状态经 app（Main.qml 根 Window）访问。
// =====================================================================
Item {
    id: page
    property var app

    // 服务与特征面板是否展开（仅本页使用）
    property bool gattOpen: false

    // 「绑定此设备」勾选框状态：双向桥接到 DebugCheckboxStatusControl，
    // 让 Main.qml 启动时从 SQLite 仓库回填，用户切换时落库。
    // 注：bindCurrentChecked 是 UI 层的临时状态，真正持久化通过
    // bleManager.checkboxControl.bindCurrentDevice 完成。
    property bool bindCurrentChecked: false
    // 最近一次「绑定此设备」勾选时所绑定设备的小写 MAC 地址。
    // 与 bindCurrentChecked 同步维护，用于让 BindPage 解除绑定时
    // 能准确判定「被解绑的就是当前勾选的这台」—— 仅靠 app.targetAddress
    // 判定太脆弱（断连/切设备后值会变），用本页维护的勾选地址更可靠。
    property string currentBoundAddress: ""

    // 调试页「服务与特征」目录（字段由 BleTerminal::charCatalog 返回：
    // serviceUuid/serviceName/charUuid/charName/properties/
    // writable/readable/notifiable/notifyOn/isSendTarget）
    ListModel { id: gattModel }

    // ==================== 页面专属逻辑 ====================
    // 打开导出对话框（清空文件名，走 C++ 自动生成带时间戳的默认名）
    function openExportDialog() {
        exportNameField.text = ""
        exportDialog.open()
    }
    // 选择导出目录：Android 弹系统文件夹选择器(SAF)，其余平台弹系统目录对话框
    function chooseExportDir() {
        if (Qt.platform.os === "android") {
            if (!bleManager.terminal.pickFolder())
                app.showToast("当前系统暂不支持选择目录")
            return
        }
        exportFolderDialog.open()
    }
    // 目录选择完成（桌面 FolderDialog 回调；Android 由 folderPicked 信号通知）
    function onExportFolderSelected(url) {
        var s = url ? String(url) : ""
        if (s === "") { app.showToast("未选择目录"); return }
        bleManager.terminal.exportTarget = s
        app.showToast("保存目录: " + bleManager.terminal.exportDisplay)
    }
    // 导出（第一步校验）-> 打开存储空间检测对话框
    function doExport() {
        if (bleManager.terminal.receivedText.trim() === "") {
            app.showToast("接收区暂无数据，无法导出")
            return
        }
        if (bleManager.terminal.exportTarget === "") {
            app.showToast("请先选择保存目录")
            page.chooseExportDir()
            return
        }
        spaceDialog.info = bleManager.terminal.storageInfo()
        exportDialog.close()
        spaceDialog.open()
    }
    // 确认导出（存储空间检测通过后真正写入）
    function confirmExport() {
        var path = bleManager.terminal.saveReceived(exportNameField.text)
        if (path !== "")
            app.showToast("已导出: " + bleManager.terminal.exportDisplay)
        else
            exportDialog.open()   // 写失败（原因已由错误提示展示），回到导出框调整
        spaceDialog.close()
    }
    // 字节数转可读字符串（存储检测展示用）
    function formatBytes(b) {
        if (typeof b !== "number" || b < 0) return "未知"
        if (b < 1024) return b + " B"
        var units = ["KB", "MB", "GB", "TB", "PB"]
        var v = b, i = -1
        do { v /= 1024; i++ } while (v >= 1024 && i < units.length - 1)
        return v.toFixed(2) + " " + units[i]
    }
    // 刷新「服务与特征」列表模型（服务发现/目标变更后由 C++ 信号触发）
    function refreshGattModel() {
        gattModel.clear()
        var arr = bleManager.terminal.charCatalog()
        if (!arr) return
        for (var i = 0; i < arr.length; i++)
            gattModel.append(arr[i])
    }
    // 确认定时发送参数
    function confirmTimerSend() {
        var interval = parseFloat(timerIntervalField.text)
        if (isNaN(interval) || interval <= 0) {
            app.showToast("时间间隔请输入正数")
            return
        }
        var durText = timerDurationField.text.trim()
        var duration = 0
        if (durText !== "" && durText !== "一直") {
            var d = parseFloat(durText)
            if (isNaN(d) || d <= 0) {
                app.showToast("持续时间请输入正数，或填“一直”表示持续发送")
                return
            }
            duration = d
        }
        bleManager.terminal.startTimerSend(timerDialog.unitSel, interval,
                                           duration, sendField.text)
        timerDialog.close()
    }

    // -------- 调试终端相关信号（随页面内聚，与原 Main.qml 行为一致） --------
    Connections {
        target: bleManager.terminal
        function onErrorOccurred(message) {
            app.showToast(message)
        }
        // 自动配置完成：告知用户无需手动开启通知 / 选择特征即可收发
        function onAutoConfigured(services, notifyCount) {
            if (notifyCount > 0)
                app.showToast("已自动配置 " + services + " 个服务并开启 "
                               + notifyCount + " 个通知特征，收到数据将实时显示")
            else
                app.showToast("已自动配置 " + services
                               + " 个服务：发送目标已选定，"
                               + "无通知的特征将自动轮询读取，无需手动点读取")
        }
        function onTimerEnabledChanged() {
            if (!bleManager.terminal.timerEnabled && timerSendBox.checked)
                timerSendBox.checked = false
        }
        // 服务/特征发现或读写/通知目标变化后刷新目录列表
        function onCatalogChanged() {
            page.refreshGattModel()
        }
        // Android SAF 目录选择完成（未选择时 target 为空串）
        function onFolderPicked(target, display) {
            if (target === "")
                return
            app.showToast("保存目录已设置: " + (display === "" ? target : display))
        }
    }

    // -------- 调试页顶栏 --------
    Rectangle {
        id: debugHeader
        width: parent.width
        height: 56
        color: app.cSurface
        z: 10
        Rectangle {
            width: parent.width
            height: 1
            color: app.cDivider
            anchors.bottom: parent.bottom
        }

        // 返回
        ToolButton {
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 44
            implicitHeight: 44
            onClicked: app.currentTab = 0
            contentItem: Text {
                text: "‹"
                font.pixelSize: 30
                color: app.cText
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 52
            anchors.verticalCenter: parent.verticalCenter
            text: "调试终端"
            color: app.cText
            font.pixelSize: 16
            font.bold: true
        }

        // 连接状态徽标
        Rectangle {
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            width: 78
            height: 24
            radius: 12
            color: bleManager.terminal.active
                   ? Qt.rgba(0.18, 0.80, 0.44, 0.15)
                   : Qt.rgba(0.91, 0.30, 0.24, 0.15)
            Text {
                anchors.centerIn: parent
                text: bleManager.terminal.active ? "● 已连接" : "○ 未连接"
                font.pixelSize: 11
                color: bleManager.terminal.active ? app.cGreen : app.cRed
            }
        }
    }

    // -------- 调试主内容 --------
    Item {
        anchors.top: debugHeader.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12

        // -------- 服务与特征（读写特征选择，实现双向收发）--------
        Rectangle {
            id: gattHead
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 34
            radius: 8
            color: app.cSurface
            border.color: app.cDivider

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "服务与特征"
                color: app.cText
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 30
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    // 外设广播模式：不面向远端 GATT，无需也无法手动选择特征
                    if (bleManager.peripheralConnected)
                        return "外设模式 · 收发自动同步"
                    var n = gattModel.count
                    if (n === 0) return "尚未发现"
                    return n + " 个特征 · 已自动收发"
                }
                color: app.cSubtext
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: page.gattOpen ? "▴" : "▾"
                color: app.cAccent
                font.pixelSize: 13
            }
            MouseArea {
                anchors.fill: parent
                onClicked: page.gattOpen = !page.gattOpen
            }
        }

        Rectangle {
            id: gattBody
            visible: page.gattOpen
            anchors.top: gattHead.bottom
            anchors.topMargin: 6
            anchors.left: parent.left
            anchors.right: parent.right
            height: 168
            radius: 8
            color: app.cSurface2
            border.color: app.cDivider
            clip: true

            Text {
                anchors.centerIn: parent
                visible: gattModel.count === 0
                text: bleManager.peripheralConnected
                      ? "外设广播模式\n收到写入即自动显示，无需任何设置"
                      : "正在发现服务与特征…\n发现后自动开启通知 / 轮询读取，无需手动操作"
                color: app.cSubtext
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
            }

            // 自动读取轮询开关：只读型（无 Notify）特征靠它自动收数据。
            // 有通知的特征不受影响，仍走推送通道（此开关只控制轮询）。
            Rectangle {
                id: pollRow
                visible: gattModel.count > 0
                         && !bleManager.peripheralConnected
                anchors.top: parent.top
                anchors.topMargin: 2
                anchors.left: parent.left
                anchors.right: parent.right
                height: 22
                color: "transparent"
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 4
                    anchors.right: parent.right
                    anchors.rightMargin: 44
                    anchors.verticalCenter: parent.verticalCenter
                    text: "自动轮询读取（无通知的特征自动读并显示）"
                    color: app.cSubtext
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }
                Switch {
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    checked: bleManager.terminal.autoReadEnabled
                    onCheckedChanged: bleManager.terminal.autoReadEnabled = checked
                    scale: 0.75
                }
            }

            ListView {
                id: gattList
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: pollRow.visible ? 30 : 2
                anchors.leftMargin: 4
                anchors.rightMargin: 4
                anchors.bottomMargin: 4
                model: gattModel
                spacing: 4
                clip: true
                ScrollBar.vertical: ScrollBar {
                    width: 3
                    policy: ScrollBar.AsNeeded
                }

                delegate: Item {
                    width: gattList.width - 6
                    height: 46
                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: model.isSendTarget
                               ? Qt.rgba(0.00, 0.66, 0.81, 0.12)
                               : app.cSurface
                        border.color: model.isSendTarget
                                      ? app.cAccent : app.cDivider
                    }
                    // 左：特征/服务描述
                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 172
                        spacing: 2
                        Text {
                            width: parent.width
                            text: (model.charName !== ""
                                   ? model.charName + "  " : "")
                                  + model.charUuid
                            color: model.isSendTarget ? app.cAccent
                                                      : app.cText
                            font.pixelSize: 11
                            font.family: "Consolas"
                            font.bold: model.isSendTarget
                            elide: Text.ElideMiddle
                        }
                        Text {
                            width: parent.width
                            text: (model.serviceName !== ""
                                   ? model.serviceName + "  " : "")
                                  + model.serviceUuid
                            color: app.cSubtext
                            font.pixelSize: 9
                            font.family: "Consolas"
                            elide: Text.ElideMiddle
                        }
                    }
                    // 右：读写/通知操作
                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        Rectangle {
                            visible: model.writable
                            width: 52
                            height: 24
                            radius: 4
                            color: model.isSendTarget ? app.cAccent
                                                      : app.cSurface2
                            border.color: model.isSendTarget
                                          ? app.cAccent : app.cDivider
                            Text {
                                anchors.centerIn: parent
                                text: model.isSendTarget ? "✓发送" : "设发送"
                                color: "white"
                                font.pixelSize: 10
                                font.bold: model.isSendTarget
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: bleManager.terminal.setSendTarget(
                                               model.serviceUuid,
                                               model.charUuid)
                            }
                        }
                        Rectangle {
                            visible: model.readable
                            width: 52
                            height: 24
                            radius: 4
                            color: app.cSurface2
                            border.color: app.cDivider
                            Text {
                                anchors.centerIn: parent
                                text: "读取"
                                color: app.cText
                                font.pixelSize: 10
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: bleManager.terminal.readNow(
                                               model.serviceUuid,
                                               model.charUuid)
                            }
                        }
                        Rectangle {
                            visible: model.notifiable
                            width: 54
                            height: 24
                            radius: 4
                            color: model.notifyOn
                                   ? Qt.rgba(0.18, 0.80, 0.44, 0.25)
                                   : app.cSurface2
                            border.color: model.notifyOn
                                          ? app.cGreen : app.cDivider
                            Text {
                                anchors.centerIn: parent
                                text: model.notifyOn ? "通知开" : "通知关"
                                color: model.notifyOn
                                       ? app.cGreen : app.cText
                                font.pixelSize: 10
                                font.bold: model.notifyOn
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: bleManager.terminal.toggleNotify(
                                               model.serviceUuid,
                                               model.charUuid,
                                               !model.notifyOn)
                            }
                        }
                    }
                }
            }
        }

        // 收到的数据
        Text {
            id: recvLabel
            anchors.top: page.gattOpen ? gattBody.bottom : gattHead.bottom
            anchors.topMargin: 10
            anchors.left: parent.left
            anchors.right: parent.right
            text: "收到的数据 (Received)"
            color: app.cSubtext
            font.pixelSize: 13
        }

        Rectangle {
            id: recvBox
            anchors.top: recvLabel.bottom
            anchors.topMargin: 6
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: sendLabel.top
            anchors.bottomMargin: 10
            radius: 8
            color: app.cSurface
            border.color: app.cDivider
            clip: true

            ScrollView {
                id: recvScroll
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                ScrollBar.vertical: ScrollBar {}

                TextArea {
                    id: recvArea
                    width: recvScroll.availableWidth
                    text: bleManager.terminal.receivedText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextArea.Wrap
                    color: app.cText
                    font.family: "Consolas"
                    font.pixelSize: 13
                    background: Item {}
                    // 新数据到达时自动滚动到底部
                    onTextChanged: Qt.callLater(function () {
                        var f = recvScroll.contentItem
                        f.contentY = Math.max(0, f.contentHeight - f.height)
                    })
                }
            }
        }

        // 发送的数据
        Text {
            id: sendLabel
            anchors.bottom: sendBox.top
            anchors.bottomMargin: 6
            anchors.left: parent.left
            anchors.right: parent.right
            text: "发送的数据 (Send)"
            color: app.cSubtext
            font.pixelSize: 13
        }

        Rectangle {
            id: sendBox
            anchors.bottom: btnRow.top
            anchors.bottomMargin: 10
            anchors.left: parent.left
            anchors.right: parent.right
            height: 44
            radius: 8
            color: app.cSurface2
            border.color: app.cDivider

            TextField {
                id: sendField
                anchors.fill: parent
                anchors.margins: 4
                placeholderText: bleManager.terminal.hexSend
                                ? "输入十六进制，如 01 A0 FF"
                                : "输入要发送的内容"
                placeholderTextColor: app.cSubtext
                background: Item {}
                color: app.cText
                font.family: "Consolas"
                font.pixelSize: 14
                selectByMouse: true
            }
        }

        // 导出 / 发送 按钮
        Row {
            id: btnRow
            anchors.bottom: clearRow.top
            anchors.bottomMargin: 10
            anchors.left: parent.left
            anchors.right: parent.right
            height: 42
            spacing: 10

            Rectangle {
                width: (parent.width - parent.spacing) / 2
                height: parent.height
                radius: 8
                color: app.cSurface2
                border.color: app.cDivider
                opacity: bleManager.terminal.active ? 1 : 0.45
                Text {
                    anchors.centerIn: parent
                    text: "导出 (Export)"
                    color: app.cText
                    font.pixelSize: 14
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: bleManager.terminal.active
                    onClicked: page.openExportDialog()
                }
            }

            Rectangle {
                width: (parent.width - parent.spacing) / 2
                height: parent.height
                radius: 8
                color: app.cAccent
                opacity: bleManager.terminal.active ? 1 : 0.45
                Text {
                    anchors.centerIn: parent
                    text: "发送 (Send)"
                    color: "white"
                    font.pixelSize: 14
                    font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: bleManager.terminal.active
                    onClicked: bleManager.terminal.sendText(sendField.text)
                }
            }
        }

        // 清空按钮：清除接收区与发送输入框内容
        Rectangle {
            id: clearRow
            anchors.bottom: chkRow.top
            anchors.bottomMargin: 10
            anchors.left: parent.left
            anchors.right: parent.right
            height: 34
            radius: 8
            color: Qt.rgba(0.91, 0.30, 0.24, 0.12)
            border.color: app.cRed
            opacity: bleManager.terminal.active ? 1 : 0.45
            Text {
                anchors.centerIn: parent
                text: "清空收发记录 (Clear)"
                color: app.cRed
                font.pixelSize: 13
                font.bold: true
            }
            MouseArea {
                anchors.fill: parent
                enabled: bleManager.terminal.active
                onClicked: {
                    bleManager.terminal.clearReceived()
                    sendField.text = ""
                    app.showToast("已清空接收区与发送框")
                }
            }
        }

        // 勾选框（两组独立 Flow：每组内部按需换行，跨组不会溢出重叠）
        // 第 1 组：十六进制发送 / 接收(带空格) / 接收(不带空格)
        // 第 2 组：定时发送 / 时间戳
        // 设计要点：把 5 个按钮分到两个独立 Flow 里，避免单个 Flow 把
        // 后面组别的按钮排进同一行；每个 CheckBox 的 width 都按
        // 「文本实际宽度 + indicator + padding」精确预留，杜绝
        // 「定时发送的勾选框遮住十六进制接收(不带空格)文本」的问题。
        Column {
            id: chkRow
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: chkRow.implicitHeight
            spacing: 6

            // ---- 第 0 组：「绑定此设备」独立行（功能较重，单独一行更醒目）----
            // 勾选后把当前连接的设备地址/名称写入仓库，下一次扫描到时自动连接；
            // 取消勾选时如果之前绑定过，自动解绑，与「绑定」页的解除绑定按钮等效。
            CheckBox {
                id: bindCurrentBox
                width: 116
                height: 26
                text: "绑定此设备"
                checked: page.bindCurrentChecked
                enabled: bleManager.terminal.active
                          || (bleManager.connected && bleManager.targetAddress !== "")
                onToggled: {
                    page.bindCurrentChecked = checked
                    var ctl = bleManager.checkboxControl
                    if (!ctl) return
                    // 当前连接目标地址由 Main.qml 的 root.targetAddress 管理
                    var addr = String(app.targetAddress || "").trim()
                    if (checked) {
                        // 必须有连接地址才允许绑定；否则不落库
                        if (addr === "") {
                            checked = false
                            page.bindCurrentChecked = false
                            app.showToast("请先连接一个设备")
                            return
                        }
                        // 同步记录当前被勾选绑定的设备地址（小写归一化），
                        // 让 BindPage 解除绑定时能精准匹配。
                        page.currentBoundAddress = addr.toLowerCase()
                        // 设备名：优先从 Main.qml 的扫描列表中查找，
                        // 查不到则用 MAC 截断作为占位（避免空名入库）。
                        var nm = "N/A"
                        try {
                            if (app && app.deviceModel) {
                                for (var i = 0; i < app.deviceModel.count; i++) {
                                    var it = app.deviceModel.get(i)
                                    if (it && String(it.address || "")
                                            .toLowerCase() === addr.toLowerCase()) {
                                        nm = String(it.name || "").trim()
                                        break
                                    }
                                }
                            }
                        } catch (e) { nm = "N/A" }
                        if (!nm || nm === "") {
                            // 用 MAC 末 5 位做占位名（保留可读性）
                            nm = addr.length >= 5
                                  ? ("设备-" + addr.substr(addr.length - 5).toUpperCase())
                                  : "设备"
                        }
                        if (ctl.bindDevice(nm, addr)) {
                            ctl.bindCurrentDevice = true
                            app.showToast("已绑定当前设备：" + nm)
                        } else {
                            checked = false
                            page.bindCurrentChecked = false
                            app.showToast("绑定失败")
                        }
                    } else {
                        // 取消勾选：解除绑定（与「绑定」页解除绑定按钮等效）
                        if (addr !== "" && ctl.isBound(addr)) {
                            ctl.unbindDevice(addr)
                            app.showToast("已解除绑定当前设备")
                        }
                        ctl.bindCurrentDevice = false
                        // 同步清空「当前勾选地址」，避免下次勾选别的设备时残留旧地址
                        page.currentBoundAddress = ""
                    }
                }
                indicator: Rectangle {
                    implicitWidth: 18
                    implicitHeight: 18
                    radius: 4
                    color: parent.checked ? app.cAccent : app.cSurface2
                    border.color: parent.checked ? app.cAccent : app.cDivider
                    Text {
                        anchors.centerIn: parent
                        text: "✓"
                        visible: parent.parent.checked
                        color: "white"
                        font.pixelSize: 12
                    }
                }
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 12
                    color: app.cText
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }

            // ---- 第 1 组：三个十六进制相关开关 ----
            Flow {
                width: parent.width
                spacing: 6
            CheckBox {
                id: hexSendBox
                width: 100
                height: 26
                text: "十六进制发送"
                checked: bleManager.terminal.hexSend
                onToggled: {
                    bleManager.terminal.hexSend = checked
                    // 持久化到仓库，下次启动自动还原
                    if (bleManager.checkboxControl)
                        bleManager.checkboxControl.hexSend = checked
                }
                indicator: Rectangle {
                    implicitWidth: 18
                    implicitHeight: 18
                    radius: 4
                    color: parent.checked ? app.cAccent : app.cSurface2
                    border.color: parent.checked ? app.cAccent : app.cDivider
                    Text {
                        anchors.centerIn: parent
                        text: "✓"
                        visible: parent.parent.checked
                        color: "white"
                        font.pixelSize: 12
                    }
                }
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 12
                    color: app.cText
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }

            CheckBox {
                id: hexRecvBox
                width: 142
                height: 26
                text: "十六进制接收(带空格)"
                checked: bleManager.terminal.hexReceive
                onToggled: {
                    bleManager.terminal.hexReceive = checked
                    // 互斥：勾选带空格时取消无空格
                    if (checked) {
                        bleManager.terminal.hexReceiveNoSpace = false
                        hexRecvNoSpaceBox.checked = false
                    }
                    // 持久化到仓库（两个接收开关是互斥的，只需持久化当前状态）
                    var ctl = bleManager.checkboxControl
                    if (!ctl) return
                    ctl.hexReceiveSpaced = checked
                    if (checked) ctl.hexReceiveNoSpace = false
                }
                indicator: Rectangle {
                    implicitWidth: 18
                    implicitHeight: 18
                    radius: 4
                    color: parent.checked ? app.cAccent : app.cSurface2
                    border.color: parent.checked ? app.cAccent : app.cDivider
                    Text {
                        anchors.centerIn: parent
                        text: "✓"
                        visible: parent.parent.checked
                        color: "white"
                        font.pixelSize: 12
                    }
                }
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 12
                    color: app.cText
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }

            CheckBox {
                id: hexRecvNoSpaceBox
                width: 158
                height: 26
                text: "十六进制接收(不带空格)"
                checked: bleManager.terminal.hexReceiveNoSpace
                onToggled: {
                    bleManager.terminal.hexReceiveNoSpace = checked
                    // 互斥：勾选不带空格时取消带空格
                    if (checked) {
                        bleManager.terminal.hexReceive = false
                        hexRecvBox.checked = false
                    }
                    // 持久化到仓库（两个接收开关是互斥的，只需持久化当前状态）
                    var ctl = bleManager.checkboxControl
                    if (!ctl) return
                    ctl.hexReceiveNoSpace = checked
                    if (checked) ctl.hexReceiveSpaced = false
                }
                indicator: Rectangle {
                    implicitWidth: 18
                    implicitHeight: 18
                    radius: 4
                    color: parent.checked ? app.cAccent : app.cSurface2
                    border.color: parent.checked ? app.cAccent : app.cDivider
                    Text {
                        anchors.centerIn: parent
                        text: "✓"
                        visible: parent.parent.checked
                        color: "white"
                        font.pixelSize: 12
                    }
                }
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 12
                    color: app.cText
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
            }

            // ---- 第 2 组：定时发送 + 时间戳 ----
            Flow {
                width: parent.width
                spacing: 6

            CheckBox {
                id: timerSendBox
                width: 80
                height: 26
                text: "定时发送"
                onToggled: {
                    if (checked) {
                        if (!bleManager.terminal.active) {
                            checked = false
                            app.showToast("请先连接蓝牙")
                            return
                        }
                        timerDialog.open()
                    } else {
                        bleManager.terminal.stopTimerSend()
                    }
                }
                indicator: Rectangle {
                    implicitWidth: 18
                    implicitHeight: 18
                    radius: 4
                    color: parent.checked ? app.cAccent : app.cSurface2
                    border.color: parent.checked ? app.cAccent : app.cDivider
                    Text {
                        anchors.centerIn: parent
                        text: "✓"
                        visible: parent.parent.checked
                        color: "white"
                        font.pixelSize: 12
                    }
                }
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 12
                    color: app.cText
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }

            CheckBox {
                id: tsBox
                width: 72
                height: 26
                text: "时间戳"
                checked: bleManager.terminal.timestampEnabled
                onToggled: {
                    bleManager.terminal.timestampEnabled = checked
                    // 持久化到仓库
                    if (bleManager.checkboxControl)
                        bleManager.checkboxControl.timestampEnabled = checked
                }
                indicator: Rectangle {
                    implicitWidth: 18
                    implicitHeight: 18
                    radius: 4
                    color: parent.checked ? app.cAccent : app.cSurface2
                    border.color: parent.checked ? app.cAccent : app.cDivider
                    Text {
                        anchors.centerIn: parent
                        text: "✓"
                        visible: parent.parent.checked
                        color: "white"
                        font.pixelSize: 12
                    }
                }
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 12
                    color: app.cText
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
            }
        }

        // -------- 未连接时的提示层（覆盖整个内容区）--------
        Rectangle {
            anchors.fill: parent
            visible: !bleManager.terminal.active
            color: Qt.rgba(0.07, 0.08, 0.10, 0.62)
            radius: 8
            Column {
                anchors.centerIn: parent
                spacing: 10
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "🔌"
                    font.pixelSize: 40
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "请先连接蓝牙"
                    color: app.cText
                    font.pixelSize: 16
                    font.bold: true
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "在扫描页点击设备连接，或开启广播等待其它主设备连接"
                    color: app.cSubtext
                    font.pixelSize: 12
                }
            }
        }
    }

    // =================================================================
    // 导出对话框（调试页：接收数据导出为 txt）
    // 注意：header 属性是 Dialog 专有（Dialog 继承自 Popup），
    // 若用 Popup 声明会导致 QML 加载失败、程序启动闪退。
    // =================================================================
    Dialog {
        id: exportDialog
        anchors.centerIn: parent
        modal: true
        width: Math.min(app.width - 32, 360)
        padding: 16

        background: Rectangle { color: app.cSurface; radius: 12 }
        header: Rectangle {
            width: parent.width
            height: 48
            color: "transparent"
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: "导出接收数据 (Export)"
                color: app.cText
                font.pixelSize: 16
                font.bold: true
            }
        }

        contentItem: Column {
            spacing: 10
            Text {
                text: "保存位置（自定义，导出后手机文件管理可直接找到）"
                color: app.cSubtext
                font.pixelSize: 13
                wrapMode: Text.Wrap
                width: parent.width
            }
            // 自定义目录选择
            Rectangle {
                width: parent.width
                height: 44
                radius: 8
                color: app.cSurface2
                border.color: app.cDivider
                clip: true
                Rectangle {
                    id: exportDirPickBtn
                    anchors.left: parent.left
                    anchors.leftMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    width: 86
                    height: 30
                    radius: 6
                    color: app.cAccent
                    Text {
                        anchors.centerIn: parent
                        text: "选择目录"
                        color: "white"
                        font.pixelSize: 12
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: page.chooseExportDir()
                    }
                }
                Text {
                    id: exportDirText
                    anchors.left: exportDirPickBtn.right
                    anchors.leftMargin: 8
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: bleManager.terminal.exportDisplay === ""
                          ? "未选择，点击「选择目录」"
                          : bleManager.terminal.exportDisplay
                    color: bleManager.terminal.exportDisplay === ""
                           ? app.cSubtext : app.cText
                    font.pixelSize: 12
                    font.family: "Consolas"
                    elide: Text.ElideLeft
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Text { text: "文件名"; color: app.cSubtext; font.pixelSize: 13 }
            Rectangle {
                width: parent.width
                height: 40
                radius: 8
                color: app.cSurface2
                border.color: app.cDivider
                TextField {
                    id: exportNameField
                    anchors.fill: parent
                    anchors.margins: 2
                    placeholderText: "留空自动生成"
                    placeholderTextColor: app.cSubtext
                    background: Item {}
                    color: app.cText
                    font.pixelSize: 14
                }
            }
            Text {
                width: parent.width
                text: "点击「导出」前将自动检测可用空间"
                color: app.cSubtext
                font.pixelSize: 11
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10
                Rectangle {
                    width: 110
                    height: 38
                    radius: 8
                    color: app.cSurface2
                    border.color: app.cDivider
                    Text {
                        anchors.centerIn: parent
                        text: "取消"
                        color: app.cText
                        font.pixelSize: 14
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: exportDialog.close()
                    }
                }
                Rectangle {
                    width: 110
                    height: 38
                    radius: 8
                    color: app.cAccent
                    Text {
                        anchors.centerIn: parent
                        text: "导出"
                        color: "white"
                        font.pixelSize: 14
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: page.doExport()
                    }
                }
            }
        }
    }

    // 桌面平台的系统目录选择对话框（Android 走 terminal.pickFolder → SAF）
    FolderDialog {
        id: exportFolderDialog
        title: "选择导出保存目录"
        onAccepted: page.onExportFolderSelected(exportFolderDialog.selectedFolder)
    }

    // 导出前存储空间检测提示框
    Dialog {
        id: spaceDialog
        anchors.centerIn: parent
        modal: true
        width: Math.min(app.width - 32, 340)
        padding: 16

        property var info: ({ totalBytes: -1, freeBytes: -1,
                              neededBytes: 0, enough: true })

        background: Rectangle { color: app.cSurface; radius: 12 }
        header: Rectangle {
            width: parent.width
            height: 48
            color: "transparent"
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: "导出前存储检测"
                color: app.cText
                font.pixelSize: 16
                font.bold: true
            }
        }

        contentItem: Column {
            spacing: 10
            Text {
                width: parent.width
                text: "保存位置: " + (bleManager.terminal.exportDisplay === ""
                                     ? "（未选择）" : bleManager.terminal.exportDisplay)
                color: app.cSubtext
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            Rectangle {
                width: parent.width
                height: 108
                radius: 8
                color: app.cSurface2
                border.color: app.cDivider
                Column {
                    anchors.centerIn: parent
                    spacing: 8
                    Row {
                        width: 250
                        Text { text: "手机可用总空间："; color: app.cSubtext; font.pixelSize: 13 }
                        Text {
                            text: page.formatBytes(spaceDialog.info.totalBytes)
                            color: app.cText; font.pixelSize: 13; font.bold: true
                        }
                    }
                    Row {
                        width: 250
                        Text { text: "当前可用空间："; color: app.cSubtext; font.pixelSize: 13 }
                        Text {
                            text: page.formatBytes(spaceDialog.info.freeBytes)
                            color: spaceDialog.info.enough ? app.cGreen : app.cRed
                            font.pixelSize: 13; font.bold: true
                        }
                    }
                    Row {
                        width: 250
                        Text { text: "所需空间大小："; color: app.cSubtext; font.pixelSize: 13 }
                        Text {
                            text: page.formatBytes(spaceDialog.info.neededBytes)
                            color: app.cText; font.pixelSize: 13; font.bold: true
                        }
                    }
                }
            }
            Text {
                width: parent.width
                visible: !spaceDialog.info.enough
                text: "可用空间不足，请先清理手机存储或更换保存目录"
                color: app.cRed
                font.pixelSize: 12
                font.bold: true
                wrapMode: Text.Wrap
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10
                Rectangle {
                    width: 110
                    height: 38
                    radius: 8
                    color: app.cSurface2
                    border.color: app.cDivider
                    Text {
                        anchors.centerIn: parent
                        text: "取消"
                        color: app.cText
                        font.pixelSize: 14
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            spaceDialog.close()
                            exportDialog.open()
                        }
                    }
                }
                Rectangle {
                    width: 120
                    height: 38
                    radius: 8
                    color: spaceDialog.info.enough ? app.cAccent : app.cSurface2
                    Text {
                        anchors.centerIn: parent
                        text: "确认导出"
                        color: spaceDialog.info.enough ? "white" : app.cSubtext
                        font.pixelSize: 14
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: spaceDialog.info.enough
                        onClicked: page.confirmExport()
                    }
                }
            }
        }
    }

    // =================================================================
    // 定时发送设置对话框（调试页）
    // 注意：header 属性是 Dialog 专有（Dialog 继承自 Popup），
    // 若用 Popup 声明会导致 QML 加载失败、程序启动闪退。
    // =================================================================
    Dialog {
        id: timerDialog
        anchors.centerIn: parent
        modal: true
        width: Math.min(app.width - 32, 340)
        padding: 16

        property string unitSel: "秒"

        background: Rectangle { color: app.cSurface; radius: 12 }
        header: Rectangle {
            width: parent.width
            height: 48
            color: "transparent"
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: "定时发送设置"
                color: app.cText
                font.pixelSize: 16
                font.bold: true
            }
        }

        contentItem: Column {
            spacing: 12
            // 时间单位
            Text { text: "时间单位"; color: app.cSubtext; font.pixelSize: 13 }
            Row {
                spacing: 6
                Repeater {
                    model: ["秒", "毫秒", "分"]
                    Rectangle {
                        width: 64
                        height: 30
                        radius: 15
                        color: timerDialog.unitSel === modelData ? app.cAccent : app.cSurface2
                        Text {
                            anchors.centerIn: parent
                            text: modelData
                            color: "white"
                            font.pixelSize: 12
                            font.bold: timerDialog.unitSel === modelData
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: timerDialog.unitSel = modelData
                        }
                    }
                }
            }
            // 时间间隔
            Text { text: "时间间隔"; color: app.cSubtext; font.pixelSize: 13 }
            Rectangle {
                width: parent.width
                height: 40
                radius: 8
                color: app.cSurface2
                border.color: app.cDivider
                TextField {
                    id: timerIntervalField
                    anchors.fill: parent
                    anchors.margins: 2
                    text: "1"
                    placeholderText: "默认 1"
                    placeholderTextColor: app.cSubtext
                    background: Item {}
                    color: app.cText
                    font.pixelSize: 14
                    inputMethodHints: Qt.ImhDigitsOnly
                }
            }
            // 持续时间
            Text { text: "持续时间（“一直”= 持续发送，可填数字如 5）"
                   color: app.cSubtext; font.pixelSize: 13 }
            Rectangle {
                width: parent.width
                height: 40
                radius: 8
                color: app.cSurface2
                border.color: app.cDivider
                TextField {
                    id: timerDurationField
                    anchors.fill: parent
                    anchors.margins: 2
                    text: "一直"
                    placeholderText: "例如 5 = 按当前单位持续 5 个"
                    placeholderTextColor: app.cSubtext
                    background: Item {}
                    color: app.cText
                    font.pixelSize: 14
                }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10
                Rectangle {
                    width: 110
                    height: 38
                    radius: 8
                    color: app.cSurface2
                    border.color: app.cDivider
                    Text {
                        anchors.centerIn: parent
                        text: "取消"
                        color: app.cText
                        font.pixelSize: 14
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            timerDialog.close()
                            timerSendBox.checked = false
                        }
                    }
                }
                Rectangle {
                    width: 110
                    height: 38
                    radius: 8
                    color: app.cAccent
                    Text {
                        anchors.centerIn: parent
                        text: "开始"
                        color: "white"
                        font.pixelSize: 14
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: page.confirmTimerSend()
                    }
                }
            }
        }
    }
}
