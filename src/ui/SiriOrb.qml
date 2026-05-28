// Copyright 2026 Aananth C N
// Apache License, Version 2.0
//
// Siri-like orb that animates based on AssistantState:
//   0 IDLE      — tiny grey, motionless
//   1 LISTENING — medium blue, slow breathing pulse
//   2 RECORDING — larger orange-red, fast pulse
//   3 THINKING  — medium violet, rotating layers
//   4 TALKING   — large rainbow core + rotating rings + animated EQ bars

import QtQuick 2.15

Item {
    id: root

    // Set from outside: 0=IDLE 1=LISTENING 2=RECORDING 3=THINKING 4=TALKING
    property int assistantState: 0

    // Overall orb diameter — layers scale relative to this.
    property real orbSize: 200

    width:  orbSize
    height: orbSize

    // -----------------------------------------------------------------------
    // State → visual parameter tables
    // -----------------------------------------------------------------------
    readonly property var stateScale:   [0.30, 0.70, 0.72, 0.80, 1.00]
    readonly property var stateOpacity: [0.25, 0.80, 0.90, 0.85, 1.00]

    // Core colours per state (inner glow centre)
    readonly property var stateCoreColor: [
        "#555555",   // IDLE
        "#2979FF",   // LISTENING  — blue
        "#FF6D00",   // RECORDING  — orange
        "#7C4DFF",   // THINKING   — violet
        "#00E5FF"    // TALKING    — cyan (cycles via talkingHue)
    ]

    // Outer glow colours
    readonly property var stateGlowColor: [
        "#222222",
        "#0D47A1",
        "#BF360C",
        "#4527A0",
        "#006064"
    ]

    // -----------------------------------------------------------------------
    // Animated master scale + opacity (smooth cross-state transitions)
    // -----------------------------------------------------------------------
    property real targetScale:   stateScale[assistantState]
    property real targetOpacity: stateOpacity[assistantState]

    Behavior on targetScale   { NumberAnimation { duration: 400; easing.type: Easing.InOutQuad } }
    Behavior on targetOpacity { NumberAnimation { duration: 400 } }

    // -----------------------------------------------------------------------
    // Layer 1 — outermost glow ring (rotates slow during THINKING, fast during TALKING)
    // -----------------------------------------------------------------------
    Rectangle {
        id: glowRing
        anchors.centerIn: parent
        width:  root.orbSize * root.targetScale * 1.10
        height: width
        radius: width / 2
        opacity: root.targetOpacity * 0.40
        color: "transparent"
        border.width: root.orbSize * 0.045
        border.color: root.stateGlowColor[root.assistantState]

        Behavior on width        { NumberAnimation { duration: 400; easing.type: Easing.InOutQuad } }
        Behavior on border.color { ColorAnimation   { duration: 500 } }

        RotationAnimator on rotation {
            running: root.assistantState === 3 || root.assistantState === 4
            loops:   Animation.Infinite
            from:    0; to: 360
            duration: root.assistantState === 4 ? 1800 : 3000
        }
        scale: root.ringBreathScale   // shares transformOrigin with rotation — no conflict
    }

    // -----------------------------------------------------------------------
    // Layer 2 — mid ring (counter-rotates during TALKING for swirl illusion)
    // -----------------------------------------------------------------------
    Rectangle {
        id: midRing
        anchors.centerIn: parent
        width:  root.orbSize * root.targetScale * 0.90
        height: width
        radius: width / 2
        opacity: root.targetOpacity * 0.55
        color: "transparent"
        border.width: root.orbSize * 0.055
        border.color: root.stateCoreColor[root.assistantState]

        Behavior on width        { NumberAnimation { duration: 400; easing.type: Easing.InOutQuad } }
        Behavior on border.color { ColorAnimation   { duration: 600 } }

        RotationAnimator on rotation {
            running: root.assistantState === 4   // counter-rotates during TALKING
            loops:   Animation.Infinite
            from:    360; to: 0
            duration: 2400
        }
        scale: root.ringBreathScale   // shares transformOrigin with rotation — no conflict
    }

    // -----------------------------------------------------------------------
    // Layer 3 — solid core
    // During TALKING the gradient top/bottom follow talkingHue so the core
    // cycles through the same rainbow as the sine waves.
    // -----------------------------------------------------------------------
    Rectangle {
        id: core
        anchors.centerIn: parent
        width:  root.orbSize * root.targetScale * 0.72
        height: width
        radius: width / 2
        opacity: root.targetOpacity

        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: root.assistantState === 4
                       ? Qt.lighter(root.talkingHue, 1.5)
                       : Qt.lighter(root.stateCoreColor[root.assistantState], 1.4)
            }
            GradientStop {
                position: 1.0
                color: root.assistantState === 4
                       ? Qt.darker(root.talkingHue, 1.4)
                       : root.stateCoreColor[root.assistantState]
            }
        }

        Behavior on width { NumberAnimation { duration: 400; easing.type: Easing.InOutQuad } }
        scale: root.breatheScale
    }

    // -----------------------------------------------------------------------
    // TALKING EQ phase — advances 0→2π continuously; each bar multiplies by
    // its own frequency so bars oscillate at different rates (organic motion).
    // requestPaint() is called every time the property changes (every frame).
    // -----------------------------------------------------------------------
    property real eqPhase: 0.0

    onEqPhaseChanged: {
        if (root.assistantState === 4) eqCanvas.requestPaint()
    }

    NumberAnimation on eqPhase {
        running: root.assistantState === 4
        loops:   Animation.Infinite
        from:    0.0
        to:      6.2832   // 2π
        duration: 1800    // base oscillation period
    }

    // -----------------------------------------------------------------------
    // Equalizer canvas — 9 vertical bars centred on the horizontal midline.
    // Bars sweep both above and below centre (sin, not abs-sin).
    // Monochrome gradient: dark at the centre baseline, brightening toward
    // each tip.  Corner radius is clamped so very short bars near zero-
    // crossing never produce an invalid arc.  Circular clip gives the outer
    // bars a natural dome silhouette matching the orb boundary.
    // -----------------------------------------------------------------------
    Canvas {
        id: eqCanvas
        anchors.centerIn: parent
        width:  root.orbSize
        height: root.orbSize
        visible: root.assistantState === 4
        opacity: root.targetOpacity
        scale: root.breatheScale

        onPaint: {
            var ctx   = getContext("2d")
            var w     = width
            var h     = height
            var cx    = w * 0.5
            var cy    = h * 0.5
            var clipR = w * 0.36   // matches core radius

            ctx.clearRect(0, 0, w, h)

            ctx.save()
            ctx.beginPath()
            ctx.arc(cx, cy, clipR, 0, Math.PI * 2)
            ctx.clip()

            // ── faint centre reference line ──────────────────────────────────
            ctx.strokeStyle = "#909090"
            ctx.lineWidth   = 1.0
            ctx.globalAlpha = 0.30
            ctx.beginPath()
            ctx.moveTo(cx - clipR, cy)
            ctx.lineTo(cx + clipR, cy)
            ctx.stroke()

            // ── bar geometry — pill centred on cy ────────────────────────────
            var numBars  = 27
            var span     = clipR * 1.55         // total horizontal span
            var slotW    = span / numBars
            var barW     = slotW * 0.62
            var maxHalf  = clipR * 0.82         // max half-height (above AND below cy)
            var startX   = cx - span * 0.5

            // ── monochrome gradient: light at cy, ring-tinted tips ───────────
            // The gradient spans the full ±maxHalf range so short bars show
            // mostly light grey and tall bars bleed into the ring tint at tips.
            var ringColor = root.stateGlowColor[root.assistantState]
            var grad = ctx.createLinearGradient(0, cy - maxHalf, 0, cy + maxHalf)
            grad.addColorStop(0.0, ringColor)   // top tip    — outer-ring colour
            grad.addColorStop(0.5, "#D0D0D0")   // centre     — light grey
            grad.addColorStop(1.0, ringColor)   // bottom tip — outer-ring colour
            ctx.fillStyle   = grad
            ctx.globalAlpha = 0.92

            // ── per-bar oscillation parameters (golden-angle distribution) ───
            var freqs  = []
            var phases = []
            for (var j = 0; j < numBars; j++) {
                freqs.push(0.6  + (j * 1.6180) % 1.60)   // 0.6 … 2.2
                phases.push(     (j * 2.3999) % 6.2832)   // 0 … 2π
            }

            for (var i = 0; i < numBars; i++) {
                var t        = freqs[i] * root.eqPhase + phases[i]
                // abs(sin) → 0..1; min 10 % so bars are always visible
                var barHalf  = maxHalf * (0.10 + 0.90 * Math.abs(Math.sin(t)))
                var x        = startX + i * slotW + (slotW - barW) * 0.5
                var topY     = cy - barHalf
                var botY     = cy + barHalf
                // Corner radius clamped so very short pills still draw cleanly
                var r        = Math.min(barW * 0.35, barHalf * 0.40)

                // ── pill shape: rounded top AND bottom ───────────────────────
                ctx.beginPath()
                ctx.moveTo(x + r,        topY)
                ctx.lineTo(x + barW - r, topY)
                ctx.quadraticCurveTo(x + barW, topY, x + barW, topY + r)
                ctx.lineTo(x + barW,     botY - r)
                ctx.quadraticCurveTo(x + barW, botY, x + barW - r, botY)
                ctx.lineTo(x + r,        botY)
                ctx.quadraticCurveTo(x,  botY, x,    botY - r)
                ctx.lineTo(x,            topY + r)
                ctx.quadraticCurveTo(x,  topY, x + r, topY)
                ctx.closePath()
                ctx.fill()
            }

            ctx.restore()
        }
    }

    // -----------------------------------------------------------------------
    // Breathing pulse — applied per element, not on the root Item, so rings
    // and core+canvas can scale at independent speeds during TALKING.
    //
    // breatheScale    → core + eqCanvas  (full speed — bars jump with the beat)
    // ringBreathScale → glowRing + midRing  (same speed for LISTENING/RECORDING;
    //                   3× slower during TALKING so rings glide while bars pulse)
    // -----------------------------------------------------------------------
    property real breatheScale: 1.0

    SequentialAnimation on breatheScale {
        running: root.assistantState === 1   // LISTENING — very subtle ripple
        loops:   Animation.Infinite
        NumberAnimation { to: 1.03; duration: 1800; easing.type: Easing.InOutSine }
        NumberAnimation { to: 0.97; duration: 1800; easing.type: Easing.InOutSine }
    }
    SequentialAnimation on breatheScale {
        running: root.assistantState === 2   // RECORDING — fast pulse
        loops:   Animation.Infinite
        NumberAnimation { to: 1.10; duration: 250; easing.type: Easing.InOutSine }
        NumberAnimation { to: 0.90; duration: 250; easing.type: Easing.InOutSine }
    }
    SequentialAnimation on breatheScale {
        running: root.assistantState === 4   // TALKING — fast swell (600 ms)
        loops:   Animation.Infinite
        NumberAnimation { to: 1.20; duration: 600; easing.type: Easing.InOutSine }
        NumberAnimation { to: 0.82; duration: 600; easing.type: Easing.InOutSine }
    }

    property real ringBreathScale: 1.0

    SequentialAnimation on ringBreathScale {
        running: root.assistantState === 1
        loops:   Animation.Infinite
        NumberAnimation { to: 1.03; duration: 1800; easing.type: Easing.InOutSine }
        NumberAnimation { to: 0.97; duration: 1800; easing.type: Easing.InOutSine }
    }
    SequentialAnimation on ringBreathScale {
        running: root.assistantState === 2
        loops:   Animation.Infinite
        NumberAnimation { to: 1.10; duration: 250; easing.type: Easing.InOutSine }
        NumberAnimation { to: 0.90; duration: 250; easing.type: Easing.InOutSine }
    }
    SequentialAnimation on ringBreathScale {
        running: root.assistantState === 4   // TALKING — slow swell (1800 ms = 3× breatheScale)
        loops:   Animation.Infinite
        NumberAnimation { to: 1.20; duration: 1800; easing.type: Easing.InOutSine }
        NumberAnimation { to: 0.82; duration: 1800; easing.type: Easing.InOutSine }
    }

    // -----------------------------------------------------------------------
    // TALKING rainbow hue cycle — talkingHue drives the core gradient stops
    // above so the orb background also cycles through the rainbow.
    // -----------------------------------------------------------------------
    property color talkingHue: "#00E5FF"

    SequentialAnimation on talkingHue {
        running: root.assistantState === 4
        loops:   Animation.Infinite
        ColorAnimation { to: "#FF4081"; duration: 800 }   // cyan → pink
        ColorAnimation { to: "#7C4DFF"; duration: 800 }   // pink → violet
        ColorAnimation { to: "#00E5FF"; duration: 800 }   // violet → cyan
    }
}
