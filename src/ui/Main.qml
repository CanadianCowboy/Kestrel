import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1420
    height: 900
    minimumWidth: 980
    minimumHeight: 640
    visible: true
    title: "Kestrel"
    color: palette.canvas

    readonly property color canvas: "#0c0d10"
    readonly property color panel: "#15171c"
    readonly property color panelRaised: "#1b1e25"
    readonly property color line: "#282c35"
    readonly property color ink: "#f1f3f5"
    readonly property color muted: "#858c9a"
    readonly property color accent: "#b7e3cf"
    readonly property color accentInk: "#132019"

    QtObject {
        id: palette
        readonly property color canvas: window.canvas
    }

    Shortcut { sequence: "Ctrl+N"; onActivated: appController.newConversation() }
    Shortcut { sequence: "Ctrl+K"; onActivated: composer.forceActiveFocus() }
    Shortcut { sequence: "Escape"; onActivated: appController.stopGeneration() }

    Rectangle {
        anchors.fill: parent
        color: window.canvas

        Rectangle {
            anchors.fill: parent
            opacity: 0.18
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#253b39" }
                GradientStop { position: 0.35; color: window.canvas }
                GradientStop { position: 1.0; color: "#121625" }
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 14

            Rectangle {
                id: sidebar
                Layout.preferredWidth: appController.sidebarOpen ? 268 : 0
                Layout.fillHeight: true
                radius: 18
                color: "#b3171b20"
                border.color: window.line
                clip: true
                visible: width > 0
                Behavior on Layout.preferredWidth { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 18
                    width: sidebar.width - 32

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Text {
                            text: "✦"
                            color: window.accent
                            font.pixelSize: 24
                        }
                        Text {
                            text: "KESTREL"
                            color: window.ink
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            font.letterSpacing: 3
                        }
                        Item { Layout.fillWidth: true }
                        Text { text: "0.1"; color: window.muted; font.pixelSize: 11 }
                    }

                    Button {
                        text: "+   New conversation"
                        Layout.fillWidth: true
                        implicitHeight: 46
                        onClicked: appController.newConversation()
                        contentItem: Text {
                            text: parent.text
                            color: window.accentInk
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: Text.AlignHCenter
                        }
                        background: Rectangle { radius: 12; color: window.accent }
                    }

                    Text {
                        text: "RECENT"
                        color: window.muted
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                        font.letterSpacing: 2
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 54
                        radius: 11
                        color: "#253036"
                        border.color: "#35433f"
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 13
                            anchors.rightMargin: 13
                            spacing: 10
                            Rectangle { width: 7; height: 7; radius: 4; color: window.accent }
                            Text { text: appController.conversationTitle; color: window.ink; font.pixelSize: 13; elide: Text.ElideRight; Layout.fillWidth: true }
                        }
                    }

                    Item { Layout.fillHeight: true }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 78
                        radius: 12
                        color: "#15181d"
                        border.color: window.line
                        Column {
                            anchors.fill: parent
                            anchors.margins: 13
                            spacing: 7
                            Text { text: "LOCAL RUNTIME"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.4 }
                            Row {
                                spacing: 8
                                Rectangle { width: 7; height: 7; radius: 4; color: window.accent; anchors.verticalCenter: parent.verticalCenter }
                                Text { text: appController.modelName; color: window.ink; font.pixelSize: 12 }
                            }
                            Text { text: appController.backendName; color: window.muted; font.pixelSize: 11 }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 54
                    spacing: 13
                    Button {
                        text: appController.sidebarOpen ? "‹" : "☰"
                        implicitWidth: 36
                        implicitHeight: 36
                        onClicked: appController.sidebarOpen = !appController.sidebarOpen
                        contentItem: Text { text: parent.text; color: window.muted; font.pixelSize: 22; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { color: "transparent" }
                    }
                    Column {
                        spacing: 3
                        Text { text: appController.conversationTitle; color: window.ink; font.pixelSize: 15; font.weight: Font.DemiBold }
                        Text { text: "Private workspace  ·  local only"; color: window.muted; font.pixelSize: 11 }
                    }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        implicitWidth: 126
                        implicitHeight: 34
                        radius: 17
                        color: "#17221f"
                        border.color: "#2d463d"
                        Row {
                            anchors.centerIn: parent
                            spacing: 8
                            Rectangle { width: 7; height: 7; radius: 4; color: window.accent; anchors.verticalCenter: parent.verticalCenter }
                            Text { text: "GPU READY"; color: window.accent; font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 1 }
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: window.line; opacity: 0.7 }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 62
                        anchors.rightMargin: 62
                        anchors.topMargin: 30
                        anchors.bottomMargin: 20
                        spacing: 20

                        Item { Layout.fillHeight: true; Layout.minimumHeight: 24 }

                        Column {
                            visible: appController.messages.length === 0
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 16
                            Text { text: "✦"; color: window.accent; font.pixelSize: 32; anchors.horizontalCenter: parent.horizontalCenter }
                            Text { text: "A quieter way to think."; color: window.ink; font.pixelSize: 28; font.weight: Font.Light; anchors.horizontalCenter: parent.horizontalCenter }
                            Text { text: "Your private AI workspace, powered by your GPU."; color: window.muted; font.pixelSize: 13; anchors.horizontalCenter: parent.horizontalCenter }
                            Row {
                                anchors.horizontalCenter: parent.horizontalCenter
                                spacing: 8
                                Repeater {
                                    model: ["Ask anything", "Summarize a file", "Write some code"]
                                    delegate: Rectangle {
                                        width: suggestionText.implicitWidth + 26; height: 34; radius: 17
                                        color: "#14171c"; border.color: window.line
                                        Text { id: suggestionText; anchors.centerIn: parent; text: modelData; color: window.muted; font.pixelSize: 11 }
                                    }
                                }
                            }
                        }

                        ListView {
                            id: messageList
                            visible: appController.messages.length > 0
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            model: appController.messages
                            spacing: 20
                            clip: true
                            delegate: Item {
                                width: messageList.width
                                height: bubble.implicitHeight + 4
                                Rectangle {
                                    id: bubble
                                    width: Math.min(parent.width * 0.78, messageText.implicitWidth + 30)
                                    height: messageText.implicitHeight + 24
                                    radius: 15
                                    anchors.left: modelData.role === "assistant" ? parent.left : undefined
                                    anchors.right: modelData.role === "user" ? parent.right : undefined
                                    color: modelData.role === "assistant" ? "#15181d" : "#243b34"
                                    border.color: modelData.role === "assistant" ? window.line : "#385b4e"
                                    Text {
                                        id: messageText
                                        anchors.fill: parent
                                        anchors.margins: 15
                                        text: modelData.content
                                        color: window.ink
                                        font.pixelSize: 14
                                        lineHeight: 1.35
                                        wrapMode: Text.Wrap
                                    }
                                }
                            }
                            onCountChanged: Qt.callLater(function() { positionViewAtEnd() })
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 68
                            radius: 18
                            color: "#191c22"
                            border.color: composer.activeFocus ? "#55776a" : window.line
                            Behavior on border.color { ColorAnimation { duration: 160 } }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 18
                                anchors.rightMargin: 10
                                spacing: 10
                                TextArea {
                                    id: composer
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    placeholderText: "Message Kestrel..."
                                    placeholderTextColor: "#626a77"
                                    color: window.ink
                                    font.pixelSize: 14
                                    wrapMode: TextArea.Wrap
                                    background: Item {}
                                    verticalAlignment: TextEdit.AlignVCenter
                                    Keys.onReturnPressed: function(event) {
                                        if (!(event.modifiers & Qt.ShiftModifier)) {
                                            appController.sendMessage(text)
                                            text = ""
                                            event.accepted = true
                                        }
                                    }
                                }
                                Button {
                                    implicitWidth: 44
                                    implicitHeight: 44
                                    text: appController.generating ? "■" : "↑"
                                    onClicked: {
                                        if (appController.generating) appController.stopGeneration()
                                        else { appController.sendMessage(composer.text); composer.text = "" }
                                    }
                                    contentItem: Text { text: parent.text; color: window.accentInk; font.pixelSize: 20; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                    background: Rectangle { radius: 13; color: window.accent }
                                }
                            }
                        }

                        Text { text: "Kestrel can make mistakes. Nothing leaves this device."; color: "#555d69"; font.pixelSize: 10; Layout.alignment: Qt.AlignHCenter }
                    }
                }
            }
        }
    }
}
