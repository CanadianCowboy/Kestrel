import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

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

    // A switch for the presence panel. Declared here rather than repeated three
    // times in the diagnostics column, because the file already styles every
    // control by hand and a stock CheckBox would not match it.
    component IdleToggle: RowLayout {
        id: toggle
        required property string label
        property string hint: ""
        property bool checked: false
        signal toggled()
        Layout.fillWidth: true
        activeFocusOnTab: true
        Keys.onSpacePressed: function(event) {
            toggle.toggled()
            event.accepted = true
        }
        // One MouseArea over the whole row, so the label is the target and the
        // hover that reveals the hint is the same gesture as the click.
        //
        // It lives in a plain Item rather than being a RowLayout child. A
        // layout gives every child its own cell, so a MouseArea declared here
        // is a cell of its own *after* the indicator and the label -- it does
        // not cover them, and because it and the label both ask for
        // Layout.fillWidth they merely split the leftover width between them.
        // Clicks on the indicator, and on the left part of the label, did
        // nothing. The comment here used to say the opposite.
        //
        // Wrapping the row in an Item lets the MouseArea anchor to the row
        // instead, which is legal precisely because it is no longer a child of
        // the layout. One cell, one item, one gesture over all of it.
        //
        // The indicator is inside that Item too, which is the part this got
        // wrong once already. Left as a sibling of the Item it is a cell of its
        // own further left, so clicking the switch did nothing -- the same
        // defect one cell along. The inner Row exists only to keep the
        // indicator and the label side by side now that they share a cell.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Row {
                id: row
                anchors.fill: parent
                spacing: 10
                Rectangle {
                    width: 14
                    height: 14
                    radius: 4
                    color: toggle.checked ? window.accent : "#1a1d23"
                    // Focus is drawn on the indicator because it is the one
                    // fixed-size part of the row, so a keyboard user can see
                    // where they are without the label having to change.
                    border.color: toggle.activeFocus ? window.ink
                                                    : toggle.checked ? window.accent : window.line
                    Behavior on color { ColorAnimation { duration: 140 } }
                }
                Text {
                    // 14 for the indicator, 10 for the gap, and whatever is
                    // left, so the label elides rather than pushing the row
                    // wider than the panel it is in.
                    width: Math.max(0, row.width - 24)
                    anchors.verticalCenter: parent.verticalCenter
                    text: toggle.label
                    color: window.ink
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
            }
            MouseArea {
                id: hover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    toggle.forceActiveFocus(Qt.MouseFocusReason)
                    toggle.toggled()
                }
            }
            ToolTip.visible: hover.containsMouse && toggle.hint.length > 0
            ToolTip.text: toggle.hint
            ToolTip.delay: 400
        }
    }

    Shortcut { sequence: "Ctrl+N"; onActivated: appController.newConversation() }
    Shortcut { sequence: "Ctrl+K"; onActivated: composer.forceActiveFocus() }
    Shortcut { sequence: "Ctrl+D"; onActivated: appController.diagnosticsOpen = !appController.diagnosticsOpen }
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
                        implicitHeight: 96
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
                            // Loading a model from disk, so the app is not stuck
                            // previewing canned text. Hidden entirely in a build
                            // that cannot load a real model, rather than offered
                            // and then refused.
                            Row {
                                spacing: 8
                                visible: appController.canLoadModel
                                Button {
                                    id: loadModelButton
                                    text: appController.modelPath.length > 0 ? "Change model" : "Load model"
                                    onClicked: modelDialog.open()
                                }
                                Button {
                                    text: "Preview"
                                    visible: appController.modelPath.length > 0
                                    onClicked: appController.usePreviewBackend()
                                }
                            }
                            Text {
                                visible: appController.modelError.length > 0
                                text: appController.modelError
                                color: "#c08a95"
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                                // A plain Column gives no layout width, and
                                // Text.Wrap cannot wrap without one.
                                width: parent.width
                            }
                            Text {
                                text: appController.gpuAvailable ? appController.gpuSummary : "No GPU detected"
                                color: appController.gpuAvailable ? window.muted : "#b5909c"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                                width: parent.width
                            }
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
                    // The presence indicator. A soft pulse and a mood, with no
                    // controls attached: it reports that Kestrel is there
                    // without becoming a dashboard.
                    Rectangle {
                        id: presencePill
                        implicitWidth: presenceRow.implicitWidth + 28
                        implicitHeight: 34
                        radius: 17
                        color: "#161e1c"
                        border.color: "#2b3a35"
                        opacity: 0.62 + 0.38 * appController.presenceIntensity
                        Behavior on opacity { NumberAnimation { duration: 240 } }
                        Row {
                            id: presenceRow
                            anchors.centerIn: parent
                            spacing: 9
                            Rectangle {
                                width: 8; height: 8; radius: 4
                                anchors.verticalCenter: parent.verticalCenter
                                color: window.accent
                                SequentialAnimation on scale {
                                    loops: Animation.Infinite
                                    running: appController.presenceSpeaking
                                    NumberAnimation { from: 1.0; to: 1.8; duration: 850; easing.type: Easing.InOutSine }
                                    NumberAnimation { from: 1.8; to: 1.0; duration: 850; easing.type: Easing.InOutSine }
                                }
                            }
                            Text {
                                text: appController.presenceState
                                color: window.accent
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                                font.letterSpacing: 1.2
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        ToolTip.visible: presenceHover.containsMouse
                        ToolTip.text: "Kestrel is " + appController.presenceState
                                      + " · " + appController.personaMood
                        ToolTip.delay: 400
                        MouseArea {
                            id: presenceHover
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                        }
                    }
                    Rectangle {
                        id: gpuBadge
                        implicitWidth: 126
                        implicitHeight: 34
                        radius: 17
                        color: appController.diagnosticsOpen ? "#1e2a26" : "#17221f"
                        border.color: appController.gpuAvailable ? "#2d463d" : "#3a3038"
                        Behavior on color { ColorAnimation { duration: 160 } }
                        Row {
                            anchors.centerIn: parent
                            spacing: 8
                            Rectangle {
                                width: 7; height: 7; radius: 4
                                anchors.verticalCenter: parent.verticalCenter
                                color: appController.gpuAvailable ? window.accent : "#8a6a74"
                            }
                            Text {
                                text: appController.gpuAvailable ? "GPU READY" : "NO GPU"
                                color: appController.gpuAvailable ? window.accent : "#b5909c"
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                                font.letterSpacing: 1
                            }
                        }
                        MouseArea {
                            id: gpuBadgeHover
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            hoverEnabled: true
                            onClicked: appController.diagnosticsOpen = !appController.diagnosticsOpen
                        }
                        ToolTip.visible: gpuBadgeHover.containsMouse
                        ToolTip.text: appController.gpuAvailable
                                       ? appController.gpuName + " — " + appController.gpuDetail
                                       : appController.gpuDetail
                        ToolTip.delay: 400
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

                        // Pushes the rest of the column down while the
                        // conversation is empty. It must not also fill once
                        // there are messages: a ColumnLayout shares the
                        // leftover height between everything asking to fill, so
                        // an unconditional fill here would take half the chat
                        // area away from the ListView below it. The ListView is
                        // invisible (and so excluded from the layout) exactly
                        // when count is zero, so binding the fill to the same
                        // condition hands the space to whoever needs it.
                        Item {
                            Layout.fillHeight: messageList.count === 0
                            Layout.minimumHeight: 24
                        }

                        Column {
                            visible: messageList.count === 0
                            Layout.alignment: Qt.AlignHCenter
                            Layout.fillHeight: true
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
                            visible: count > 0
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            // The transcript is the point of the window, so it is
                            // the last thing allowed to be squeezed. Without a
                            // floor, a short window leaves it a sliver tall while
                            // the composer keeps its full height -- which is the
                            // arrangement that made a conversation invisible.
                            Layout.minimumHeight: 160
                            model: appController.messages
                            spacing: 16
                            clip: true
                            delegate: MessageBubble {
                                // ListView.view, not the bare id. A delegate
                                // declared as its own component is not evaluated
                                // in this file's context, so messageList.width
                                // does not reliably reach it -- the delegates
                                // came out zero wide, and a zero-wide row has a
                                // zero-wide bubble however correctly the
                                // component is sized.
                                width: ListView.view.width
                            }
                            onCountChanged: Qt.callLater(function() { positionViewAtEnd() })
                        }

                        // One line, saying what Kestrel is doing. Deliberately
                        // not a panel and not a banner: this is the only status
                        // surface in the window, so it can stay quiet.
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.bottomMargin: 2
                            spacing: 9
                            Rectangle {
                                width: 6; height: 6; radius: 3
                                color: window.accent
                                // Tied to the presence engine rather than to any
                                // one event, so the dot breathes with Kestrel's
                                // state instead of blinking on token arrival.
                                opacity: 0.3 + 0.55 * appController.presenceIntensity
                                SequentialAnimation on scale {
                                    loops: Animation.Infinite
                                    running: appController.presenceSpeaking
                                    NumberAnimation { from: 1.0; to: 1.7; duration: 900; easing.type: Easing.InOutSine }
                                    NumberAnimation { from: 1.7; to: 1.0; duration: 900; easing.type: Easing.InOutSine }
                                }
                            }
                            Text {
                                text: appController.statusWhisper
                                color: window.muted
                                font.pixelSize: 11
                            }
                            // The idle loop's own thought. Hidden unless asked
                            // for: an internal note that shows by default is
                            // not an internal note.
                            Text {
                                visible: appController.showIdleThoughts && appController.ambientThought.length > 0
                                text: "· " + appController.ambientThought
                                color: "#4d5a68"
                                font.pixelSize: 11
                                font.italic: true
                            }
                        }                            Item {
                                id: composerDock
                                Layout.fillWidth: true
                                // The dictation line sits above the shell and needs
                                // its own room: its implicit height plus the shell
                                // plus the 6 pixel gap between them. 84 is the
                                // shell's height, and it is fixed rather than a
                                // fill -- a shell that grows with the dock
                                // swallows the line that the dock grew for.
                                Layout.preferredHeight: dictationLine.visible
                                                      ? dictationLine.implicitHeight + 90 : 84


                            // The breathing glow. Slow, wide, and close to
                            // invisible, so the field reads as alive rather than
                            // as decorated.
                            Rectangle {
                                anchors.fill: composerShell
                                anchors.margins: -7
                                radius: 25
                                color: "transparent"
                                border.width: 1
                                border.color: window.accent
                                opacity: 0.12
                                NumberAnimation on opacity {
                                    from: 0.08; to: 0.34
                                    duration: 3600
                                    loops: Animation.Infinite
                                    running: composerDock.visible
                                    easing.type: Easing.InOutSine
                                }
                            }

                            // What dictation is doing, said where the user is
                            // already looking. SAPI reports a finished phrase
                            // rather than the words so far, so most of the time
                            // this is the reason a microphone was pressed and
                            // nothing else -- the words appear in the
                            // transcript when the phrase lands.
                            //
                            // Shown only when there is something to say, so an
                            // idle app is not carrying a status line for a
                            // microphone nobody pressed.
                            Text {
                                id: dictationLine
                                anchors.bottom: composerDock.top
                                anchors.bottomMargin: 6
                                anchors.left: composerDock.left
                                anchors.right: composerDock.right
                                anchors.leftMargin: 18
                                visible: appController.listenError.length > 0
                                         || appController.listening
                                         || appController.partialTranscript.length > 0
                                color: appController.listenError.length > 0 ? "#c98a8a" : window.muted
                                font.pixelSize: 11
                                elide: Text.ElideRight
                                text: appController.listenError.length > 0
                                      ? appController.listenError
                                      : (appController.partialTranscript.length > 0
                                         ? appController.partialTranscript
                                         : "Listening...")
                            }

                            // The audio-reactive ring. Only alive while Kestrel
                            // is speaking, and bright in proportion to the
                            // presence intensity, so it tracks meaning rather
                            // than raw audio amplitude.
                            Rectangle {
                                anchors.fill: composerShell
                                anchors.margins: -7
                                radius: 25
                                color: "transparent"
                                border.width: 1
                                border.color: window.accent
                                opacity: appController.presenceSpeaking
                                         ? 0.14 + 0.42 * appController.presenceIntensity : 0
                                visible: opacity > 0.01
                                SequentialAnimation on scale {
                                    running: appController.presenceSpeaking
                                    loops: Animation.Infinite
                                    NumberAnimation { from: 1.0; to: 1.014; duration: 460; easing.type: Easing.InOutSine }
                                    NumberAnimation { from: 1.014; to: 1.0; duration: 460; easing.type: Easing.InOutSine }
                                }
                            }

                                Rectangle {
                                id: composerShell
                                // Left, right and bottom rather than fill: the
                                // dock is taller than the shell whenever the
                                // dictation line is showing, and filling would
                                // give the extra pixels to the shell instead of
                                // to the line.
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 6
                                height: 84
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
                                        // Anything unsent is a person being present,
                                        // so the idle loop goes quiet before it can
                                        // decide anything.
                                        onTextChanged: appController.inputPending = length > 0
                                        onActiveFocusChanged: appController.inputPending = activeFocus || length > 0
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
                                    // Dictation. Always offered rather than shown
                                    // only on a machine with a microphone, because
                                    // on a machine without one the button still
                                    // answers: sttDetail says why, which is more
                                    // use than a control that is mysteriously
                                    // absent.
                                    Button {
                                        id: micButton
                                        implicitWidth: 44
                                        implicitHeight: 44
                                        text: appController.listening ? "■" : "🎤"
                                        onClicked: {
                                            if (appController.listening)
                                                appController.stopListening()
                                            else
                                                appController.startListening()
                                        }
                                        contentItem: Text {
                                            text: micButton.text
                                            color: appController.listening ? window.accent : window.ink
                                            font.pixelSize: 15
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                        background: Rectangle {
                                            radius: 13
                                            color: appController.listening ? "#2a2f28" : "#232830"
                                            border.color: appController.listening ? window.accent : window.line
                                        }
                                    }
                                    Button {
                                        id: pauseResumeButton
                                        // Only offered when the voice state machine
                                        // says the transition is actually legal, so
                                        // the button can never be a no-op that
                                        // silently does nothing.
                                        visible: appController.canPause || appController.canResume
                                        implicitWidth: 44
                                        implicitHeight: 44
                                        text: appController.canResume ? "▶" : "❚❚"
                                        onClicked: {
                                            if (appController.canResume) appController.resumeConversation()
                                            else appController.pauseConversation()
                                        }
                                        contentItem: Text {
                                            text: pauseResumeButton.text
                                            color: window.ink
                                            font.pixelSize: 13
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                        background: Rectangle {
                                            radius: 13
                                            color: "#232830"
                                            border.color: window.line
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
                            }

                        Text { text: "Kestrel can make mistakes. Nothing leaves this device."; color: "#555d69"; font.pixelSize: 10; Layout.alignment: Qt.AlignHCenter }
                    }
                }
            }
        }

        // Runtime diagnostics live in a secondary panel, never in the
        // conversation flow, so the main workspace stays quiet as the runtime
        // layer grows.
        Rectangle {
            id: diagnosticsPanel
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.margins: 18
            width: 360
            radius: 18
            color: "#15171c"
            border.color: window.line
            visible: appController.diagnosticsOpen
            opacity: appController.diagnosticsOpen ? 1 : 0
            clip: true

            Behavior on opacity {
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    Column {
                        spacing: 3
                        Text { text: "Runtime"; color: window.ink; font.pixelSize: 14; font.weight: Font.DemiBold }
                        Text { text: "Ctrl+D to close  ·  click Refresh to re-probe"; color: window.muted; font.pixelSize: 10 }
                    }
                    Item { Layout.fillWidth: true }
                    Button {
                        text: "Refresh"
                        implicitHeight: 30
                        onClicked: appController.refreshRuntime()
                        contentItem: Text { text: parent.text; color: window.ink; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 9; color: "#1e222a"; border.color: window.line }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: window.line; opacity: 0.7 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Text { text: "ACTIVE MODEL"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.4 }
                    Text { text: appController.modelName; color: window.ink; font.pixelSize: 13; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Text { text: appController.runtimeDetail; color: window.muted; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: gpuColumn.implicitHeight + 24
                    radius: 12
                    color: "#1a1d23"
                    border.color: window.line
                    ColumnLayout {
                        id: gpuColumn
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 6
                        Text { text: "GPU"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.4 }
                        Text { text: appController.gpuName; color: window.ink; font.pixelSize: 13; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        Text {
                            text: appController.gpuAvailable
                                  ? appController.gpuSummary + "  ·  " + appController.gpuDeviceCount + " device(s)"
                                  : appController.gpuDetail
                            color: window.muted; font.pixelSize: 11
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                }

                Text { text: "ASSISTANT"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.4 }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: presenceColumn.implicitHeight + 24
                    radius: 12
                    color: "#1a1d23"
                    border.color: window.line
                    ColumnLayout {
                        id: presenceColumn
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 7
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text { text: "PRESENCE"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.2; Layout.fillWidth: true }
                            Text { text: appController.presenceState; color: window.ink; font.pixelSize: 12 }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text { text: "MOOD"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.2; Layout.fillWidth: true }
                            Text { text: appController.personaMood; color: window.ink; font.pixelSize: 12 }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text { text: "TOPIC"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.2; Layout.fillWidth: true }
                            Text {
                                text: appController.sessionTopic.length > 0 ? appController.sessionTopic : "—"
                                color: window.muted; font.pixelSize: 11
                                horizontalAlignment: Text.AlignRight
                                elide: Text.ElideLeft
                                Layout.maximumWidth: 170
                            }
                        }
                        // Which voice is speaking, or why nothing is. Said
                        // plainly rather than as a silent failure: a reply that
                        // was never spoken is otherwise indistinguishable from
                        // one that was too fast to notice.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text { text: "VOICE"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.2; Layout.fillWidth: true }
                            Text {
                                text: appController.ttsAvailable ? appController.ttsVoice
                                                                : (appController.ttsError.length > 0 ? appController.ttsError : "text only")
                                color: appController.ttsAvailable ? window.ink : window.muted
                                font.pixelSize: 11
                                horizontalAlignment: Text.AlignRight
                                elide: Text.ElideRight
                                Layout.maximumWidth: 170
                            }
                        }

                        // Voice choice, when the backend has a choice to offer.
                        // Hidden otherwise rather than shown empty: a picker with
                        // nothing in it is a control that cannot be used, and on
                        // a machine using the platform voice there is genuinely
                        // nothing to choose between.
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: appController.speechVoices.length > 0
                            spacing: 4

                            Text {
                                text: "VOICE PROFILE"
                                color: window.muted
                                font.pixelSize: 10
                                font.letterSpacing: 1.2
                            }

                            Flow {
                                Layout.fillWidth: true
                                spacing: 6

                                Repeater {
                                    model: appController.speechVoices
                                    delegate: Rectangle {
                                        required property string modelData
                                        readonly property bool selected:
                                            modelData === appController.currentVoice
                                        width: profileText.implicitWidth + 16
                                        height: 22
                                        radius: 11
                                        color: selected ? window.accent : "#1a1d23"
                                        border.color: selected ? window.accent : window.line
                                        Behavior on color { ColorAnimation { duration: 140 } }

                                        Text {
                                            id: profileText
                                            anchors.centerIn: parent
                                            text: parent.modelData
                                            color: parent.selected ? "#0d1015" : window.muted
                                            font.pixelSize: 11
                                        }

                                        MouseArea {
                                            id: profileHit
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: appController.setSpeechVoice(parent.modelData)
                                        }
                                    }
                                }
                            }
                        }

                        // The three switches that keep the idle loop inside its
                        // box: whether it runs at all, whether it may touch the
                        // GPU, and whether its private thoughts are shown.
                        IdleToggle {
                            label: "Idle loop"
                            hint: "quiet internal work between turns"
                            checked: appController.idleLoopEnabled
                            onToggled: appController.idleLoopEnabled = !appController.idleLoopEnabled
                        }
                        IdleToggle {
                            label: "GPU prewarm"
                            hint: "opt-in: a discarded generation while idle"
                            checked: appController.idlePrewarmEnabled
                            onToggled: appController.idlePrewarmEnabled = !appController.idlePrewarmEnabled
                        }
                        IdleToggle {
                            label: "Show thoughts"
                            hint: "reveal what the idle loop is thinking"
                            checked: appController.showIdleThoughts
                            onToggled: appController.showIdleThoughts = !appController.showIdleThoughts
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: appController.idleTaskLabel.length > 0
                            text: appController.idleTaskLabel
                            color: "#4d5a68"
                            font.pixelSize: 11
                            font.italic: true
                            wrapMode: Text.Wrap
                        }

                        // Idle tools. A switch is not enough on its own: a tool
                        // that is on but has not been granted what it declared
                        // is still off, and the user is the one who decides
                        // which is which. So each tool gets its own switch and
                        // its own list of capabilities, each of which can be
                        // granted here.
                        Repeater {
                            model: appController.idleTools
                            delegate: ColumnLayout {
                                required property var modelData
                                readonly property var tool: modelData
                                Layout.fillWidth: true
                                spacing: 4

                                IdleToggle {
                                    label: tool.name
                                    // Both states are worth saying out loud, and
                                    // saying which one applies is the point of the
                                    // hint rather than decoration.
                                    hint: tool.enabled
                                          ? (tool.permitted
                                             ? tool.summary
                                             : "needs: " + tool.missing.join(", "))
                                          : "switch on to allow this"
                                    checked: tool.enabled
                                    onToggled: appController.setIdleToolEnabled(tool.name, !tool.enabled)
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 24
                                    spacing: 2
                                    visible: tool.required.length > 0

                                    Repeater {
                                        model: tool.required
                                        delegate: IdleToggle {
                                            required property string modelData
                                            required property var model
                                            readonly property string capability: modelData
                                            readonly property bool granted: !tool.missing.includes(capability)
                                            label: capability
                                            hint: granted
                                                  ? "granted"
                                                  : "Kestrel cannot do this until you allow it"
                                            checked: granted
                                            onToggled: appController.setToolPermission(capability, !granted)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Text { text: "LIVE GENERATION"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.4 }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: liveColumn.implicitHeight + 24
                    radius: 12
                    color: "#1a1d23"
                    border.color: appController.generating ? window.accent : window.line
                    Behavior on border.color { ColorAnimation { duration: 160 } }
                    ColumnLayout {
                        id: liveColumn
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 7
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text { text: "THROUGHPUT"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.2; Layout.fillWidth: true }
                            Text {
                                text: appController.tokensPerSecond > 0
                                      ? appController.tokensPerSecond.toFixed(1) + " tok/s"
                                      : "—"
                                color: appController.tokensPerSecond > 0 ? window.accent : window.muted
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text { text: "TOKENS"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.2; Layout.fillWidth: true }
                            Text { text: appController.tokensGenerated; color: window.ink; font.pixelSize: 13 }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text { text: "CONTEXT"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.2; Layout.fillWidth: true }
                            Text {
                                text: appController.contextSummary
                                color: window.ink; font.pixelSize: 11
                                horizontalAlignment: Text.AlignRight
                                elide: Text.ElideRight
                                Layout.maximumWidth: 160
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text { text: "KV CACHE"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.2; Layout.fillWidth: true }
                            Text {
                                text: appController.kvCacheSummary
                                color: window.ink; font.pixelSize: 11
                                horizontalAlignment: Text.AlignRight
                                elide: Text.ElideRight
                                Layout.maximumWidth: 160
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text { text: "SHARED PREFIX"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.2; Layout.fillWidth: true }
                            Text {
                                text: appController.prefixSummary
                                color: window.ink; font.pixelSize: 11
                                horizontalAlignment: Text.AlignRight
                                elide: Text.ElideRight
                                Layout.maximumWidth: 160
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text { text: "VOICE STATE"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.2; Layout.fillWidth: true }
                            Text { text: appController.voiceState; color: window.ink; font.pixelSize: 12 }
                        }
                    }
                }

                Text { text: "DIAGNOSTICS"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.4 }

                ListView {
                    id: diagnosticList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 10
                    model: appController.runtimeDiagnostics
                    boundsBehavior: Flickable.StopAtBounds
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 7
                            Rectangle {
                                width: 6; height: 6; radius: 3
                                color: modelData.ok ? window.accent : "#8a6a74"
                                Layout.alignment: Qt.AlignTop
                                Layout.topMargin: 4
                            }
                            Text {
                                text: modelData.label
                                color: window.ink
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                            }
                        }
                        Text {
                            text: modelData.value
                            color: window.muted
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                            Layout.leftMargin: 13
                        }
                    }
                }
            }
        }
    }

    // Native picker for a GGUF on disk. The URL is handed straight to the
    // controller, which does the local-path conversion; doing that in C++ is
    // far more dependable than trimming "file:///" off a percent-encoded
    // string, which is wrong on Windows and for any path containing a space.
    FileDialog {
        id: modelDialog
        title: "Choose a GGUF model"
        nameFilters: ["GGUF models (*.gguf)", "All files (*)"]
        onAccepted: appController.loadModelFromUrl(selectedFile.toString())
    }
}
