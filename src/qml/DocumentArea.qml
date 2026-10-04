// Placeholder for the document canvas: shows the first page as one image rendered by the engine. DESK-002 replaces it with the tiled
// document canvas (scrolling, zoom, input methods and accessibility).
import QtQuick
import QtQuick.Controls
import Bayan.Desktop

Rectangle {
    id: area

    required property DocumentSession session

    color: Theme.canvas
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Document")

    ScrollView {
        id: scroller

        anchors.fill: parent
        contentWidth: Math.max(page.width + 48, scroller.width)
        contentHeight: page.height + 48
        visible: area.session.status === DocumentSession.Status.Ready

        Item {
            width: scroller.contentWidth
            height: scroller.contentHeight

            Rectangle {
                // A soft shadow under the page.
                x: page.x + 2
                y: page.y + 3
                width: page.width
                height: page.height
                color: Theme.pageShadow
            }

            Image {
                id: page

                objectName: "pageImage"
                x: Math.round((parent.width - width) / 2)
                y: 24
                width: area.session.pageSize.width
                height: area.session.pageSize.height
                // Render at the screen's real pixel density so that the page is sharp.
                sourceSize: Qt.size(Math.round(width * Screen.devicePixelRatio), Math.round(height * Screen.devicePixelRatio))
                source: area.session.pageCount > 0 ? "image://tiles/" + area.session.revision + "/0" : ""
                cache: false
                asynchronous: false
                Accessible.ignored: true
            }
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: area.session.status === DocumentSession.Status.Starting
        visible: running
    }

    Label {
        anchors.centerIn: parent
        visible: area.session.status === DocumentSession.Status.Failed
        text: qsTr("The document engine stopped working.")
        color: Theme.text
    }
}
