import QtQuick
import QtQuick.Controls

// =====================================================================
// 关于页（底部导航第 5 个 Tab）
// 所有主题色 / 状态均通过 app（Main.qml 根 Window）访问。
// =====================================================================
Item {
    id: page
    property var app

    // -------- 关于页顶栏 --------
    Rectangle {
        id: aboutHeader
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
            text: "关于 (About)"
            color: app.cText
            font.pixelSize: 16
            font.bold: true
        }
    }

    // -------- 关于内容 --------
    Column {
        anchors.centerIn: parent
        spacing: 8
        Image {
            width: 80
            height: 80
            anchors.horizontalCenter: parent.horizontalCenter
            source: "qrc:/qt/qml/BLE_SAR/icons/tubiao.png"
            sourceSize.width: 160
            sourceSize.height: 160
            fillMode: Image.PreserveAspectFit
            antialiasing: true
        }
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "BLE-SAR"; color: app.cText; font.pixelSize: 22; font.bold: true }
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Version 1.0.0"; color: app.cSubtext; font.pixelSize: 13 }
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "做您最舒心的蓝牙调试助手"; color: app.cSubtext; font.pixelSize: 13 }
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "联系作者:1261034038@qq.com"; color: app.cSubtext; font.pixelSize: 13 }
    }
}
