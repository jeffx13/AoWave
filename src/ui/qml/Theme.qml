pragma Singleton
import QtQuick

QtObject {
    id: theme

    property string name: "nightfall"
    property string customAccent: ""   // empty = the theme's own accent

    // Logical pixels: scale at the point of use with Globals.sp().
    readonly property int bodySize: 21
    readonly property int labelSize: 21
    readonly property int buttonSize: 21
    readonly property int headingSize: 28
    readonly property int metadataSize: 17
    readonly property int compactSize: 18
    readonly property int scrollbarGutter: 16
    property bool reduceMotion: false

    // Every palette is a full set: two accents, four surfaces, two borders, three text weights.
    readonly property var dark: ({
        "nightfall": {
            label: "Nightfall",
            accent: "#7C83FF", accent2: "#C084FC",
            background: "#0F1329", bgBottom: "#080B1B",
            surface: "#171C3A", surfaceAlt: "#202750", surfaceRaised: "#262E5C", surfaceDeep: "#0B0E22",
            border: "#2B3466", borderStrong: "#3D478A",
            textPrimary: "#ECEEFF", textSecondary: "#C3C8EC", textMuted: "#8A90BD",
            info: "#60A5FA", success: "#34D399", warning: "#FBBF24", danger: "#FB7185",
            selection: "#2E3A8C"
        },
        "sakura": {
            label: "Sakura Night",
            accent: "#F472B6", accent2: "#FB923C",
            background: "#1C1224", bgBottom: "#110A18",
            surface: "#26182F", surfaceAlt: "#331F3D", surfaceRaised: "#3B2547", surfaceDeep: "#150C1B",
            border: "#45304F", borderStrong: "#5C4268",
            textPrimary: "#FBEFF6", textSecondary: "#DCC6D4", textMuted: "#A38AA0",
            info: "#93C5FD", success: "#6EE7B7", warning: "#FCD34D", danger: "#FB7185",
            selection: "#6B2A55"
        },
        "tokyo": {
            label: "Tokyo",
            accent: "#7AA2F7", accent2: "#BB9AF7",
            background: "#1A1B26", bgBottom: "#13141C",
            surface: "#24283B", surfaceAlt: "#2F3450", surfaceRaised: "#3B4261", surfaceDeep: "#16161E",
            border: "#3B4261", borderStrong: "#545C7E",
            textPrimary: "#C0CAF5", textSecondary: "#A9B1D6", textMuted: "#7E85A8",
            info: "#7DCFFF", success: "#9ECE6A", warning: "#E0AF68", danger: "#F7768E",
            selection: "#33467C"
        },
        "aurora": {
            label: "Aurora",
            accent: "#2DD4BF", accent2: "#A3E635",
            background: "#0B1A1E", bgBottom: "#061114",
            surface: "#12262B", surfaceAlt: "#1A343A", surfaceRaised: "#214147", surfaceDeep: "#08171B",
            border: "#24444B", borderStrong: "#345B63",
            textPrimary: "#E3F6F4", textSecondary: "#B5D8D6", textMuted: "#7CA5A7",
            info: "#38BDF8", success: "#4ADE80", warning: "#FACC15", danger: "#FB7185",
            selection: "#0F4C4A"
        },
        "ink": {
            label: "Ink",
            accent: "#5B8CFF", accent2: "#22D3EE",
            background: "#0A0A0C", bgBottom: "#000000",
            surface: "#121216", surfaceAlt: "#1B1B21", surfaceRaised: "#23232B", surfaceDeep: "#050507",
            border: "#26262E", borderStrong: "#3A3A45",
            textPrimary: "#F4F4F6", textSecondary: "#C2C2CA", textMuted: "#85858F",
            info: "#60A5FA", success: "#22C55E", warning: "#F59E0B", danger: "#EF4444",
            selection: "#1E2F5E"
        }
    })

    readonly property var light: ({
        "paper": {
            label: "Paper",
            accent: "#4F46E5", accent2: "#DB2777",
            background: "#F7F5F0", bgBottom: "#ECE8E0",
            surface: "#FFFFFF", surfaceAlt: "#F1EEE7", surfaceRaised: "#FBFAF7", surfaceDeep: "#E7E2D8",
            border: "#DDD7CB", borderStrong: "#C5BDAE",
            textPrimary: "#1E1B16", textSecondary: "#4A463E", textMuted: "#6B665C",
            info: "#2563EB", success: "#15803D", warning: "#B45309", danger: "#DC2626",
            selection: "#C7D2FE"
        },
        "matcha": {
            label: "Matcha",
            accent: "#2F7A2C", accent2: "#CA8A04",
            background: "#F2F7EF", bgBottom: "#E4EEDF",
            surface: "#FFFFFF", surfaceAlt: "#EAF2E4", surfaceRaised: "#FAFCF8", surfaceDeep: "#DDE9D6",
            border: "#CEDCC5", borderStrong: "#AFC4A3",
            textPrimary: "#1B2A1B", textSecondary: "#3E4D3B", textMuted: "#62705F",
            info: "#0369A1", success: "#15803D", warning: "#B45309", danger: "#DC2626",
            selection: "#BBF7D0"
        },
        "lagoon": {
            label: "Lagoon",
            accent: "#0E7490", accent2: "#0284C7",
            background: "#EFF8F9", bgBottom: "#DFEEF1",
            surface: "#FFFFFF", surfaceAlt: "#E4F1F3", surfaceRaised: "#F8FCFC", surfaceDeep: "#D6E8EB",
            border: "#C7DDE1", borderStrong: "#A3C4CA",
            textPrimary: "#12292E", textSecondary: "#35494F", textMuted: "#5B6F74",
            info: "#0369A1", success: "#15803D", warning: "#B45309", danger: "#DC2626",
            selection: "#BAE6FD"
        },
        "peony": {
            label: "Peony",
            accent: "#7C3AED", accent2: "#E11D48",
            background: "#F8F4FB", bgBottom: "#EEE6F5",
            surface: "#FFFFFF", surfaceAlt: "#F2EAF8", surfaceRaised: "#FBF9FD", surfaceDeep: "#E9DFF1",
            border: "#DFD2EA", borderStrong: "#C4B1D5",
            textPrimary: "#26192F", textSecondary: "#493A55", textMuted: "#6C5C78",
            info: "#2563EB", success: "#15803D", warning: "#B45309", danger: "#DC2626",
            selection: "#DDD6FE"
        },
        "dawn": {
            label: "Dawn",
            accent: "#A54C66", accent2: "#D28A2C",
            background: "#FAF4ED", bgBottom: "#F2E9E1",
            surface: "#FFFAF3", surfaceAlt: "#F4EDE4", surfaceRaised: "#FFFDFA", surfaceDeep: "#EBE2D8",
            border: "#E2D7CB", borderStrong: "#CDBDAC",
            textPrimary: "#575279", textSecondary: "#6E6A86", textMuted: "#6F6B86",
            info: "#286983", success: "#3A7D5C", warning: "#B45309", danger: "#C7304B",
            selection: "#F2D6DD"
        }
    })

    readonly property string resolvedName: dark[name] || light[name] ? name : "nightfall"
    readonly property bool isLight: light[resolvedName] !== undefined
    readonly property var pal: isLight ? light[resolvedName] : dark[resolvedName]

    function swatches(wantLight) {
        const set = wantLight ? light : dark
        return Object.keys(set).map(n => ({ name: n, label: set[n].label }))
    }
    function palette(n) { return dark[n] ? dark[n] : light[n] }

    // Decoupled, so a custom accent works on every theme.
    readonly property string accent:       customAccent !== "" ? customAccent : pal.accent
    readonly property string accent2:      pal.accent2
    readonly property string accentLight:  isLight ? Qt.darker(accent, 1.15) : Qt.lighter(accent, 1.25)
    readonly property string textAccent:   isLight ? Qt.darker(accent, 1.25) : Qt.lighter(accent, 1.35)

    readonly property string background:    pal.background
    readonly property string bgBottom:      pal.bgBottom
    readonly property string surface:       pal.surface
    readonly property string surfaceAlt:    pal.surfaceAlt
    readonly property string surfaceRaised: pal.surfaceRaised
    readonly property string surfaceDeep:   pal.surfaceDeep
    readonly property string border:        pal.border
    readonly property string borderStrong:  pal.borderStrong

    readonly property string textPrimary:   pal.textPrimary
    readonly property string textSecondary: pal.textSecondary
    readonly property string textMuted:     pal.textMuted
    readonly property color  textDisabled:  Qt.alpha(textMuted, 0.55)

    readonly property string info:      pal.info
    readonly property string success:   pal.success
    readonly property string warning:   pal.warning
    readonly property string danger:    pal.danger
    readonly property string selection: pal.selection

    // Built from ink or light so they read on either.
    readonly property color hoverFill:   isLight ? Qt.rgba(0, 0, 0, 0.05) : Qt.rgba(1, 1, 1, 0.06)
    readonly property color pressedFill: isLight ? Qt.rgba(0, 0, 0, 0.09) : Qt.rgba(1, 1, 1, 0.11)
    readonly property color focusRing:   Qt.alpha(accent, 0.35)

    readonly property color accentSoft:   Qt.alpha(accent, 0.12)
    readonly property color accentMuted:  Qt.alpha(accent, 0.19)
    readonly property color accentStrong: Qt.alpha(accent, 0.31)
    readonly property color successSoft:  Qt.alpha(success, 0.12)
    readonly property color successMuted: Qt.alpha(success, 0.19)
    readonly property color dangerSoft:   Qt.alpha(danger, 0.14)
    readonly property color warningSoft:  Qt.alpha(warning, 0.14)
    readonly property color infoSoft:     Qt.alpha(info, 0.14)

    // In the order logger.h names them.
    function logColor(level) {
        switch (level) {
        case "error": return danger
        case "warn":  return warning
        case "ok":    return success
        case "step":  return accent2
        case "raw":   return textMuted
        default:      return info
        }
    }

    // In Library::LibraryType order.
    readonly property var libraryTypeColors: [info, accent2, warning, danger, success]
    function libraryTypeColor(type) {
        return (type >= 0 && type < libraryTypeColors.length) ? libraryTypeColors[type] : accent
    }

    // White highlights are invisible on a light surface.
    readonly property color glossHi: isLight ? "#22000000" : "#70FFFFFF"
    readonly property color glossLo: isLight ? "#10000000" : "#12FFFFFF"

    // Player chrome sits on video: dark in every palette.
    readonly property color overlayScrim:      "#E2060A14"
    readonly property color overlayScrimMid:   "#B0080C18"
    readonly property color overlayScrimSoft:  "#40080C18"
    readonly property color overlayLine:       Qt.rgba(1, 1, 1, 0.06)
    readonly property color overlayFillSoft:   Qt.rgba(1, 1, 1, 0.04)
    readonly property color overlayFill:       Qt.rgba(1, 1, 1, 0.07)
    readonly property color overlayFillHover:  Qt.rgba(1, 1, 1, 0.12)
    readonly property color overlayFillActive: Qt.rgba(1, 1, 1, 0.21)

    readonly property color onOverlay:      "#F2F4F8"
    readonly property color onOverlayMuted: "#C7CEDB"
    readonly property color onOverlayDim:   "#9AA3B5"
    readonly property color onOverlayFaint: "#5A6274"
    // A light palette's accent is too dark to read on that chrome.
    readonly property color onOverlayAccent: Qt.lighter(accent, 1.6)

    // A light palette needs far less dimming.
    readonly property color scrim: isLight ? Qt.rgba(0, 0, 0, 0.35) : Qt.rgba(0, 0, 0, 0.6)

    readonly property color ink: "#12141C"

    // WCAG relative luminance.
    function luminance(c) {
        const lin = v => v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)
        const col = Qt.color(c)
        return 0.2126 * lin(col.r) + 0.7152 * lin(col.g) + 0.0722 * lin(col.b)
    }
    // Near-black on light fills, white on dark ones.
    function inkOn(bg) { return luminance(bg) > 0.4 ? ink : "#FFFFFF" }
    readonly property color onAccent: inkOn(accent)
}
