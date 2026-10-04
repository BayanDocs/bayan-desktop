// Colours of the interface, following the system's light or dark colour scheme. Documents themselves stay as the engine draws them.
pragma Singleton

import QtQuick

QtObject {
    readonly property bool dark: Application.styleHints.colorScheme === Qt.ColorScheme.Dark

    readonly property color window: dark ? "#1f1f1f" : "#f3f3f3"
    readonly property color ribbon: dark ? "#2b2b2b" : "#ffffff"
    readonly property color ribbonBorder: dark ? "#3c3c3c" : "#d6d6d6"
    readonly property color text: dark ? "#f0f0f0" : "#1f1f1f"
    readonly property color mutedText: dark ? "#b4b4b4" : "#5f5f5f"
    readonly property color accent: dark ? "#6ea8ff" : "#1f5fbf"
    readonly property color canvas: dark ? "#141414" : "#e6e6e6"
    readonly property color pageShadow: dark ? "#66000000" : "#33000000"
}
