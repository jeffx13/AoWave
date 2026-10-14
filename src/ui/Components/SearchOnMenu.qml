pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import App

AppMenu {
    id: searchOnMenu

    property string showTitle: ""
    property string ownProvider: ""

    title: qsTr("Search on")
    iconName: "search"

    function searchOn(index) {
        const query = searchOnMenu.showTitle
        App.providers.currentIndex = index
        Qt.callLater(() => Ui.searchProviders(query))
    }

    Instantiator {
        model: App.providers
        delegate: Action {
            id: providerAction
            required property int index
            required property var model
            property bool shown: providerAction.model.text !== searchOnMenu.ownProvider
            text: providerAction.model.text
        }
        onObjectAdded: (i, obj) => {
            obj.triggered.connect(() => searchOnMenu.searchOn(obj.index))
            searchOnMenu.insertAction(i, obj)
        }
        onObjectRemoved: (i, obj) => searchOnMenu.removeAction(obj)
    }
}
