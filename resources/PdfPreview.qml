import QtQuick
import QtWebView

Rectangle {
    id: root
    color: "#f3f4f6"

    WebView {
        anchors.fill: parent
        url: pdfPreviewUrl
    }
}
