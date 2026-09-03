import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BLE_SAR.Ble

// =====================================================================
// 广播页（底部导航第 2 个 Tab）
// 本机作为可连接的 BLE 外设广播；广播参数读取本页输入框。
// 所有主题色 / 共享状态均通过 app（Main.qml 根 Window）访问。
// =====================================================================
Item {
    id: page
    property var app

    // ---- 广播控制（权限申请统一走 app.ensurePermission）----
    function startAdvertise() {
        app.ensurePermission(BlePermissions.AdvertisePermission, function () {
            // 广播名称 = 本机系统蓝牙名称：Android 平台广播包名称只能由系统
            // 填充（即系统蓝牙名，应用内无法自定义），其它平台则以该名称广播；
            // 名称为空时由 C++ 端回退默认名。超长截断由 C++ 端完成并回调提示。
            bleManager.startAdvertise(bleManager.localDeviceName,
                                      advertUuidField.text.trim(),
                                      parseInt(advertIntervalField.text, 10) || 100)
        })
    }
    function stopAdvertise() {
        bleManager.stopAdvertise()
    }

    // -------- 广播页顶栏 --------
    Rectangle {
        id: advertHeader
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
            text: "广播 (Advertiser)"
            color: app.cText
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
            color: app.cText
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
                    color: app.advertising ? app.cGreen : app.cSubtext
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: app.advertising ? "正在广播 (Advertising)" : "未广播 (Idle)"
                    color: app.advertising ? app.cGreen : app.cSubtext
                    font.pixelSize: 13
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Row {
                spacing: 8
                visible: app.advertising   // 仅在广播中显示连接状态
                Rectangle {
                    width: 10
                    height: 10
                    radius: 5
                    color: app.peripheralConnected ? app.cGreen : app.cSubtext
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: app.peripheralConnected
                          ? "已连接：可收发数据" : "等待其它设备连接…"
                    color: app.peripheralConnected ? app.cGreen : app.cSubtext
                    font.pixelSize: 13
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
        Rectangle {
            width: parent.width
            height: 44
            radius: 22
            color: app.advertising ? app.cRed : app.cAccent
            anchors.horizontalCenter: parent.horizontalCenter
            Text {
                anchors.centerIn: parent
                text: app.advertising ? qsTr("停止广播 (Stop)") : qsTr("开始广播 (Start)")
                color: "white"
                font.pixelSize: 15
                font.bold: true
            }
            MouseArea {
                anchors.fill: parent
                onClicked: app.advertising ? page.stopAdvertise() : page.startAdvertise()
            }
        }
        GridLayout {
            width: parent.width
            columns: 2
            rowSpacing: 10
            columnSpacing: 12

            Text { text: "广播名称"; color: app.cSubtext; font.pixelSize: 14 }
            TextField {
                id: advertNameField
                Layout.fillWidth: true
                height: 36
                // 只读显示本机系统蓝牙名称：Android 广播包中的名称由系统
                // 填充（即系统蓝牙名），应用内无法自定义，展示给用户直观对应
                readOnly: true
                text: bleManager.localDeviceName.length > 0
                      ? bleManager.localDeviceName : "BLE_SAR"
                placeholderText: "BLE_SAR"
                placeholderTextColor: app.cSubtext
                color: app.cSubtext
                font.pixelSize: 13
                background: Rectangle { color: app.cSurface2; radius: 4; border.color: app.cDivider }
            }

            Text { text: "Service UUID"; color: app.cSubtext; font.pixelSize: 14 }
            TextField {
                id: advertUuidField
                Layout.fillWidth: true
                height: 36
                placeholderText: "0000feb1-0000-1000-8000-00805f9b34fb"
                placeholderTextColor: app.cSubtext
                color: app.cAccent
                font.pixelSize: 12
                background: Rectangle { color: app.cSurface2; radius: 4; border.color: app.cDivider }
            }

            Text { text: "广播间隔(ms)"; color: app.cSubtext; font.pixelSize: 14 }
            TextField {
                id: advertIntervalField
                Layout.fillWidth: true
                height: 36
                text: "100"
                placeholderText: "100"
                inputMethodHints: Qt.ImhDigitsOnly
                placeholderTextColor: app.cSubtext
                color: app.cText
                font.pixelSize: 13
                background: Rectangle { color: app.cSurface2; radius: 4; border.color: app.cDivider }
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: qsTr("说明：点击「开始广播」后本机作为真实可连接的 BLE 外设广播（内置 GATT 服务），其它设备可扫描到并连接，连接后可收发数据。广播名称显示的是本机系统蓝牙名称（Android 平台广播名称即系统蓝牙名，应用内不可修改），如需修改请到手机「设置 → 蓝牙 → 设备名称」中更改。Service UUID 可留空。")
            color: app.cSubtext
            font.pixelSize: 12
        }
    }
}
