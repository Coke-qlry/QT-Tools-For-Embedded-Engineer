import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// =====================================================================
// 已绑定设备页（底部导航第 5 个 Tab）
//
// 职责：
//   * 列出 DebugCheckboxStatusControl 仓库中所有已绑定设备
//     （按绑定时间倒序，最近绑定的在最上面，与用户「依次向下排列」一致）
//   * 单击列表项：选中（高亮）但不做其它操作（与指令页的「点按发送」不同）
//   * 右上角「解除绑定」按钮：解除当前选中设备的绑定
//     （与调试页「绑定此设备」取消勾选等效，会同时取消勾选并触发自动重连规则）
//   * 没有文本框（仅展示，不需要编辑）
//
// 数据来源：
//   bleManager.checkboxControl.boundDevices() → [{address, name, boundTime}, ...]
//   仓库变更通过 boundDevicesChanged 信号驱动本页刷新（详见 Connections）。
// =====================================================================

Item {
    id: page
    property var app

    // 列表当前选中项索引；-1 表示未选中
    property int selectedIndex: -1
    // 列表数据模型（每次刷新从仓库重新填充）
    ListModel { id: boundModel }

    // 把仓库里的绑定列表灌到 boundModel；同时复位选中项。
    // 仓库返回 [{address, name, boundTime}]，我们额外把 boundTime 转成
    // 可读的本地时间字符串显示（UI 友好）。
    function reload() {
        boundModel.clear()
        var rows = []
        try {
            rows = bleManager.checkboxControl.boundDevices()
        } catch (e) {
            rows = []
        }
        for (var i = 0; i < rows.length; i++) {
            var t = Number(rows[i].boundTime) || 0
            var dateStr = t > 0
                          ? Qt.formatDateTime(new Date(t), "yyyy-MM-dd HH:mm:ss")
                          : "-"
            boundModel.append({
                address: String(rows[i].address || ""),
                name:    String(rows[i].name    || "N/A"),
                timeStr: dateStr,
                // 存原始时间戳便于按时间排序或后续扩展
                boundTime: t
            })
        }
        // 如果之前选中的项已经不存在，复位为 -1
        if (selectedIndex >= boundModel.count) {
            selectedIndex = -1
        }
    }

    Component.onCompleted: reload()

    // 仓库变化时重新加载列表（绑定/解绑后实时刷新）
    Connections {
        target: bleManager.checkboxControl
        function onBoundDevicesChanged() {
            page.reload()
        }
        function onErrorOccurred(msg) {
            if (app && app.showToast) app.showToast(msg)
        }
    }

    // ================ 顶栏 ================
    Rectangle {
        id: topBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 44
        color: app.cSurface
        border.color: app.cDivider
        border.width: 0

        // 左：返回按钮
        Button {
            id: backBtn
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            width: 36
            height: 32
            text: "←"
            font.pixelSize: 18
            background: Rectangle {
                color: backBtn.pressed ? app.cSurface2 : "transparent"
                radius: 6
            }
            onClicked: app.currentTab = 2   // 调试页（与首次进入调试页后从右上解绑的体验保持一致）
        }

        // 中：标题
        Text {
            anchors.centerIn: parent
            text: "已绑定设备"
            font.pixelSize: 15
            font.bold: true
            color: app.cText
        }

        // 右：解除绑定按钮（仅在有选中项时可点）
        Button {
            id: unbindBtn
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            width: 78
            height: 30
            text: "解除绑定"
            font.pixelSize: 12
            enabled: page.selectedIndex >= 0
            background: Rectangle {
                radius: 6
                color: unbindBtn.enabled
                       ? (unbindBtn.pressed ? Qt.darker(app.cRed, 1.2) : app.cRed)
                       : app.cSurface2
            }
            contentItem: Text {
                text: unbindBtn.text
                font.pixelSize: 12
                color: unbindBtn.enabled ? "white" : app.cSubtext
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: page.unbindSelected()
        }
    }

    // ================ 空态提示 ================
    // 没有绑定记录时显示一个友好的占位
    Item {
        anchors.top: topBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: boundModel.count === 0
        Column {
            anchors.centerIn: parent
            spacing: 8
            Text {
                text: "🔗"
                font.pixelSize: 36
                color: app.cSubtext
                horizontalAlignment: Text.AlignHCenter
                anchors.horizontalCenter: parent.horizontalCenter
            }
            Text {
                text: "暂无已绑定设备"
                color: app.cSubtext
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                anchors.horizontalCenter: parent.horizontalCenter
            }
            Text {
                text: "在「调试」页连接设备后，勾选「绑定此设备」即可加入此列表"
                color: app.cSubtext
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
                anchors.horizontalCenter: parent.horizontalCenter
                width: 240
                wrapMode: Text.WordWrap
            }
        }
    }

    // ================ 列表 ================
    ListView {
        id: bindList
        anchors.top: topBar.bottom
        anchors.topMargin: 4
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        visible: boundModel.count > 0
        model: boundModel
        spacing: 1
        ScrollBar.vertical: ScrollBar {}

        delegate: Rectangle {
            width: bindList.width
            // 比指令页略矮（没有指令内容两行），但保留充足视觉重量
            height: 70
            color: bindMouse.pressed ? app.cSurface2 : app.cSurface

            MouseArea {
                id: bindMouse
                anchors.fill: parent
                // 单击即选中（不发送任何命令、不触发连接）
                onClicked: bindList.currentIndex = index
            }

            // 序号徽标（与指令页风格一致）
            Rectangle {
                width: 28
                height: 28
                radius: 14
                color: bindList.currentIndex === index ? app.cAccent : app.cSurface2
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

            // 主信息：设备名 + MAC + 绑定时间
            Column {
                anchors.left: parent.left
                anchors.leftMargin: 56
                anchors.right: parent.right
                anchors.rightMargin: 16
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
                    elide: Text.ElideMiddle
                    text: model.address
                    color: app.cAccent
                    font.pixelSize: 12
                    font.family: "Consolas"
                }
                Text {
                    width: parent.width
                    elide: Text.ElideRight
                    text: "绑定于 " + model.timeStr
                    color: app.cSubtext
                    font.pixelSize: 10
                }
            }
        }

        // 高亮样式（与指令页保持一致）
        highlightFollowsCurrentItem: true
        highlightMoveDuration: 150
        highlight: Rectangle {
            color: Qt.rgba(0, 0.66, 0.81, 0.22)
            radius: 4
        }

        // 列表选中索引变化 → 同步到 page.selectedIndex
        onCurrentIndexChanged: {
            if (page.selectedIndex !== currentIndex)
                page.selectedIndex = currentIndex
        }
    }

    // ================ 解绑操作 ================
    // 解除当前选中项的绑定：
    //   1) 调用仓库 unbindDevice（数据库立刻更新 → boundDevicesChanged 信号
    //      触发 reload() → 列表自动刷新）
    //   2) 如果当前连接的设备刚好就是被解绑的设备，同时同步取消调试页
    //      「绑定此设备」勾选框（与取消勾选的语义一致）
    function unbindSelected() {
        if (selectedIndex < 0 || selectedIndex >= boundModel.count) return
        var it = boundModel.get(selectedIndex)
        if (!it) return
        var addr = String(it.address || "")
        if (addr === "") return
        var ok = bleManager.checkboxControl.unbindDevice(addr)
        if (ok) {
            if (app && app.showToast) app.showToast("已解除绑定：" + it.name)
            // 同步取消调试页「绑定此设备」CheckBox 勾选状态：
            //   仅当被解绑的就是「当前连接的设备」时才取消勾选；
            //   如果解除的是别的设备（不是当前连接），则 CheckBox 不动——
            //   CheckBox 在语义上标记的是"当前连接设备"的绑定偏好，
            //   与历史/其他绑定记录互不影响，也没连上的解除后下次扫描
            //   不会自动连接。
            var isCurrentConnection = false
            try {
                if (app && bleManager && bleManager.connected === true) {
                    var ta = String(app.targetAddress || "").trim().toLowerCase()
                    isCurrentConnection = (ta !== "" && ta === addr)
                }
            } catch (e) { /* 容错，按未命中处理 */ }
            if (isCurrentConnection) {
                try {
                    if (app.terminalPage
                        && typeof app.terminalPage.bindCurrentChecked !== "undefined") {
                        app.terminalPage.bindCurrentChecked = false
                        app.terminalPage.currentBoundAddress = ""
                    }
                } catch (e) { /* 容错：解除绑定本身已成功 */ }
            }
            // 数据库侧的 bindCurrentDevice 标志无论命中与否都清零，
            // 避免下次启动时把残留的 true 回填到 CheckBox，造成 UI 与用户
            // 当前意图不一致。
            bleManager.checkboxControl.bindCurrentDevice = false
            // 清空选中
            selectedIndex = -1
            bindList.currentIndex = -1
        } else {
            if (app && app.showToast) app.showToast("解除绑定失败")
        }
    }
}
