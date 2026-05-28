// Copyright 2026 Aananth C N
// Apache License, Version 2.0

import QtQuick 2.15
import QtQuick.Window 2.15

Window {
    id: root
    title: "Velan"
    // Start maximized; width/height are used as the restored (un-maximized) size.
    width:      480
    height:     480
    visibility: Window.Maximized
    visible:    true
    color: "#0A0A0A"   // near-black background

    // Current assistant state (0=IDLE … 4=TALKING)
    property int  currentState:  0
    property bool connected:     false
    property string lastTranscript: ""
    property string lastResponse:   ""

    // Map int state → label string
    readonly property var stateLabels: ["Idle", "Listening…", "Recording…", "Thinking…", "Speaking…"]
    readonly property var stateLabelColors: ["#555", "#2979FF", "#FF6D00", "#7C4DFF", "#00E5FF"]

    // -----------------------------------------------------------------------
    // Wire C++ GrpcStateWatcher signals to QML properties
    // -----------------------------------------------------------------------
    Connections {
        target: stateWatcher

        function onStateChanged(state, transcript, response) {
            root.currentState    = state
            if (transcript !== "") root.lastTranscript = transcript
            if (response   !== "") root.lastResponse   = response
        }

        function onConnectedChanged(connected) {
            root.connected = connected
        }
    }

    // -----------------------------------------------------------------------
    // Orb — centred, scales to 70% of the narrower window dimension
    // -----------------------------------------------------------------------
    SiriOrb {
        id: orb
        anchors.centerIn: parent
        orbSize: Math.min(root.width, root.height) * 0.70
        assistantState: root.currentState
    }

    // -----------------------------------------------------------------------
    // State label
    // -----------------------------------------------------------------------
    Text {
        id: stateLabel
        anchors {
            horizontalCenter: parent.horizontalCenter
            bottom: parent.bottom
            bottomMargin: 60
        }
        text: root.stateLabels[root.currentState]
        color: root.stateLabelColors[root.currentState]
        font.pixelSize: 22
        font.family: "Sans"
        Behavior on color { ColorAnimation { duration: 400 } }
    }

    // -----------------------------------------------------------------------
    // Last transcript (shown while THINKING / TALKING)
    // -----------------------------------------------------------------------
    Text {
        anchors {
            horizontalCenter: parent.horizontalCenter
            bottom: stateLabel.top
            bottomMargin: 8
        }
        text: (root.currentState >= 3 && root.lastTranscript !== "")
              ? "“" + root.lastTranscript + "”"
              : ""
        color: "#AAAAAA"
        font.pixelSize: 14
        font.italic: true
        width: root.width * 0.85
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }

    // -----------------------------------------------------------------------
    // Connection status dot (top-right)
    // -----------------------------------------------------------------------
    Rectangle {
        id: connDot
        anchors { top: parent.top; right: parent.right; margins: 14 }
        width: 10; height: 10; radius: 5
        color: root.connected ? "#00C853" : "#FF1744"

        SequentialAnimation on opacity {
            running: !root.connected
            loops:   Animation.Infinite
            NumberAnimation { to: 0.2; duration: 600 }
            NumberAnimation { to: 1.0; duration: 600 }
        }
    }
    Text {
        anchors { right: connDot.left; rightMargin: 5; verticalCenter: connDot.verticalCenter }
        text: root.connected ? "Connected" : "Connecting…"
        color: "#666"; font.pixelSize: 11
    }
}
