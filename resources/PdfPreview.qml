import QtQuick
import QtWebView

Rectangle {
    id: root
    required property url previewUrl
    color: "#f3f4f6"

    WebView {
        anchors.fill: parent
        url: root.previewUrl
    }
}
