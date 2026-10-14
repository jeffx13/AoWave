import QtQuick
import QtQuick.Controls

Popup {
    property alias backgroundRadius: bg.radius
    // Declared inside its opener: a press on the button toggles, not closes-then-reopens on the click.
    property bool toggledByParent: false

    padding: 0
    closePolicy: toggledByParent ? Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                                 : Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function toggle() {
        if (visible) close()
        else open()
    }

    Overlay.modal: Rectangle { color: Theme.scrim }

    enter: Transition { NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 120; easing.type: Easing.OutCubic } }
    exit:  Transition { NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 90;  easing.type: Easing.InCubic  } }

    background: Card { id: bg }
}
