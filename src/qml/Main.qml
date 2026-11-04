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

        objectName: "pages"
        anchors.fill: parent
        session: window.session
    }

    // The status bar: the engine and the document on the left, the zoom controls on the right. Every text the engine provides is shown as
    // plain text, so that markup in it is shown as it is and can neither change the label nor load an image (threat T18).
    footer: Rectangle {
        implicitHeight: Math.max(statusText.implicitHeight, zoomControls.implicitHeight) + 8
        color: Theme.ribbon

        Label {
            id: statusText

            objectName: "statusText"
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 8
            color: Theme.mutedText
            textFormat: Text.PlainText
            text: {
                switch (window.session.status) {
                case DocumentSession.Status.Ready:
                    return qsTr("Engine %1 · %n page(s)", "", window.session.pageCount).arg(window.session.engineVersion);
                case DocumentSession.Status.Closed:
                    return qsTr("No document open");
                case DocumentSession.Status.Failed:
                    return qsTr("Engine stopped");
                default:
                    return window.session.engineVersion === "" ? qsTr("Starting the engine…") : qsTr("Opening the document…");
                }
            }
        }

        // Zoom out and Zoom in step through the canvas's zoom steps, and the new level is announced to screen readers, as on the web: the
        // keyboard's way to zoom, since the canvas sends Control with =, - and 0 to the engine (ADR-0020 §5).
        Row {
            id: zoomControls

            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 8
            spacing: 4

            ToolButton {
                id: zoomOutButton

                objectName: "zoomOutButton"
                text: "\u2212"
                enabled: documentArea.canvas.canZoomOut
                Accessible.name: qsTr("Zoom out")
                ToolTip.text: qsTr("Zoom out")
                ToolTip.visible: hovered
                onClicked: zoomLevel.zoom(zoomOutButton, zoomInButton, false)
            }

            Label {
                id: zoomLevel

                // Zooms with one of the buttons and announces the new level. A button is disabled at its end of the range: if it had the
                // keyboard focus, the focus moves to the other one instead of being lost.
                function zoom(button: ToolButton, other: ToolButton, zoomIn: bool) {
                    const hadFocus = button.activeFocus;
                    if (zoomIn)
                        documentArea.canvas.zoomIn();
                    else
                        documentArea.canvas.zoomOut();
                    if (hadFocus && !button.enabled)
                        other.forceActiveFocus(Qt.TabFocusReason);
                    zoomLevel.Accessible.announce(zoomLevel.text);
                }

                objectName: "zoomLevel"
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.mutedText
                textFormat: Text.PlainText
                text: qsTr("Zoom %1 %").arg(Math.round(documentArea.canvas.zoom * 100))
            }

            ToolButton {
                id: zoomInButton

                objectName: "zoomInButton"
                text: "+"
                enabled: documentArea.canvas.canZoomIn
                Accessible.name: qsTr("Zoom in")
                ToolTip.text: qsTr("Zoom in")
                ToolTip.visible: hovered
                onClicked: zoomLevel.zoom(zoomInButton, zoomOutButton, true)
            }
        }
    }
}
