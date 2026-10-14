// The main window: a placeholder ribbon above the document area, and a status bar.
import QtQuick
import QtQuick.Controls
import Bayan.Desktop

ApplicationWindow {
    id: window

    required property DocumentSession session

    width: 1200
    height: 860
    visible: true
    title: qsTr("BayanDocs")
    color: Theme.window

    header: Ribbon {}

    DocumentArea {
        id: documentArea

        anchors.fill: parent
        session: window.session
    }

    footer: Rectangle {
        implicitHeight: statusText.implicitHeight + 8
        color: Theme.ribbon

        Label {
            id: statusText

            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 8
            color: Theme.mutedText
            text: window.session.status === DocumentSession.Status.Ready
                  ? qsTr("Engine %1 · %n page(s)", "", window.session.pageCount).arg(window.session.engineVersion)
                  : window.session.status === DocumentSession.Status.Failed ? qsTr("Engine stopped") : qsTr("Starting the engine…")
        }

        Label {
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 8
            color: Theme.mutedText
            visible: window.session.status === DocumentSession.Status.Ready
            text: qsTr("Zoom %1 %").arg(Math.round(documentArea.canvas.zoom * 100))
        }
    }
}
