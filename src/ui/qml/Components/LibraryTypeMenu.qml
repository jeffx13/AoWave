pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import App
import ".."

AppMenu {
    id: typeMenu

    // -1 = not in the library yet
    property int currentType: -1
    signal picked(int type)

    title: currentType === -1 ? qsTr("Add to Library") : qsTr("Change Type")

    Instantiator {
        model: Globals.libraryTypeNames
        delegate: Action {
            required property int    index
            required property string modelData
            text: modelData
        }
        onObjectAdded: (i, obj) => {
            obj.enabled = Qt.binding(() => typeMenu.currentType !== obj.index)
            obj.triggered.connect(() => typeMenu.picked(obj.index))
            typeMenu.insertAction(i, obj)
        }
        onObjectRemoved: (i, obj) => typeMenu.removeAction(obj)
    }
}
