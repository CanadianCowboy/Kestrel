import QtQuick
import QtWebEngine

// The embedded browser for the browser panel. It is loaded only while the panel
// is open, so the browser engine does not start until a page is shown.
WebEngineView {
    url: browserPanel.url
}
