// The document area: the engine's pages on the document canvas (DESK-002), with scroll bars, pinch zoom and the states before the
// document is ready.
import QtQuick
import QtQuick.Controls
import Bayan.Desktop

Rectangle {
    id: area

    required property DocumentSession session
    // The canvas, for the status bar's zoom display and the smoke test.
    readonly property alias canvas: canvas

    color: Theme.canvas
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Document area")

    DocumentCanvas {
        id: canvas

        objectName: "documentCanvas"
        anchors.fill: parent
        anchors.rightMargin: verticalBar.visible ? verticalBar.width : 0
        anchors.bottomMargin: horizontalBar.visible ? horizontalBar.height : 0
        visible: area.session.status === DocumentSession.Status.Ready
        session: area.session
        focus: true
        pageShadowColor: Theme.pageShadow
        pagePlaceholderColor: Theme.pagePlaceholder
        // Pages are drawn as the engine renders them, on white paper, whatever the interface theme: the caret stays dark.
        caretColor: "#000000"
        selectionColor: Theme.selection
        // Marks the canvas as an accessible item; its custom accessibility interface (CanvasAccessible) supplies the role and the text.
        Accessible.role: Accessible.EditableText
        Accessible.name: qsTr("Document")

        // Pinch to zoom on touchpads and touch screens, around the fingers. The tiles of the new zoom are asked for when the pinch ends.
        PinchHandler {
            id: pinch

            target: null
            onScaleChanged: delta => canvas.zoomAt(canvas.zoom * delta, pinch.centroid.position)
            onActiveChanged: {
                if (!active)
                    canvas.commitZoom();
            }
        }
    }

    ScrollBar {
        id: verticalBar

        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: canvas.bottom
        orientation: Qt.Vertical
        policy: ScrollBar.AlwaysOn
        visible: canvas.visible && canvas.contentHeight > canvas.height
        size: canvas.height / Math.max(canvas.contentHeight, 1)
        onPositionChanged: {
            if (pressed)
                canvas.contentY = position * canvas.contentHeight;
        }
        // Follows the canvas, except while the user drags it.
        Binding on position {
            when: !verticalBar.pressed
            value: canvas.contentY / Math.max(canvas.contentHeight, 1)
        }
    }

    ScrollBar {
        id: horizontalBar

        anchors.left: parent.left
        anchors.right: canvas.right
        anchors.bottom: parent.bottom
        orientation: Qt.Horizontal
        policy: ScrollBar.AlwaysOn
        visible: canvas.visible && canvas.contentWidth > canvas.width
        size: canvas.width / Math.max(canvas.contentWidth, 1)
        onPositionChanged: {
            if (pressed)
                canvas.contentX = position * canvas.contentWidth;
        }
        Binding on position {
            when: !horizontalBar.pressed
            value: canvas.contentX / Math.max(canvas.contentWidth, 1)
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
