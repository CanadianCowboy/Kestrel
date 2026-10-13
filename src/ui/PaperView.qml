import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Pdf

// The pages of the paper the panel shows, with zoom controls. It is loaded only
// while the paper panel is open, so Qt PDF renders nothing until someone looks.
Item {
    id: paperView

    property real zoomLevel: 1.2

    RowLayout {
        id: zoomBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 8

        Text {
            text: pdfDocument.pageCount > 0
                ? "Page " + (pages.currentPage + 1) + " of " + pdfDocument.pageCount
                : ""
            color: "#8b93a3"
            font.pixelSize: 12
        }
        Item { Layout.fillWidth: true }
        Button {
            objectName: "paperZoomOut"
            text: "−"
            onClicked: paperView.zoomLevel = Math.max(0.6, paperView.zoomLevel - 0.2)
        }
        Text {
            text: Math.round(paperView.zoomLevel * 100) + "%"
            color: "#e8ebf2"
            font.pixelSize: 12
        }
        Button {
            objectName: "paperZoomIn"
            text: "+"
            onClicked: paperView.zoomLevel = Math.min(3.0, paperView.zoomLevel + 0.2)
        }
        Button {
            objectName: "paperZoomReset"
            text: "100%"
            onClicked: paperView.zoomLevel = 1.2
        }
    }

    PdfMultiPageView {
        id: pages
        anchors.top: zoomBar.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 8
        document: PdfDocument {
            id: pdfDocument
            source: paperLibrary.currentSource
        }
        renderScale: paperView.zoomLevel
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - 40
        visible: pdfDocument.status === PdfDocument.Error
        text: "This PDF could not be opened. The file may be damaged or password-protected."
        color: "#8b93a3"
        font.pixelSize: 12
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
    }
}
