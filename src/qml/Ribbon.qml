// Placeholder for the ribbon. The real ribbon is generated from the shared UI manifest (ADR-0019) in a later work package.
import QtQuick
import QtQuick.Controls

Rectangle {
    id: ribbon

    implicitHeight: 112
    color: Theme.ribbon

    Accessible.role: Accessible.ToolBar
    Accessible.name: qsTr("Ribbon")

    Column {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        TabBar {
            id: tabs

            background: null

            Repeater {
                model: [qsTr("Home"), qsTr("Insert"), qsTr("Layout"), qsTr("References"), qsTr("Review"), qsTr("View")]

                TabButton {
                    required property string modelData

                    text: modelData
                    width: implicitWidth
                }
            }
        }

        Label {
            text: qsTr("Ribbon placeholder: commands arrive with the shared UI manifest.")
            color: Theme.mutedText
            leftPadding: 8
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.ribbonBorder
    }
}
