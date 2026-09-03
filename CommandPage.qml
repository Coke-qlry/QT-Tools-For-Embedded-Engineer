import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// =====================================================================
// 自定义指令页（底部导航第 4 个 Tab）
// 指令数据以 SQLite 数据库 ble_command_config 为唯一事实来源：
//   * 表结构：command_name(指令名称) / command_self(指令内容)
//   * 启动时由 Main.qml 调用 initFromSqlite()：
//     库不存在 → 自动建库并用下方「出厂默认指令」初始化；
//     库已存在 → 直接加载用户上次保存的配置。
//   * 页内增/删/改之后会即时整体写回数据库，
//     因此用户自定义的指令在下一次启动仍然保留。
// 主题色与 Toast 经 app（Main.qml 根 Window）访问。
// 顶栏下方为「信息/日志文本框」（放大版）：彩色分行显示三类内容——
//   ① 增删改查动作与数据库状态（[BLE-SAR]: 前缀，最多保留 300 行）；
//   ② 连接信息：连接成功绿字「已连接到蓝牙…」+ 设备信息
//      （MAC地址 / 服务UUID / 信号强度，打印逻辑在 Main.qml）；
//   ③ 收发数据流：监听 BleTerminal::dataLogged，与调试文本框同源镜像
//      （[接收]: 青色 / [发送]: 绿色），十六进制/时间戳开关同样生效。
// 下方指令列表：单击一条指令 = 把该条 command_self 实际内容直接发送到
// 已连接的蓝牙设备（未连接时提示，见 sendCommand）。
// =====================================================================
Item {
    id: page
    property var app

    // 指令查找的起始游标（查找后从下一条继续，实现循环定位）
    property int lastFound: -1

    // ---- 出厂默认指令（仅在数据库首次创建时写入一次）----
    readonly property var factoryCommands: [
        { name: "读取电量",    command: "AT+VBAT?" },
        { name: "读取SAR值",   command: "AT+SAR?" },
        { name: "启动扫描",    command: "AT+SCAN=1" },
        { name: "停止扫描",    command: "AT+SCAN=0" },
        { name: "设置阈值",    command: "AT+THRESH=100" },
        { name: "重启设备",    command: "AT+RST" }
    ]

    // ---- 操作日志（增删改查动作 + 数据库状态，显示在顶栏下方的文本框）----
    ListModel { id: logModel }

    // 向日志文本框追加一行记录；超过 300 行时自动裁剪最旧内容。
    // color 缺省取正文色，调用方可用 app.cGreen(成功)/app.cRed(失败)/
    // app.cOrange(警告)/app.cAccent(信息) 标识不同性质的记录。
    function appendLog(line, color) {
        logModel.append({ line: line, color: color || app.cText })
        while (logModel.count > 300) logModel.remove(0, 100)
        Qt.callLater(function () {
            if (logList.count > 0) logList.positionViewAtEnd()
        })
    }
    // 清空下方日志文本框中的全部内容（状态条右侧「清空」按钮触发）
    function clearLog() {
        logModel.clear()
        app.showToast("已清空信息与日志")
    }

    // ---- 自定义指令数据（运行时列表，启动时从数据库加载）----
    ListModel {
        id: instructionModel
    }

    // ============ SQLite 持久化 ============
    // 启动初始化：确保库与 config 表就绪；仅当本次真正“新建了表”时写入
    // 出厂默认指令；随后把数据库内容加载到列表。
    // 注意：判断依据是「config 表是否已建好」，而不是「.db 文件是否存在」。
    // 因为旧版本崩溃可能留下「文件在、表没建成」的半成品库，此时若按文件
    // 判断会跳过初始化 → 页面永远空白、增删改也全部写库失败。
    function initFromSqlite() {
        var dbName = "ble_command_config"
        try {
            console.log("[CommandPage] initFromSqlite 开始, db=" + dbName)
            // ① 确保库文件存在（已存在则幂等复用，不会重复创建）
            var ok1 = sqliteWarehouse.create_sqlite_warehouse(dbName)
            console.log("[CommandPage] create_sqlite_warehouse → " + ok1)
            // ② config 表缺失（字段列表为空）→ 补建表结构；已存在且一致则原样复用
            var fields = sqliteWarehouse.sqlite_wh_fields(dbName)
            console.log("[CommandPage] sqlite_wh_fields 当前字段数 = " + fields.length
                        + " (字段: " + fields.join(",") + ")")
            var needSeed = fields.length === 0
            if (needSeed) {
                // 多值统一用 \x1F 拼接成单字符串传入（_sep 版），
                // 不经 JSON.stringify，规避真机 qmlcache 下内容错乱的问题
                var sepFields = "command_name\u001Fcommand_self"
                console.log("[CommandPage] 建表字段原文=[" + sepFields + "]")
                var ok2 = sqliteWarehouse.create_sqlite_wh_config_sep(dbName, sepFields)
                console.log("[CommandPage] create_sqlite_wh_config → " + ok2)
            }
            // ③ 加载已有数据
            loadFromSqlite()
            console.log("[CommandPage] 初次加载后列表条数 = " + instructionModel.count)
            // ④ 只有本次确实新建了表，才补入「出厂默认指令」；
            //    用户之后手动清空全部指令不会被再次回填
            if (needSeed) {
                for (var i = 0; i < factoryCommands.length; i++) {
                    var ok3 = sqliteWarehouse.add_sqlite_wh_config_sep(
                                dbName, factoryCommands[i].name
                                       + "\u001F" + factoryCommands[i].command)
                    console.log("[CommandPage] 写入出厂指令[" + i + "] " + factoryCommands[i].name
                                + " → " + ok3)
                }
                loadFromSqlite()
                console.log("[CommandPage] 补种后列表条数 = " + instructionModel.count)
                appendLog("[BLE-SAR]:已写入 " + factoryCommands.length
                          + " 条出厂默认指令", app.cGreen)
            }
            // 汇总数据库状态到日志文本框
            var fieldText = needSeed ? "command_name,command_self" : fields.join(",")
            appendLog("[BLE-SAR]:" + (needSeed ? "数据库创建成功" : "数据库已就绪")
                      + " · 字段[" + fieldText + "] · 当前 "
                      + instructionModel.count + " 条指令", app.cAccent)
            console.log("[CommandPage] initFromSqlite 完成")
        } catch (e) {
            console.log("[CommandPage] initFromSqlite 异常: " + e)
            appendLog("[BLE-SAR]:数据库初始化异常 " + e, app.cRed)
        }
    }
    // 从数据库加载全部指令到列表（每次启动/需要刷新时调用）
    function loadFromSqlite() {
        try {
            var rows = sqliteWarehouse.sqlite_wh_records("ble_command_config")
            console.log("[CommandPage] loadFromSqlite 读到 " + rows.length + " 行")
            instructionModel.clear()
            for (var i = 0; i < rows.length; i++) {
                instructionModel.append({ name: rows[i].command_name,
                                          command: rows[i].command_self })
            }
            console.log("[CommandPage] 列表填充后 count = " + instructionModel.count)
        } catch (e) {
            console.log("[CommandPage] loadFromSqlite 异常: " + e)
        }
    }
    // 把当前列表整体写回数据库（增/删/改后调用，保证数据库与界面一致）。
    // 返回 true 表示已整体落库；返回 false 表示数据库写入失败（此时界面
    // 修改仅存在于内存，重启后丢失），调用方据此提示，避免误报成功。
    function syncToSqlite() {
        sqliteWarehouse.delete_sqlite_wh_all("ble_command_config")
        for (var i = 0; i < instructionModel.count; i++) {
            var it = instructionModel.get(i)
            console.log("[CommandPage] syncToSqlite 写入第 " + (i + 1) + "/"
                        + instructionModel.count + " 条，名称=[" + it.name
                        + "] 内容=[" + it.command + "]")
            if (!sqliteWarehouse.add_sqlite_wh_config_sep(
                        "ble_command_config",
                        it.name + "\u001F" + it.command)) {
                appendLog("[BLE-SAR]:数据库写入失败，重启后本次修改可能丢失", app.cRed)
                app.showToast("数据库写入失败，重启后本次修改可能丢失")
                return false
            }
        }
        return true
    }

    // ---- 增删改查 ----
    // 新增：弹出空表单
    function openAddDialog() {
        editDialog.isNew = true
        editDialog.titleText = "新增指令 (Add)"
        editNameField.text = ""
        editCmdField.text = ""
        appendLog("[BLE-SAR]:已打开「新增指令」对话框，填写名称与内容后点确定写入数据库", app.cAccent)
        editDialog.open()
    }
    // 修改：选中后单击右上角修改图标
    function openEditDialog() {
        var idx = commandList.currentIndex
        if (idx < 0) {
            appendLog("[BLE-SAR]:请先在列表中选中一条指令再修改", app.cOrange)
            app.showToast("请先选中一条指令")
            return
        }
        var it = instructionModel.get(idx)
        editDialog.isNew = false
        editDialog.titleText = "修改指令 (Edit)"
        editNameField.text = it.name
        editCmdField.text = it.command
        appendLog("[BLE-SAR]:已打开「修改指令」对话框：[" + it.name + "]", app.cAccent)
        editDialog.open()
    }
    // 保存（新增/修改共用）
    function saveEdit() {
        console.log("[CommandPage] saveEdit 被调用")
        var n = editNameField.text.trim()
        var c = editCmdField.text.trim()
        console.log("[CommandPage] 名称=[" + n + "] 内容=[" + c + "]")
        if (n === "" && c === "") {
            console.log("[CommandPage] 名称与内容都为空，中止")
            appendLog("[BLE-SAR]:已取消保存，名称与内容不能同时为空", app.cOrange)
            // modal 弹窗会盖住底部 toast，必须在对话框内部就地提示
            editErrText.text = "名称与指令内容不能同时为空，请先填写"
            editErrText.visible = true
            app.showToast("名称与内容不能同时为空")
            return
        }
        // 校验通过：清除就地错误提示
        editErrText.visible = false
        // 显示用名称：为空时给出占位，避免日志出现空方括号
        var displayName = (n === "") ? "未命名" : n
        try {
            var isNew = editDialog.isNew
            if (isNew) {
                instructionModel.append({ name: n, command: c })
                console.log("[CommandPage] 已 append，列表 count = " + instructionModel.count)
            } else {
                instructionModel.set(commandList.currentIndex, { name: n, command: c })
            }
            var ok = page.syncToSqlite()
            console.log("[CommandPage] syncToSqlite → " + ok)
            editDialog.close()
            Qt.inputMethod.hide()
            if (ok) {
                // 写库成功：按动作类型记一条操作日志
                appendLog(isNew
                          ? "[BLE-SAR]:已添加[" + displayName + "]:" + c
                          : "[BLE-SAR]:已修改[" + displayName + "]:" + c,
                          app.cGreen)
                // 让列表立刻滚到刚新增/修改的行并高亮，给出明确视觉反馈
                if (isNew) {
                    commandList.currentIndex = instructionModel.count - 1
                    commandList.positionViewAtIndex(commandList.currentIndex, ListView.Contain)
                } else if (commandList.currentIndex >= 0) {
                    commandList.positionViewAtIndex(commandList.currentIndex, ListView.Contain)
                }
                app.showToast(isNew ? "已新增指令" : "已保存修改")
            } else {
                // 写库失败：syncToSqlite 内部已记录失败日志，此处仅保留原提示
                app.showToast("已修改（但数据库写入失败，重启后会丢失）")
            }
        } catch (e) {
            console.log("[CommandPage] saveEdit 异常: " + e)
        }
    }
    // 删除按钮：短按删选中（同步删除数据库对应记录）
    function deleteSelectedCommand() {
        var idx = commandList.currentIndex
        if (idx < 0) {
            appendLog("[BLE-SAR]:请先在列表中选中一条指令再删除", app.cOrange)
            app.showToast("请先选中一条指令")
            return
        }
        var it = instructionModel.get(idx)
        var displayName = (it.name === "") ? "未命名" : it.name
        instructionModel.remove(idx)
        commandList.currentIndex = Math.min(idx, instructionModel.count - 1)
        var ok = page.syncToSqlite()
        appendLog(ok ? "[BLE-SAR]:已删除[" + displayName + "]:" + it.command
                     : "[BLE-SAR]:数据库写入失败，重启后本次修改可能丢失",
                  ok ? app.cGreen : app.cRed)
        app.showToast(ok ? "已删除选中指令" : "已删除（但数据库写入失败，重启后会恢复）")
    }
    // 删除按钮：长按 2 秒删全部（同步清空数据库，仅保留库结构）
    function deleteAllCommands() {
        var total = instructionModel.count
        instructionModel.clear()
        commandList.currentIndex = -1
        var ok = page.syncToSqlite()
        appendLog(ok ? "[BLE-SAR]:已删除全部指令(" + total + ")"
                     : "[BLE-SAR]:数据库写入失败，重启后本次修改可能丢失",
                  ok ? app.cGreen : app.cRed)
        app.showToast(ok ? "已删除全部指令" : "已删除（但数据库写入失败，重启后会恢复）")
    }
    // 单击指令项：把该条指令的 command_self 实际内容直接发送到已连接的蓝牙。
    // 未连接时仅提示并记录，不发送；连接后（中央/外设模式）立即发出并留日志。
    function sendCommand(index) {
        var it = instructionModel.get(index)
        if (!it) return
        if (!bleManager.terminal.active) {
            appendLog("[BLE-SAR]:尚未连接蓝牙，无法发送指令[" + it.name + "]", app.cOrange)
            app.showToast("请先连接蓝牙")
            return
        }
        // 强制按文本发送（AT 指令为 ASCII），不受调试页“十六进制发送”开关影响
        bleManager.terminal.sendAsciiText(it.command)
        appendLog("[BLE-SAR]:已发送指令[" + it.name + "] → " + it.command, app.cGreen)
        app.showToast("已发送: " + it.name)
    }
    // 对话框避让软键盘：Android 上软键盘弹出时不会自动把 modal Dialog 顶起，
    // 底部按钮会被键盘遮住导致点不到。此处键盘可见时把对话框整体上移，
    // 使其下缘位于键盘上方（保留 12 逻辑像素间距），隐藏时恢复垂直居中。
    // 注意：Qt 6 Android 的 keyboardRectangle 返回物理像素（Java 端未按 DPR
    // 换算，Qt 6.10.3 源码 androidjniinput 实测），必须除以 devicePixelRatio；
    // 桌面/其它平台已是逻辑单位，直接使用。
    function dialogTop(dlg) {
        var centerY = Math.max(8, (page.height - dlg.height) / 2)
        if (!Qt.inputMethod.visible) return centerY
        var kbRaw = Qt.inputMethod.keyboardRectangle.height
        var kbH = (Qt.platform.os === "android")
                  ? kbRaw / Screen.devicePixelRatio : kbRaw
        // 下缘贴到键盘上方；若键盘过高导致放不下，则贴顶保底（按钮仍在可见区）
        var topLimit = page.height - kbH - 12 - dlg.height
        return Math.max(8, Math.min(centerY, topLimit))
    }
    // 水平居中 + 按键盘状态定位（对话框打开时与键盘几何变化时调用）。
    // 注意 Popup 的 anchors 组只有 centerIn，故 x/y 均显式计算。
    function repositionDialog(dlg) {
        dlg.x = Math.max(8, (page.width - dlg.width) / 2)
        dlg.y = page.dialogTop(dlg)
    }
    // 键盘弹出/收起/切换高度时，给所有已打开的对话框重新定位
    function syncDialogPositions() {
        if (editDialog.opened) page.repositionDialog(editDialog)
        if (findDialog.opened) page.repositionDialog(findDialog)
    }
    // 查找：输入关键字后跳转定位。找到返回 true，未找到返回 false。
    function locateCommands(keyword) {
        keyword = keyword.trim()
        if (keyword === "") return false
        var total = instructionModel.count
        var start = (lastFound + 1) % Math.max(total, 1)
        for (var i = 0; i < total; i++) {
            var idx = (start + i) % total
            var it = instructionModel.get(idx)
            if (it.name.indexOf(keyword) >= 0 || it.command.indexOf(keyword) >= 0) {
                page.lastFound = idx
                commandList.currentIndex = idx
                commandList.positionViewAtIndex(idx, ListView.Center)
                appendLog("[BLE-SAR]:已定位[" + it.name + "]", app.cAccent)
                app.showToast("已定位: " + it.name)
                return true
            }
        }
        appendLog("[BLE-SAR]:未找到包含「" + keyword + "」的指令", app.cOrange)
        app.showToast("未找到包含「" + keyword + "」的指令")
        return false
    }
    // 查找按钮统一入口：结果就地反馈在对话框内，找不到时不关闭对话框
    function doFind() {
        Qt.inputMethod.hide()
        var kw = findField.text.trim()
        findErrText.visible = false
        if (kw === "") {
            findErrText.text = "请输入名称或指令关键字"
            findErrText.visible = true
            return
        }
        if (page.locateCommands(kw)) {
            findDialog.close()
        } else {
            findErrText.text = "未找到包含「" + kw + "」的指令"
            findErrText.visible = true
        }
    }

    // -------- 指令页顶栏 --------
    Rectangle {
        id: cmdHeader
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
            text: "自定义指令"
            color: app.cText
            font.pixelSize: 16
            font.bold: true
        }

        // 连接状态已下沉到 cmdHeader 与日志框之间的独立状态条(statusBar)

        // -------- 右上角：增 删 改 查 --------
        ToolButton {
            id: addBtn
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 60
            implicitHeight: 52
            onClicked: page.openAddDialog()
            contentItem: Column {
                spacing: 2
                anchors.centerIn: parent
                Text {
                    text: "＋"
                    width: 60
                    height: 18
                    font.pixelSize: 15
                    color: app.cAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                Text {
                    text: "增(Add)"
                    width: 60
                    height: 12
                    font.pixelSize: 9
                    color: app.cText
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
            onClicked: page.openEditDialog()
            contentItem: Column {
                spacing: 2
                anchors.centerIn: parent
                Text {
                    text: "✎"
                    width: 60
                    height: 18
                    font.pixelSize: 15
                    color: app.cAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                Text {
                    text: "改(Edit)"
                    width: 60
                    height: 12
                    font.pixelSize: 9
                    color: app.cText
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
                if (delHoldTimer.running) { delHoldTimer.stop(); page.deleteSelectedCommand() }
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
                    page.deleteAllCommands()
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
                    color: delBtn.colorOverlay ? app.cRed : app.cText
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                Text {
                    text: "删(Del)"
                    width: 60
                    height: 12
                    font.pixelSize: 9
                    color: app.cText
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
                    color: app.cAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                Text {
                    text: "查(Find)"
                    width: 60
                    height: 12
                    font.pixelSize: 9
                    color: app.cText
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    // -------- 状态条：连接状态 + 清空日志按钮（标题栏与日志文本框之间） --------
    Rectangle {
        id: statusBar
        width: parent.width
        height: 40
        color: "transparent"
        anchors.top: cmdHeader.bottom

        // 连接状态：绿点 + 文字，提示当前能否点按指令发送
        Row {
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            Rectangle {
                width: 8
                height: 8
                radius: 4
                anchors.verticalCenter: parent.verticalCenter
                color: bleManager.terminal.active ? app.cGreen : app.cSubtext
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: bleManager.terminal.active
                      ? (bleManager.peripheralConnected ? "外设模式已连接" : "已连接")
                      : "未连接"
                color: bleManager.terminal.active ? app.cGreen : app.cSubtext
                font.pixelSize: 12
                font.bold: bleManager.terminal.active
            }
        }

        // 清空按钮：清除下方日志文本框中的全部内容
        Button {
            id: clearLogBtn
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 68
            implicitHeight: 30
            onClicked: page.clearLog()
            contentItem: Text {
                text: "✕ 清空"
                color: app.cText
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: clearLogBtn.pressed ? Qt.lighter(app.cSurface2, 0.85)
                    : (clearLogBtn.hovered ? Qt.lighter(app.cSurface2, 1.12) : app.cSurface2)
                radius: 15
                border.color: app.cDivider
                border.width: 1
            }
        }
    }

    // -------- 信息/日志文本框（顶栏下方，放大便于观察） --------
    // 显示三类内容（彩色分行）：
    //   * 指令增/删/改/查与数据库状态；
    //   * 连接信息：连接成功绿字「已连接到蓝牙…」+ 设备信息(MAC/UUID/信号强度)；
    //   * 收发数据流：与调试文本框同源（[接收]/[发送] 实时镜像）。
    Rectangle {
        id: logFrame
        anchors.top: statusBar.bottom
        anchors.topMargin: 4
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.right: parent.right
        anchors.rightMargin: 8
        height: 220
        radius: 8
        color: app.cSurface2
        border.color: app.cDivider
        border.width: 1
        clip: true

        // 空态占位：还没有任何操作记录时显示引导文案
        Text {
            anchors.fill: parent
            anchors.margins: 10
            visible: logModel.count === 0
            text: "[BLE-SAR] 信息会显示在这里\n连接蓝牙后：连接信息与收发数据将实时显示；\n点按下方指令即可直接发送"
            color: app.cSubtext
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            lineHeight: 1.6
        }

        ListView {
            id: logList
            anchors.fill: parent
            anchors.margins: 10
            clip: true
            model: logModel
            spacing: 3
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Text {
                width: logList.width - 8
                text: model.line
                color: model.color || app.cText
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }
    }

    // -------- 指令列表 --------
    ListView {
        id: commandList
        anchors.top: logFrame.bottom
        anchors.topMargin: 4
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
            height: 76
            color: mouseArea2.pressed ? app.cSurface2 : app.cSurface

            MouseArea {
                id: mouseArea2
                anchors.fill: parent
                // 单击即发送该条指令的实际内容（command_self）
                onClicked: {
                    commandList.currentIndex = index      // 同时作为当前选中项
                    page.lastFound = index
                    page.sendCommand(index)
                }
            }

            // 序号徽标
            Rectangle {
                width: 28
                height: 28
                radius: 14
                color: commandList.currentIndex === index ? app.cAccent : app.cSurface2
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

            // 名称 / 指令内容
            Column {
                anchors.left: parent.left
                anchors.leftMargin: 56
                anchors.right: parent.right
                anchors.rightMargin: 56
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3

                Text {
                    width: parent.width
                    elide: Text.ElideRight
                    text: model.name
                    color: app.cText
                    font.pixelSize: 15
                    font.bold: true
                }
                Text {
                    width: parent.width
                    elide: Text.ElideRight
                    text: model.command
                    color: app.cAccent
                    font.pixelSize: 13
                    font.family: "Consolas"
                }
            }

            // 右侧提示：单击该项即把指令内容直接发送
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "点按\n发送"
                color: commandList.currentIndex === index ? app.cAccent : app.cSubtext
                font.pixelSize: 9
                font.bold: commandList.currentIndex === index
                horizontalAlignment: Text.AlignHCenter
                lineHeight: 1.35
            }

            Rectangle {
                width: parent.width
                height: 1
                color: app.cDivider
                anchors.bottom: parent.bottom
            }
        }
    }

    // -------- 编辑对话框（新增 / 修改 共用） --------
    // 注意：header 属性是 Dialog 专有（Dialog 继承自 Popup），
    // 若用 Popup 声明会导致 QML 加载失败、程序启动闪退。
    Dialog {
        id: editDialog
        modal: true
        width: 360
        padding: 16
        // 点击对话框外部遮罩或按返回键也可关闭，避免弹框“退不出来”
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        property bool isNew: true
        property string titleText: ""

        onOpened: {
            // 打开瞬间按当前键盘状态定位，保证底部按钮不被软键盘盖住
            editErrText.visible = false       // 每次打开清除上次的就地错误提示
            page.repositionDialog(editDialog)
            editNameField.forceActiveFocus()
            // 首帧布局完成后内容高度可能才最终确定，再校正一次位置
            Qt.callLater(function () {
                if (editDialog.opened) page.repositionDialog(editDialog)
            })
        }
        onClosed: Qt.inputMethod.hide()

        background: Rectangle { color: app.cSurface; radius: 12 }
        header: Rectangle {
            width: parent.width
            height: 48
            color: "transparent"
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: editDialog.titleText
                color: app.cText
                font.pixelSize: 16
                font.bold: true
            }
        }

        contentItem: Column {
            spacing: 12
            // 就地错误提示：校验失败时显示在对话框内部（modal 遮罩会盖住全局 toast）
            Text {
                id: editErrText
                width: parent.width
                visible: false
                color: app.cRed
                font.pixelSize: 12
                font.bold: true
                wrapMode: Text.Wrap
                // 提示行出现/消失会让对话框变高变矮，结束后重新定位
                onVisibleChanged: Qt.callLater(function () {
                    if (editDialog.opened) page.repositionDialog(editDialog)
                })
            }
            TextField {
                id: editNameField
                width: parent.width
                placeholderText: "指令名称 (如: 读取电量)"
                placeholderTextColor: app.cSubtext
                color: app.cText
                font.pixelSize: 14
                // 键盘回车/完成键直接保存（Android 软键盘无实体回车按钮时也保留按钮路径）
                onAccepted: page.saveEdit()
                background: Rectangle {
                    color: app.cSurface2
                    radius: 4
                    border.color: app.cDivider
                }
            }
            TextField {
                id: editCmdField
                width: parent.width
                placeholderText: "指令内容 (如: AT+VBAT?)"
                placeholderTextColor: app.cSubtext
                color: app.cAccent
                font.pixelSize: 14
                onAccepted: page.saveEdit()
                background: Rectangle {
                    color: app.cSurface2
                    radius: 4
                    border.color: app.cDivider
                }
            }
            // 按钮区：确定为主按钮（整行大按钮、按压有明显颜色反馈），取消为次级按钮
            Column {
                width: parent.width
                spacing: 10
                Button {
                    id: editCancelBtn
                    width: parent.width
                    height: 40
                    onClicked: {
                        Qt.inputMethod.hide()
                        editDialog.close()
                    }
                    contentItem: Text {
                        text: "取消 (Cancel)"
                        color: app.cSubtext
                        font.pixelSize: 14
                    }
                    background: Rectangle {
                        color: editCancelBtn.pressed ? Qt.lighter(app.cSurface2, 0.85) : app.cSurface2
                        radius: 20
                        border.color: app.cDivider
                        border.width: 1
                    }
                }
                Button {
                    id: editOkBtn
                    width: parent.width
                    height: 48
                    onClicked: page.saveEdit()
                    contentItem: Text {
                        text: "✓ 确定 (OK)"
                        color: "white"
                        font.pixelSize: 17
                        font.bold: true
                    }
                    // 按下去整块颜色变深，明确告诉用户“已经按到了”
                    background: Rectangle {
                        color: editOkBtn.pressed ? Qt.lighter(app.cAccent, 0.75)
                                                 : (editOkBtn.hovered ? Qt.lighter(app.cAccent, 1.15) : app.cAccent)
                        radius: 24
                    }
                }
            }
        }
    }

    // -------- 查找对话框 --------
    Dialog {
        id: findDialog
        modal: true
        width: 360
        padding: 16
        // 同 editDialog：点外部/返回键可关闭
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        onOpened: {
            findErrText.visible = false       // 每次打开清除上次的就地提示
            page.repositionDialog(findDialog)
            findField.forceActiveFocus()
            // 首帧布局完成后内容高度可能才最终确定，再校正一次位置
            Qt.callLater(function () {
                if (findDialog.opened) page.repositionDialog(findDialog)
            })
        }
        onClosed: Qt.inputMethod.hide()

        background: Rectangle { color: app.cSurface; radius: 12 }
        header: Rectangle {
            width: parent.width
            height: 48
            color: "transparent"
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: "查找指令 (Find)"
                color: app.cText
                font.pixelSize: 16
                font.bold: true
            }
        }

        contentItem: Column {
            spacing: 12
            // 就地提示：空关键字/未找到时显示在对话框内部（modal 遮罩会盖住全局 toast）
            Text {
                id: findErrText
                width: parent.width
                visible: false
                color: app.cRed
                font.pixelSize: 12
                font.bold: true
                wrapMode: Text.Wrap
                // 提示行出现/消失会让对话框变高变矮，结束后重新定位
                onVisibleChanged: Qt.callLater(function () {
                    if (findDialog.opened) page.repositionDialog(findDialog)
                })
            }
            TextField {
                id: findField
                width: parent.width
                placeholderText: "输入名称/内容关键字…"
                placeholderTextColor: app.cSubtext
                color: app.cText
                font.pixelSize: 14
                onAccepted: page.doFind()
                background: Rectangle {
                    color: app.cSurface2
                    radius: 4
                    border.color: app.cDivider
                }
            }
            // 按钮区：查找为主按钮（整行大按钮、按压有明显颜色反馈），取消为次级按钮
            Column {
                width: parent.width
                spacing: 10
                Button {
                    id: findCancelBtn
                    width: parent.width
                    height: 40
                    onClicked: {
                        Qt.inputMethod.hide()
                        findDialog.close()
                    }
                    contentItem: Text {
                        text: "取消 (Cancel)"
                        color: app.cSubtext
                        font.pixelSize: 14
                    }
                    background: Rectangle {
                        color: findCancelBtn.pressed ? Qt.lighter(app.cSurface2, 0.85) : app.cSurface2
                        radius: 20
                        border.color: app.cDivider
                        border.width: 1
                    }
                }
                Button {
                    id: findOkBtn
                    width: parent.width
                    height: 48
                    onClicked: page.doFind()
                    contentItem: Text {
                        text: "🔍 查找 (Find)"
                        color: "white"
                        font.pixelSize: 17
                        font.bold: true
                    }
                    // 按下去整块颜色变深，明确告诉用户“已经按到了”
                    background: Rectangle {
                        color: findOkBtn.pressed ? Qt.lighter(app.cAccent, 0.75)
                                                 : (findOkBtn.hovered ? Qt.lighter(app.cAccent, 1.15) : app.cAccent)
                        radius: 24
                    }
                }
            }
        }
    }

    // 键盘弹出/收起/切换输入法（高度变化）时，把已打开的对话框重新定位到键盘上方，
    // 避免软键盘盖住「确定/查找」按钮导致点不到、误以为没反应。
    Connections {
        target: Qt.inputMethod
        function onVisibleChanged() { page.syncDialogPositions() }
        function onKeyboardRectangleChanged() { page.syncDialogPositions() }
    }
}
