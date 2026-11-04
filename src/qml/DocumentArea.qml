// The document area, the "Pages" item: the engine's pages on the document canvas (DESK-002), with scroll bars and pinch zoom, the states
// before the document is ready, and what the shell says after the engine failed (engine protocol §12.1).
//
// It is a tab stop just before the canvas, and scrolls the pages with Page Up, Page Down, Home, End and the arrow keys, because the canvas
// sends every key but Tab to the engine; so scrolling stays keyboard-operable (ADR-0020 §5). When a failure closes the document, keyboard
// focus that was in it stays here, never on the offer to reopen it, so that a key the user meant for the document cannot accept the offer.
// Every text the engine or a document provides is shown as plain text (threat T18).
import QtQuick
import QtQuick.Controls
import Bayan.Desktop

Rectangle {
    id: area

    required property DocumentSession session
    // The canvas, for the status bar's zoom controls and the smoke test.
    readonly property alias canvas: canvas
    // The user took the offer while its button had the keyboard focus: the button goes with the offer, so focus waits here and goes into
    // the document once it is open.
    property bool focusDocumentWhenOpen: false

    color: Theme.canvas
    activeFocusOnTab: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Pages")
    Accessible.focusable: true

    Keys.onPressed: event => {
        event.accepted = area.canvas.scrollWithKey(event.key, event.modifiers);
    }

    Connections {
        target: area.session

        // Sent once the session has forgotten the document, before the canvas disappears with it: a hidden canvas would keep the keyboard
        // focus, and keys would go to a document that is no longer there (engine protocol §12.1, point 3).
        function onDocumentClosed() {
            if (canvas.activeFocus)
                area.forceActiveFocus(Qt.OtherFocusReason);
        }

        // The offer goes when the user takes it and at the stop (engine protocol §12.1, point 3). Keyboard focus on its button then moves
        // to the Pages, because a button that is hidden keeps the focus, and the next key would reach a control nobody sees (ADR-0020 §5).
        function onOfferChanged() {
            if (area.session.offer === DocumentSession.Offer.None && offerButton.activeFocus)
                area.forceActiveFocus(Qt.OtherFocusReason);
        }

        // Every message is announced, so that screen readers read it without the user moving focus (engine protocol §12.1, point 2).
        function onMessageChanged() {
            if (area.session.message !== "")
                area.Accessible.announce(area.session.message);
        }

        // Once the document the user asked for is open, keyboard focus that waited here goes into it; if it does not open, the wish goes.
        function onStatusChanged() {
            if (area.session.status === DocumentSession.Status.Ready && area.focusDocumentWhenOpen && area.activeFocus)
                canvas.forceActiveFocus(Qt.OtherFocusReason);
            if (area.session.status !== DocumentSession.Status.Starting)
                area.focusDocumentWhenOpen = false;
        }
    }

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

    // A frame around the document area while it has the keyboard focus, drawn above the canvas (ADR-0020 §5: visible focus).
    Rectangle {
        objectName: "pagesFocusFrame"
        anchors.fill: parent
        z: 1
        color: "transparent"
        border.width: 2
        border.color: Theme.accent
        visible: area.activeFocus
    }

    // While no document is shown: the progress, the session's message (after a failure, what became of the document) and the offer.
    Column {
        anchors.centerIn: parent
        width: Math.min(area.width - 48, 640)
        spacing: 16
        visible: area.session.status !== DocumentSession.Status.Ready

        BusyIndicator {
            anchors.horizontalCenter: parent.horizontalCenter
            running: area.session.status === DocumentSession.Status.Starting
            visible: running
        }

        Label {
            objectName: "sessionMessage"
            width: parent.width
            visible: text !== ""
            text: area.session.message
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            color: Theme.text
        }

        Button {
            id: offerButton

            objectName: "offerButton"
            anchors.horizontalCenter: parent.horizontalCenter
            visible: area.session.offer !== DocumentSession.Offer.None
            text: area.session.offer === DocumentSession.Offer.Open ? qsTr("Open the document") : qsTr("Reopen the document")
            // Explicitly, rather than as Qt Quick Controls copy it once a screen reader is active.
            Accessible.name: text
            onClicked: {
                if (activeFocus) {
                    area.focusDocumentWhenOpen = true;
                    area.forceActiveFocus(Qt.OtherFocusReason);
                }
                area.session.reopen();
            }
        }
    }
}
