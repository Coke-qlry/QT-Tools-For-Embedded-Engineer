import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// =====================================================================
// 扫描页（底部导航第 1 个 Tab）
// 设备扫描列表 + 右上角「过滤 / 排序 / 清除」+ 右下角悬浮扫描按钮。
// 过滤对话框与过滤函数为本页专属；设备模型、过滤状态、连接控制、
// RSSI 颜色函数等共享逻辑均通过 app（Main.qml 根 Window）访问。
// =====================================================================
Item {
    id: page
    property var app

    // ---- 过滤：应用 / 重置 / 清除（过滤状态与模型重建由 app 完成）----
    function applyFilter() {
        app.filterMinRssi = filterDialog.rssiSel
        app.filterAddress = filterMacField.text.trim()
        app.filterName = filterNameField.text.trim()
        app.rebuildViewModel()
        filterDialog.close()
        app.showToast(app.filterActive ? "已应用过滤条件" : "已清除过滤条件")
    }
    function resetFilter() {
        app.filterMinRssi = -127
        app.filterAddress = ""
        app.filterName = ""
        filterDialog.rssiSel = -127
        filterMacField.text = ""
        filterNameField.text = ""
        app.rebuildViewModel()
        app.showToast("已重置过滤条件")
    }
    // 清除过滤条件（提示条上的 ✕）
    function clearFilter() {
        app.filterMinRssi = -127
        app.filterAddress = ""
        app.filterName = ""
        app.rebuildViewModel()
        app.showToast("已清除过滤条件")
    }

    // -------- 扫描页顶栏 --------
    Rectangle {
        id: scanHeader
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
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            text: "扫描 (Scan)"
            color: app.cText
            font.pixelSize: 16
            font.bold: true
        }
        // 过滤
        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 124
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            implicitWidth: 58
            implicitHeight: 52
            onClicked: filterDialog.open()
            contentItem: Column {
                spacing: 2
                anchors.centerIn: parent
                Text {
                    text: "⏳"
                    font.pixelSize: 15
                    height: 20
                    verticalAlignment: Text.AlignVCenter
                    color: app.filterActive ? app.cAccent : app.cText
                    horizontalAlignment: Text.AlignHCenter
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                Text {
                    text: "过滤(Filter)"
                    font.pixelSize: 9
                    color: app.filterActive ? app.cAccent : app.cText
                    horizontalAlignment: Text.AlignHCenter
                    anchors.horizontalCenter: parent.horizontalCenter
                }
            }
        }
        // 排序
        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 66
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            implicitWidth: 58
            implicitHeight: 52
            onClicked: app.sortDevices()
            contentItem: Column {
                spacing: 2
                anchors.centerIn: parent
                Text {
                    text: "⇅"
                    font.pixelSize: 15
                    height: 20
                    verticalAlignment: Text.AlignVCenter
                    color: app.cText
                    horizontalAlignment: Text.AlignHCenter
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                Text {
                    text: "排序(Sort)"
                    font.pixelSize: 9
                    color: app.cText
                    horizontalAlignment: Text.AlignHCenter
                    anchors.horizontalCenter: parent.horizontalCenter
                }
            }
        }
        // 清除
        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            implicitWidth: 58
            implicitHeight: 52
            onClicked: app.clearDevices()
            contentItem: Column {
                spacing: 2
                anchors.centerIn: parent
                Text {
                    text: "✕"
                    font.pixelSize: 15
                    height: 20
                    verticalAlignment: Text.AlignVCenter
                    color: app.cText
                    horizontalAlignment: Text.AlignHCenter
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                Text {
                    text: "清除(Clear)"
                    font.pixelSize: 9
                    color: app.cText
                    horizontalAlignment: Text.AlignHCenter
                    anchors.horizontalCenter: parent.horizontalCenter
                }
            }
        }
    }

    // -------- 过滤激活提示条 --------
    Rectangle {
        id: filterBanner
        visible: app.filterActive
        width: parent.width
        height: 36
        color: app.cAccentDark
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
                text: app.filterSummaryText
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
                onClicked: page.clearFilter()
            }
        }
    }

    // -------- 状态条 --------
    Rectangle {
        id: statusBanner
        width: parent.width
        height: 44
        color: app.scanning ? app.cAccentDark : app.cSurface
        visible: app.scanning || app.deviceModel.count === 0
        anchors.top: filterBanner.visible ? filterBanner.bottom : scanHeader.bottom
        Row {
            anchors.centerIn: parent
            spacing: 10
            BusyIndicator {
                width: 18
                height: 18
                running: app.scanning
                visible: app.scanning
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: app.scanning ? qsTr("正在扫描… (Scanning)") : qsTr("点击右下角按钮开始扫描 (Scan)")
                color: app.cText
                font.pixelSize: 13
            }
        }
    }

    // -------- 设备列表（显示模型：过滤 + 排序，由 app.rebuildViewModel 维护） --------
    ListView {
        id: deviceList
        anchors.top: statusBanner.visible ? statusBanner.bottom
                                          : (filterBanner.visible ? filterBanner.bottom : scanHeader.bottom)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        model: app.viewModel
        spacing: 1
        ScrollBar.vertical: ScrollBar {}

        delegate: Rectangle {
            id: deviceDelegate
            width: deviceList.width
            height: 72
            color: mouseArea.pressed ? app.cSurface2 : app.cSurface
            // 派生状态：绑定在 delegate 顶层，供内部嵌套 Repeater 使用
            // （避免嵌套作用域中 model 被遮蔽读不到 rssi/connecting）
            readonly property int itemBars: app.rssiBars(model.rssi)
            readonly property color itemSigColor: app.rssiColor(model.rssi)
            readonly property bool itemConnecting: model.connecting === true

            Rectangle {
                width: 10
                height: 10
                radius: 5
                color: model.connected ? app.cGreen : app.cSubtext
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
                    color: app.cText
                    font.pixelSize: 15
                    font.bold: true
                }
                Text {
                    width: parent.width
                    elide: Text.ElideRight
                    text: model.address
                    color: app.cSubtext
                    font.pixelSize: 12
                }
            }

            Row {
                id: rightCol
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                // RSSI 信号图标（强=绿满格，中=黄，弱=红）+ 数值
                Row {
                    spacing: 6
                    Rectangle {
                        width: 26
                        height: 30
                        anchors.verticalCenter: parent.verticalCenter
                        color: "transparent"
                        Repeater {
                            model: 4
                            Rectangle {
                                width: 3.5
                                height: 7 + (index + 1) * 6
                                radius: 1.5
                                anchors.bottom: parent.bottom
                                x: index * 7
                                color: index < deviceDelegate.itemBars
                                       ? deviceDelegate.itemSigColor
                                       : app.cDivider
                            }
                        }
                    }
                    Text {
                        text: model.rssi
                        color: deviceDelegate.itemSigColor
                        font.pixelSize: 12
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                // 连接按钮三态：未连接=青色「连接(Conn)」，
                // 连接中=黄色「连接中…」，已连接=灰色「已连接」
                Rectangle {
                    width: 78
                    height: 30
                    radius: 15
                    color: deviceDelegate.itemConnecting ? app.cOrange
                         : model.connected ? app.cDivider : app.cAccent
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        anchors.centerIn: parent
                        text: deviceDelegate.itemConnecting ? "连接中…"
                            : model.connected ? "已连接" : "连接(Conn)"
                        color: "white"
                        font.pixelSize: 11
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: app.connectToDeviceByAddress(model.address)
                    }
                }
            }

            // 整行点击也可连接
            MouseArea {
                id: mouseArea
                anchors.fill: parent
                onClicked: app.connectToDeviceByAddress(model.address)
            }

            Rectangle {
                width: parent.width
                height: 1
                color: app.cDivider
                anchors.bottom: parent.bottom
            }
        }
    }

    // -------- 悬浮扫描按钮 (FAB) --------
    Rectangle {
        width: 60
        height: 60
        radius: 30
        color: app.scanning ? app.cRed : app.cAccent
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
                text: app.scanning ? "■" : "▶"
                color: "white"
                font.pixelSize: 16
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: app.scanning ? "停止(Stop)" : "扫描(Scan)"
                color: "white"
                font.pixelSize: 10
            }
        }
        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (app.scanning) app.stopScan()
                else app.startScan()
            }
        }
    }

    // -------- 过滤对话框（RSSI / MAC / 名称） --------
    // 注意：header 属性是 Dialog 专有（Dialog 继承自 Popup），
    // 若用 Popup 声明会导致 QML 加载失败、程序启动闪退。
    Dialog {
        id: filterDialog
        anchors.centerIn: parent
        modal: true
        width: Math.min(app.width - 32, 400)
        padding: 16

        // 信号强度阈值选择（对话框内临时值，应用时写入 app）
        property int rssiSel: -127
        onOpened: {
            filterDialog.rssiSel = app.filterMinRssi
            filterMacField.text = app.filterAddress
            filterNameField.text = app.filterName
        }

        background: Rectangle { color: app.cSurface; radius: 12 }
        header: Rectangle {
            width: parent.width
            height: 48
            color: "transparent"
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: "设备过滤 (Filter)"
                color: app.cText
                font.pixelSize: 16
                font.bold: true
            }
        }

        contentItem: Column {
            spacing: 14
            // 信号强度阈值
            Text { text: "信号强度 (RSSI)"; color: app.cSubtext; font.pixelSize: 13 }
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
                        color: filterDialog.rssiSel === modelData.v ? app.cAccent : app.cSurface2
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
            Text { text: "MAC 地址包含"; color: app.cSubtext; font.pixelSize: 13 }
            TextField {
                id: filterMacField
                width: parent.width
                height: 36
                placeholderText: "如: A4:C1:38 (留空不过滤)"
                placeholderTextColor: app.cSubtext
                color: app.cText
                font.pixelSize: 13
                background: Rectangle { color: app.cSurface2; radius: 4; border.color: app.cDivider }
            }
            // 名称过滤
            Text { text: "蓝牙名称包含"; color: app.cSubtext; font.pixelSize: 13 }
            TextField {
                id: filterNameField
                width: parent.width
                height: 36
                placeholderText: "如: BLE (留空不过滤)"
                placeholderTextColor: app.cSubtext
                color: app.cText
                font.pixelSize: 13
                background: Rectangle { color: app.cSurface2; radius: 4; border.color: app.cDivider }
            }
            // 操作按钮
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 16
                Button {
                    text: "重置 (Reset)"
                    onClicked: page.resetFilter()
                    contentItem: Text { text: parent.text; color: app.cSubtext; font.pixelSize: 14 }
                    background: Rectangle { color: app.cSurface2; radius: 16; width: 130; height: 32 }
                }
                Button {
                    text: "应用 (Apply)"
                    onClicked: page.applyFilter()
                    contentItem: Text { text: parent.text; color: "white"; font.pixelSize: 14; font.bold: true }
                    background: Rectangle { color: app.cAccent; radius: 16; width: 130; height: 32 }
                }
            }
        }
    }
}
