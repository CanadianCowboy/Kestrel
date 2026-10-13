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
    property string privilegedRequestId: ""
    property string shellRequestId: ""

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
        // Height, and it has to be stated rather than inherited.
        //
        // The outer RowLayout had exactly one fixed-size child, the 14 pixel
        // indicator, and that is where its height came from. Moving the
        // indicator inside the hit target -- which is what makes the whole row
        // clickable -- left the layout with one child, a plain Item, whose
        // implicitHeight is zero. Every toggle in the panel then collapsed to
        // no height and its label drew on top of its neighbour's, and the
        // screenshot check could not see it because it looks for black bands
        // and this is text on text.
        implicitHeight: 18
        // Layouts derive their minimum from their children, not an overridden
        // implicitHeight. The hit-target wrapper has no minimum of its own.
        Layout.minimumHeight: 18
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
    // The memory panel and the runtime panel share the right-hand edge, so only
    // one of them is open at a time.
    property bool memoryOpen: false
    onMemoryOpenChanged: if (memoryOpen) { appController.diagnosticsOpen = false; window.activityOpen = false; browserPanel.open = false }
    // The activity panel shows tool calls and their results. It shares the same
    // right-hand edge, so it is exclusive with the other side panels.
    property bool activityOpen: false
    onActivityOpenChanged: if (activityOpen) { window.memoryOpen = false; appController.diagnosticsOpen = false; browserPanel.open = false }
    Connections {
        target: appController
        function onDiagnosticsOpenChanged() {
            if (appController.diagnosticsOpen) {
                window.memoryOpen = false
                window.activityOpen = false
                browserPanel.open = false
            }
        }
    }
    Shortcut { sequence: "Ctrl+M"; onActivated: window.memoryOpen = !window.memoryOpen }
    Shortcut { sequence: "Ctrl+Shift+A"; onActivated: window.activityOpen = !window.activityOpen }
    Shortcut { sequence: "Ctrl+Shift+B"; onActivated: browserPanel.open = !browserPanel.open }
    // Opening the browser closes the other side panels, the same way they close each other.
    Connections {
        target: browserPanel
        function onOpenChanged() {
            if (browserPanel.open) {
                window.memoryOpen = false
                window.activityOpen = false
                appController.diagnosticsOpen = false
            }
        }
    }
    Shortcut { sequence: "Ctrl+D"; onActivated: appController.diagnosticsOpen = !appController.diagnosticsOpen }
    Shortcut { sequence: "Escape"; onActivated: appController.stopSpeech() }

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

        // Plain anchors rather than a RowLayout. The layout engine was
        // shrinking the sidebar to zero beside the conversation column, so the
        // ☰ toggle had no visible effect.
        Item {
            anchors.fill: parent
            anchors.margins: 18

            Rectangle {
                id: sidebar
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                width: appController.sidebarOpen ? 268 : 0
                radius: 18
                color: "#b3171b20"
                border.color: window.line
                clip: true
                visible: width > 0
                Behavior on width { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }

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
                        implicitHeight: runtimeColumn.implicitHeight + 26
                        radius: 12
                        color: "#15181d"
                        border.color: window.line
                        Column {
                            id: runtimeColumn
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
                            Row {
                                spacing: 8
                                visible: appController.canLoadModel
                                Button {
                                    id: loadModelButton
                                    text: appController.modelPath.length > 0 ? "Change model" : "Load model"
                                    onClicked: modelDialog.open()
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
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: sidebar.right
                anchors.leftMargin: sidebar.width > 0 ? 14 : 0
                anchors.right: parent.right
                spacing: 0
                clip: true

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
                        id: activityBadge
                        implicitWidth: 96
                        implicitHeight: 34
                        radius: 17
                        color: window.activityOpen ? "#1e2a26" : "#17221f"
                        border.color: appController.activityEntries.length > 0 ? "#2d463d" : "#3a3038"
                        Accessible.name: "Open activity panel"
                        Text {
                            anchors.centerIn: parent
                            text: "ACTIVITY"
                            color: appController.activityEntries.length > 0 ? window.accent : "#b5909c"
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: window.activityOpen = !window.activityOpen
                        }
                    }
                    Rectangle {
                        id: browserBadge
                        implicitWidth: 96
                        implicitHeight: 34
                        radius: 17
                        color: browserPanel.open ? "#1e2a26" : "#17221f"
                        border.color: browserPanel.url.toString().length > 0 ? "#2d463d" : "#3a3038"
                        Accessible.name: "Open browser panel"
                        Text {
                            anchors.centerIn: parent
                            text: "BROWSER"
                            color: browserPanel.url.toString().length > 0 ? window.accent : "#b5909c"
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: browserPanel.open = !browserPanel.open
                        }
                    }
                    Rectangle {
                        id: memoryBadge
                        implicitWidth: 96
                        implicitHeight: 34
                        radius: 17
                        color: window.memoryOpen ? "#1e2a26" : "#17221f"
                        border.color: appController.recallEnabled ? "#2d463d" : "#3a3038"
                        Accessible.name: "Open memory panel"
                        Text {
                            anchors.centerIn: parent
                            text: "MEMORY"
                            color: appController.recallEnabled ? window.accent : "#b5909c"
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: window.memoryOpen = !window.memoryOpen
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
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: appController.speaking || appController.audioPaused || appController.summarizingSpeech
                                     || appController.pendingInterruption.length > 0
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    Layout.fillWidth: true
                                    text: appController.audioPaused ? "Speech paused · text continues"
                                          : appController.summarizingSpeech ? "Preparing a short spoken summary…"
                                          : appController.speakingSummary ? "Speaking summary · full answer remains in chat"
                                          : appController.canResume ? "Answer held"
                                          : "Speaking · progress through generated text"
                                    color: window.muted
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                                Button {
                                    objectName: "speechPauseButton"
                                    text: appController.audioPaused ? "Resume" : "Pause"
                                    visible: appController.pendingInterruption.length === 0
                                    enabled: appController.audioPaused ? appController.canResumeSpeech : appController.speaking
                                    onClicked: appController.audioPaused ? appController.resumeSpeech() : appController.pauseSpeech()
                                    ToolTip.visible: hovered
                                    ToolTip.text: "Pause at the next clause; text generation continues"
                                }
                                Button {
                                    objectName: "speechSkipButton"
                                    text: "Skip"
                                    enabled: appController.canSkipSpeech
                                    onClicked: appController.skipSpeech()
                                    ToolTip.visible: hovered
                                    ToolTip.text: "Skip the current spoken clause"
                                }
                                Button {
                                    objectName: "speechStopButton"
                                    text: "Stop audio"
                                    onClicked: appController.stopSpeech()
                                    ToolTip.visible: hovered
                                    ToolTip.text: "Stop speech; finish the written answer silently (Esc)"
                                }
                            }
                            ProgressBar {
                                objectName: "speechProgressBar"
                                Layout.fillWidth: true
                                from: 0
                                to: 1
                                value: appController.speechProgress
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: appController.pendingInterruption.length > 0
                                text: "Heard: " + appController.pendingInterruption
                                color: window.ink
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                            RowLayout {
                                visible: appController.pendingInterruption.length > 0
                                Button {
                                    objectName: "sendInterruptionButton"
                                    text: "Send new request"
                                    onClicked: appController.sendPendingInterruption()
                                }
                                Button {
                                    objectName: "resumeHeldAnswerButton"
                                    text: "Resume held answer"
                                    enabled: (appController.canResume || appController.canResumeSpeech) && !appController.speaking
                                    onClicked: appController.resumeInterruptedAnswer()
                                }
                            }
                        }

                        Item {
                                id: composerDock
                                Layout.fillWidth: true
                                // The dictation line sits above the shell and needs
                                // its own room: its implicit height plus the shell
                                // plus the 6 pixel gap and 6 pixel bottom margin. 84 is the
                                // shell's height, and it is fixed rather than a
                                // fill -- a shell that grows with the dock
                                // swallows the line that the dock grew for.
                                Layout.preferredHeight: dictationLine.visible
                                                      ? dictationLine.implicitHeight + 96 : 90


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
                                anchors.bottom: composerShell.top
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
                                        ToolTip.visible: hovered
                                        ToolTip.text: appController.echoCancellationDetail
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
                                        objectName: "conversationPauseButton"
                                        // Only offered when the voice state machine
                                        // says the transition is actually legal, so
                                        // the button can never be a no-op that
                                        // silently does nothing.
                                        visible: !appController.ttsAvailable && (appController.canPause || appController.canResume)
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
                                        objectName: "generationStopButton"
                                        implicitWidth: 44
                                        implicitHeight: 44
                                        text: appController.generating ? "■" : "↑"
                                        ToolTip.visible: hovered
                                        ToolTip.text: appController.generating ? "Cancel generation and speech" : "Send message"
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
        // The page the agent or the user opened. The engine is loaded only while the
        // panel is open. Without Qt WebEngine in this build, the panel says so and
        // pages open in the default browser instead.
        Rectangle {
            id: browserSidePanel
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.margins: 18
            width: 620
            radius: 18
            color: "#15171c"
            border.color: window.line
            visible: browserPanel.open
            opacity: browserPanel.open ? 1 : 0
            clip: true

            Behavior on opacity {
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        text: "Browser"
                        color: window.ink
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: browserPanel.url.toString()
                        color: "#8b93a3"
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        implicitWidth: 26
                        implicitHeight: 26
                        radius: 13
                        color: "#1d2026"
                        border.color: window.line
                        Accessible.name: "Close browser panel"
                        Text { anchors.centerIn: parent; text: "×"; color: window.ink; font.pixelSize: 14 }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: browserPanel.open = false
                        }
                    }
                }

                Loader {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    active: browserPanel.open && browserPanel.webEngineAvailable
                    source: "BrowserView.qml"
                }

                Text {
                    Layout.fillWidth: true
                    visible: !browserPanel.webEngineAvailable
                    text: "The embedded browser is not built into this copy of Kestrel. Pages open in your default browser."
                    color: "#8b93a3"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
        }

        // What the agent has done this session: each tool call, its arguments, and
        // what came back, including web search results. Read-only except for clear.
        Rectangle {
            id: activityPanel
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.margins: 18
            width: 380
            radius: 18
            color: "#15171c"
            border.color: window.line
            visible: window.activityOpen
            opacity: window.activityOpen ? 1 : 0
            clip: true

            Behavior on opacity {
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "Activity"
                        color: window.ink
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        Layout.fillWidth: true
                    }
                    Text {
                        text: appController.activityEntries.length + " events"
                        color: "#8b93a3"
                        font.pixelSize: 11
                    }
                    Rectangle {
                        implicitWidth: 60
                        implicitHeight: 26
                        radius: 13
                        color: "#1d2026"
                        border.color: window.line
                        Text { anchors.centerIn: parent; text: "Clear"; color: window.ink; font.pixelSize: 11 }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: appController.clearActivity()
                        }
                    }
                    Rectangle {
                        implicitWidth: 26
                        implicitHeight: 26
                        radius: 13
                        color: "#1d2026"
                        border.color: window.line
                        Accessible.name: "Close activity panel"
                        Text { anchors.centerIn: parent; text: "×"; color: window.ink; font.pixelSize: 14 }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: window.activityOpen = false
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        text: appController.undermindStatus
                        color: "#8b93a3"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        implicitWidth: 132
                        implicitHeight: 26
                        radius: 13
                        color: "#1d2026"
                        border.color: window.line
                        Accessible.name: "Connect Undermind"
                        Text { anchors.centerIn: parent; text: "Connect Undermind"; color: window.accent; font.pixelSize: 11 }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: appController.connectUndermind()
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: "Tool calls appear here as the model makes them. Each shows the arguments it sent and the output it got back."
                    color: "#8b93a3"
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }

                ListView {
                    id: activityList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 8
                    model: appController.activityEntries
                    onCountChanged: positionViewAtEnd()

                    delegate: Rectangle {
                        required property var modelData
                        width: activityList.width
                        height: entryColumn.implicitHeight + 20
                        radius: 10
                        color: modelData.isError ? "#2a1b1f" : "#1b1e25"
                        border.color: modelData.isError ? "#4a2a33" : window.line

                        Column {
                            id: entryColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 10
                            spacing: 4
                            Text {
                                text: (modelData.kind === "call" ? "CALL " : modelData.isError ? "FAILED " : "RESULT ") + modelData.name
                                color: modelData.isError ? "#e0a0ad" : window.accent
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                            Text {
                                width: parent.width
                                text: modelData.text
                                color: window.ink
                                font.family: "monospace"
                                font.pixelSize: 11
                                wrapMode: Text.WrapAnywhere
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: activityList.count === 0
                        text: "No tool calls yet."
                        color: "#8b93a3"
                        font.pixelSize: 12
                    }
                }
            }
        }

        // What Kestrel remembers. Stored in the profile in the system keystore, and
        // off until recall is switched on here. Every change is saved before it is
        // shown as kept; a change that cannot be saved is undone and says why.
        Rectangle {
            id: memoryPanel
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.margins: 18
            width: 360
            radius: 18
            color: "#15171c"
            border.color: window.line
            visible: window.memoryOpen
            opacity: window.memoryOpen ? 1 : 0
            clip: true
            onVisibleChanged: clearButton.armed = false

            Behavior on opacity {
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }

            ScrollView {
                id: memoryScroll
                anchors.fill: parent
                anchors.margins: 18
                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: memoryScroll.availableWidth
                    height: implicitHeight
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        Column {
                            spacing: 3
                            Text { text: "Memory"; color: window.ink; font.pixelSize: 14; font.weight: Font.DemiBold }
                            Text { text: "Ctrl+M to close  ·  kept only on this machine"; color: window.muted; font.pixelSize: 10 }
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            objectName: "memoryCloseButton"
                            text: "✕"
                            implicitWidth: 30
                            implicitHeight: 30
                            Accessible.name: "Close memory panel"
                            onClicked: window.memoryOpen = false
                            contentItem: Text { text: parent.text; color: window.ink; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { radius: 9; color: "#1e222a"; border.color: window.line }
                        }
                    }

                    IdleToggle {
                        label: "Remember across sessions"
                        hint: appController.recallEnabled
                              ? "on: facts you add are kept on this machine"
                              : "off: nothing new is stored"
                        checked: appController.recallEnabled
                        onToggled: appController.setRecallEnabled(!appController.recallEnabled)
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: appController.recallError.length > 0
                        text: appController.recallError
                        color: "#c58b98"
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        TextField {
                            id: memoryInput
                            Layout.fillWidth: true
                            placeholderText: "Something to remember"
                            Accessible.name: "Text to remember"
                            onAccepted: rememberButton.clicked()
                        }
                        Button {
                            id: rememberButton
                            text: "Remember"
                            enabled: appController.recallEnabled && memoryInput.text.trim().length > 0
                            onClicked: {
                                if (appController.rememberFact(memoryInput.text)) {
                                    memoryInput.text = ""
                                }
                            }
                            contentItem: Text { text: parent.text; color: window.ink; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { radius: 9; color: "#1e222a"; border.color: window.line }
                        }
                    }

                    TextField {
                        id: memorySearch
                        Layout.fillWidth: true
                        placeholderText: "Search memories"
                        Accessible.name: "Search memories"
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: appController.recallMemories.length === 0
                        text: appController.recallEnabled
                              ? "Nothing remembered yet."
                              : "Nothing remembered. Turn recall on to keep facts."
                        color: window.muted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        // Reading recallMemories here makes this binding re-run on
                        // every change to what is remembered, not only on a new
                        // search. searchMemories is a method, so QML cannot see
                        // that it depends on the store by itself.
                        model: {
                            appController.recallMemories
                            return appController.searchMemories(memorySearch.text)
                        }
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: modelData.kind + "  ·  " + modelData.createdAt
                                color: window.muted
                                font.pixelSize: 10
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.text
                                color: window.ink
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                            }
                            Button {
                                text: "Forget"
                                implicitHeight: 26
                                Accessible.name: "Forget this memory"
                                onClicked: appController.forgetMemory(modelData.id)
                                contentItem: Text { text: parent.text; color: window.ink; font.pixelSize: 10; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                background: Rectangle { radius: 8; color: "#1e222a"; border.color: window.line }
                            }
                        }
                    }

                    // Clearing everything takes two presses, so a stray click
                    // cannot wipe what Kestrel has learned.
                    Button {
                        id: clearButton
                        property bool armed: false
                        text: armed ? "Confirm: forget everything" : "Clear all"
                        enabled: appController.recallMemories.length > 0
                        Accessible.name: "Clear all memories"
                        onClicked: {
                            if (armed) {
                                appController.clearMemories()
                                armed = false
                            } else {
                                armed = true
                            }
                        }
                        contentItem: Text { text: parent.text; color: window.ink; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 9; color: "#1e222a"; border.color: window.line }
                    }
                }
            }
        }

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

            ScrollView {
                id: diagnosticsScroll
                anchors.fill: parent
                anchors.margins: 18
                clip: true
                contentWidth: availableWidth

            ColumnLayout {
                width: diagnosticsScroll.availableWidth
                height: implicitHeight
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    Column {
                        spacing: 3
                        Text { text: "Runtime"; color: window.ink; font.pixelSize: 14; font.weight: Font.DemiBold }                    Text {
                        text: "Ctrl+D to close  ·  click Refresh to re-probe"; color: window.muted; font.pixelSize: 10 }
                    }
                    Item { Layout.fillWidth: true }
                    Button {
                        objectName: "diagnosticsCloseButton"
                        text: "✕"
                        implicitWidth: 30
                        implicitHeight: 30
                        Accessible.name: "Close runtime panel"
                        onClicked: appController.diagnosticsOpen = false
                        contentItem: Text { text: parent.text; color: window.ink; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 9; color: "#1e222a"; border.color: window.line }
                    }

                    Button {
                        text: "Refresh"
                        implicitHeight: 30
                        onClicked: appController.refreshRuntime()
                        contentItem: Text { text: parent.text; color: window.ink; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 9; color: "#1e222a"; border.color: window.line }
                    }
                }

                Button {
                    objectName: "dictationSettingsButton"
                    Layout.fillWidth: true
                    visible: appController.linuxDictationSettings
                    text: "Dictation settings…"
                    onClicked: dictationSettingsDialog.open()
                    contentItem: Text { text: parent.text; color: window.ink; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { radius: 9; color: "#1e222a"; border.color: window.line }
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

                        IdleToggle {
                            objectName: "automaticSpeechSummaryToggle"
                            label: "Summarize long spoken answers"
                            hint: "Stream the opening, then use the local model to summarize only the unspoken remainder. Written answers stay unchanged; AI summaries may omit or misstate detail."
                            checked: appController.automaticSpeechSummary
                            onToggled: appController.automaticSpeechSummary = !appController.automaticSpeechSummary
                        }
                        IdleToggle {
                            objectName: "echoCancellationToggle"
                            visible: appController.linuxDictationSettings
                            label: "Echo-canceled listening"
                            hint: "Local WebRTC processing of the default microphone with the default output monitor as reference; capture starts only on mic request."
                            checked: appController.echoCancellationEnabled
                            onToggled: appController.echoCancellationEnabled = !appController.echoCancellationEnabled
                        }
                        Text {
                            Layout.fillWidth: true
                            text: appController.echoCancellationDetail
                            color: appController.echoCancellationActive ? window.accent : window.muted
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
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
                                        readonly property string voiceName: modelData
                                        readonly property bool selected:
                                            voiceName === appController.currentVoice
                                        width: profileText.implicitWidth + 16
                                        height: 22
                                        radius: 11
                                        color: selected ? window.accent : "#1a1d23"
                                        border.color: selected ? window.accent : window.line
                                        Behavior on color { ColorAnimation { duration: 140 } }

                                        Text {
                                            id: profileText
                                            anchors.centerIn: parent
                                            text: parent.voiceName
                                            color: parent.selected ? "#0d1015" : window.muted
                                            font.pixelSize: 11
                                        }

                                        MouseArea {
                                            id: profileHit
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: appController.setSpeechVoice(parent.voiceName)
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

                Text { text: "RUNTIME CAPABILITIES"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.4 }
                Text {
                    Layout.fillWidth: true
                    text: "Local inference runtime and device availability."
                    color: window.muted
                    font.pixelSize: 10
                    wrapMode: Text.Wrap
                }

                ListView {
                    id: diagnosticList
                    Layout.fillWidth: true
                    Layout.preferredHeight: contentHeight
                    interactive: false
                    clip: true
                    spacing: 10
                    model: appController.runtimeDiagnostics
                    boundsBehavior: Flickable.StopAtBounds
                    delegate: ColumnLayout {
                        required property var modelData
                        width: diagnosticList.width
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
    }

    Dialog {
        id: dictationSettingsDialog
        objectName: "dictationSettingsDialog"
        title: "Linux dictation"
        modal: true
        anchors.centerIn: parent
        width: Math.min(560, window.width - 48)
        standardButtons: Dialog.Close
        onOpened: {
            dictationModel.text = appController.dictationModelPath
            appController.refreshDictationMicrophones()
            dictationDevice.editText = appController.dictationAudioDevice
        }

        contentItem: ColumnLayout {
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: "Offline whisper.cpp + ALSA. Select existing local weights; nothing is downloaded. Audio stays in memory."
                color: window.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            Label { text: "Whisper ggml model (.bin)"; color: window.ink }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: dictationModel
                    objectName: "dictationModelField"
                    Layout.fillWidth: true
                    placeholderText: "Choose a local model; empty disables dictation"
                    Accessible.name: "Whisper model path"
                    selectByMouse: true
                }
                Button {
                    objectName: "dictationBrowseButton"
                    text: "Browse…"
                    onClicked: dictationModelDialog.open()
                }
            }
            Label { text: "Microphone / ALSA capture device"; color: window.ink }
            RowLayout {
                Layout.fillWidth: true
                ComboBox {
                    id: dictationDevice
                    objectName: "dictationDeviceField"
                    Layout.fillWidth: true
                    editable: true
                    model: appController.dictationMicrophones
                    Accessible.name: "ALSA microphone device"
                }
                Button {
                    text: "Refresh"
                    onClicked: {
                        const draft = dictationDevice.editText
                        appController.refreshDictationMicrophones()
                        dictationDevice.editText = draft
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                text: "Use default for the desktop audio route, or enter a custom ALSA name such as plughw:1,0. Refresh lists capture hints without opening the microphone."
                color: window.muted
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            Button {
                objectName: "dictationApplyButton"
                text: "Apply and save"
                enabled: !appController.listening
                onClicked: {
                    if (appController.applyDictationSettings(dictationModel.text, dictationDevice.editText)) {
                        dictationModel.text = appController.dictationModelPath
                        dictationDevice.editText = appController.dictationAudioDevice
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                text: appController.listening
                      ? "Stop dictation before changing its model or microphone."
                      : "Saved choices take effect on the next recording and persist after restart. Model loading is checked when you start dictation."
                color: window.muted
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            Text {
                objectName: "dictationSettingsStatus"
                Layout.fillWidth: true
                text: appController.dictationSettingsError.length > 0
                      ? appController.dictationSettingsError : appController.sttDetail
                color: appController.dictationSettingsError.length > 0 ? "#dc9292" : window.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }
    }

    // An administrator command waits here for a person. Nothing runs until
    // Approve is pressed, and Deny is the focused default so a stray Enter
    // refuses rather than grants.
    // A command the agent wants to run in the shell. Approval is per command; the
    // dialog shows the exact text so nothing runs that the user did not read.
    Connections {
        target: shellBroker
        function onRequestPending(requestId, program, commandLine, reason) {
            window.shellRequestId = requestId
            shellCommandText.text = commandLine
            shellReasonText.text = reason.length > 0 ? reason : "The assistant did not give a reason."
            shellDialog.open()
        }
        function onRequestResolved(requestId) {
            if (requestId === window.shellRequestId) {
                shellDialog.close()
            }
        }
    }

    Popup {
        id: shellDialog
        objectName: "shellApprovalDialog"
        modal: true
        focus: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: Overlay.overlay
        width: 560
        padding: 22
        background: Rectangle { radius: 16; color: window.panelRaised; border.color: window.line }
        onOpened: shellDenyButton.forceActiveFocus()

        contentItem: ColumnLayout {
            spacing: 14
            Text {
                text: "Allow the assistant to run this command?"
                color: window.ink
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                text: "The command runs in your home folder as you, with your permissions. Its output goes back to the assistant."
                color: window.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: shellColumn.implicitHeight + 24
                radius: 10
                color: "#15171c"
                border.color: window.line
                Column {
                    id: shellColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    spacing: 6
                    Text { text: "COMMAND"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.4 }
                    Text {
                        id: shellCommandText
                        width: parent.width
                        color: window.ink
                        font.family: "monospace"
                        font.pixelSize: 12
                        wrapMode: Text.WrapAnywhere
                    }
                    Text { text: "REASON"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.4 }
                    Text {
                        id: shellReasonText
                        width: parent.width
                        color: window.ink
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Item { Layout.fillWidth: true }
                Button {
                    id: shellDenyButton
                    objectName: "shellDenyButton"
                    text: "Deny"
                    onClicked: shellBroker.respond(window.shellRequestId, false)
                }
                Button {
                    objectName: "shellApproveButton"
                    text: "Approve"
                    onClicked: shellBroker.respond(window.shellRequestId, true)
                }
            }
        }
    }

    Connections {
        target: privilegedBroker
        function onRequestPending(requestId, program, commandLine, reason) {
            window.privilegedRequestId = requestId
            privilegedCommandText.text = commandLine
            privilegedReasonText.text = reason.length > 0 ? reason : "The assistant did not give a reason."
            privilegedDialog.open()
        }
        function onRequestResolved(requestId) {
            if (requestId === window.privilegedRequestId) {
                privilegedDialog.close()
            }
        }
    }

    Popup {
        id: privilegedDialog
        objectName: "privilegedApprovalDialog"
        modal: true
        focus: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: Overlay.overlay
        width: 520
        padding: 22
        background: Rectangle { radius: 16; color: window.panelRaised; border.color: window.line }
        onOpened: denyButton.forceActiveFocus()

        contentItem: ColumnLayout {
            spacing: 14
            Text {
                text: "Allow an administrator command?"
                color: window.ink
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                text: "An agent asked to run this with administrator rights. Your system's password dialog appears only if you approve."
                color: window.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: commandColumn.implicitHeight + 24
                radius: 10
                color: "#15171c"
                border.color: window.line
                Column {
                    id: commandColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    spacing: 6
                    Text { text: "COMMAND"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.4 }
                    Text {
                        id: privilegedCommandText
                        width: parent.width
                        color: window.ink
                        font.family: "monospace"
                        font.pixelSize: 12
                        wrapMode: Text.WrapAnywhere
                    }
                    Text { text: "REASON"; color: window.muted; font.pixelSize: 10; font.letterSpacing: 1.4 }
                    Text {
                        id: privilegedReasonText
                        width: parent.width
                        color: window.ink
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Item { Layout.fillWidth: true }
                Button {
                    id: denyButton
                    objectName: "privilegedDenyButton"
                    text: "Deny"
                    onClicked: {
                        privilegedBroker.respond(window.privilegedRequestId, false)
                    }
                }
                Button {
                    objectName: "privilegedApproveButton"
                    text: "Approve"
                    onClicked: {
                        privilegedBroker.respond(window.privilegedRequestId, true)
                    }
                }
            }
        }
    }

    FileDialog {
        id: dictationModelDialog
        title: "Choose a local Whisper ggml model"
        nameFilters: ["Whisper ggml models (*.bin)", "All files (*)"]
        onAccepted: dictationModel.text = selectedFile.toString()
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
