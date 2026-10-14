pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import App

AppMenu {
    id: typeMenu

    property int currentType: -1
    signal picked(int type)

    title: currentType === -1 ? qsTr("Add to library") : qsTr("Change type")

    Instantiator {
        model: Ui.libraryTypeNames
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
