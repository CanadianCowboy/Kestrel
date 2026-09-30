
import QtQuick
import QtQuick.Layouts

Item {
    id: root
    width: 420
    height: 420

    // The one line that separates the two panels. Absent, a RowLayout with a
    // single plain-Item child has nothing to take an implicit height from,
    // which is precisely what shipped.
    property bool rowsHaveHeight: true

    component Switch: RowLayout {
        id: switchRow
        required property string label
        Layout.fillWidth: true
        Layout.maximumHeight: root.rowsHaveHeight ? 18 : 0
        implicitHeight: root.rowsHaveHeight ? 18 : 0

        // A hit target around the whole row, so the label is what you click.
        Item {
            Layout.fillWidth: true
            Rectangle {
                objectName: "dot"
                width: 14; height: 14
                anchors.verticalCenter: parent.verticalCenter
                color: "#4d5a68"
            }
            Text {
                objectName: "label"
                text: switchRow.label
                anchors.verticalCenter: parent.verticalCenter
                font.pixelSize: 11
            }
            MouseArea { anchors.fill: parent }
        }
    }

    ColumnLayout {
        objectName: "column"
        anchors.fill: parent
        spacing: 6

        Switch { objectName: "idleLoop";  label: "Idle loop" }
        Switch { objectName: "gpuPrewarm"; label: "GPU prewarm" }
        Switch { objectName: "thoughts";   label: "Show thoughts" }
    }
}
