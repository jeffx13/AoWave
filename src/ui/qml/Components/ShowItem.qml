import QtQuick
import ".."

Item {
    id: showItem

    property alias showTitle: titleText.text
    property alias showCover: coverImage.source
    property alias image: imageClip
    property real  aspectRatio: 319 / 225
    property int   libraryType: -1
    property string badgeText: ""

    signal imageClicked(var mouse)
    signal imageLoaded(real sourceAspectRatio)
    signal playClicked()
    signal addClicked()

    property bool showAddAction: true

    // Judged where the card rests, not where it is drawn: the lift moves the card and its mouse
    // areas up, so its bottom edge used to hover, lift away from the pointer, drop, and repeat.
    // On the root, as an ancestor: a sibling under the cover never hears of hover the cover's
    // MouseArea has taken, so only the strip under the picture used to count.
    readonly property bool isHovered: restingHover.hovered
    HoverHandler { id: restingHover; cursorShape: Qt.PointingHandCursor }

    Rectangle {
        anchors { fill: parent; margins: 5; topMargin: 10; bottomMargin: 1 }
        radius: 10
        color: Theme.isLight ? Qt.alpha("black", 0.10) : Qt.alpha("black", 0.32)
        opacity: showItem.isHovered ? 1 : 0.35
        Behavior on opacity { NumberAnimation { duration: 180 } }
    }
    Rectangle {
        anchors { fill: parent; margins: 5 }
        color: showItem.isHovered ? Theme.surfaceRaised : Theme.surface
        radius: 9
        transform: Translate {
            y: showItem.isHovered ? -3 : 0
            Behavior on y { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
        }
        Behavior on color { ColorAnimation { duration: 180 } }

        Rectangle {
            id: imageClip
            anchors {
                top: parent.top
                left: parent.left
                right: parent.right
                margins: 2
            }
            height: width * showItem.aspectRatio
            radius: 8
            clip: true
            color: Theme.surface

            Image {
                id: coverImage
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                cache: true
                asynchronous: true
                // Fixed decode size: resizing scales the cached texture instead of re-decoding.
                sourceSize: Qt.size(360, Math.round(360 * showItem.aspectRatio))
                scale: showItem.isHovered ? 1.025 : 1.0
                opacity: status === Image.Ready ? 1.0 : 0.0
                Behavior on scale {
                    NumberAnimation { duration: 350; easing.type: Easing.OutCubic }
                }
                Behavior on opacity {
                    NumberAnimation { duration: 320; easing.type: Easing.OutCubic }
                }
                // Reassigning source would kill the binding.
                onStatusChanged: {
                    if (status === Image.Ready && sourceSize.width > 0 && sourceSize.height > 0)
                        showItem.imageLoaded(sourceSize.height / sourceSize.width)
                }
            }

            Image {
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                visible: coverImage.status === Image.Error
                source: visible ? "qrc:/App/resources/images/error_image.png" : ""
            }

            Rectangle {
                id: skeleton
                anchors.fill: parent
                visible: coverImage.status === Image.Loading || coverImage.status === Image.Null
                color: Theme.surfaceAlt
                clip: true

                Rectangle {
                    width: skeleton.width * 0.6
                    height: skeleton.height
                    rotation: 12
                    transformOrigin: Item.Center
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: "transparent" }
                        GradientStop { position: 0.5; color: Qt.alpha(Theme.textPrimary, 0.10) }
                        GradientStop { position: 1.0; color: "transparent" }
                    }
                    NumberAnimation on x {
                        from: -skeleton.width * 0.6
                        to: skeleton.width
                        duration: 1150
                        loops: Animation.Infinite
                        running: skeleton.visible && showItem.visible && !Theme.reduceMotion
                    }
                }
            }

            Rectangle {
                anchors.fill: parent
                radius: parent.radius
                opacity: showItem.isHovered ? 1.0 : 0.0
                Behavior on opacity {
                    NumberAnimation { duration: 300; easing.type: Easing.OutCubic }
                }
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop { position: 0.6; color: "transparent" }
                    GradientStop { position: 1.0; color: "#C0000000" }
                }
            }

            Rectangle {
                width: parent.width * 0.7
                height: parent.height * 0.4
                radius: parent.radius
                anchors {
                    top: parent.top
                    left: parent.left
                }
                opacity: showItem.isHovered ? 0.08 : 0.0
                Behavior on opacity {
                    NumberAnimation { duration: 400; easing.type: Easing.OutCubic }
                }
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#ffffff" }
                    GradientStop { position: 1.0; color: "transparent" }
                }
            }

            Rectangle {
                anchors {
                    bottom: parent.bottom
                    left: parent.left
                    right: parent.right
                    leftMargin: parent.width * 0.15
                    rightMargin: parent.width * 0.15
                }
                height: 2
                radius: 1
                color: Theme.accent
                opacity: showItem.isHovered ? 0.8 : 0.0
                Behavior on opacity {
                    NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                }
            }

            Rectangle {
                visible: showItem.libraryType >= 0
                anchors {
                    top: parent.top
                    right: parent.right
                    topMargin: 3
                    rightMargin: 3
                }
                width: libraryBadge.width + 14
                height: libraryBadge.height + 10
                radius: libraryBadge.radius + 5
                color: libraryBadge.typeColor
                opacity: showItem.isHovered ? 0.38 : 0.20
                Behavior on opacity { NumberAnimation { duration: 250 } }
            }

            Rectangle {
                id: libraryBadge
                visible: showItem.libraryType >= 0

                readonly property color typeColor: Theme.libraryTypeColor(showItem.libraryType)

                anchors {
                    top: parent.top
                    right: parent.right
                    topMargin: 8
                    rightMargin: 8
                }

                implicitWidth: badgeRow.implicitWidth + 18
                height: 26
                radius: 13
                color: Theme.surface
                border.color: typeColor
                border.width: 1

                scale: showItem.isHovered ? 1.08 : 1.0
                Behavior on scale { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }

                Row {
                    id: badgeRow
                    anchors.centerIn: parent
                    spacing: 5

                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: libraryBadge.typeColor
                        anchors.verticalCenter: parent.verticalCenter

                        Rectangle {
                            anchors.centerIn: parent
                            width: 4
                            height: 4
                            radius: 2
                            color: "white"
                            opacity: 0.55
                        }
                    }

                    Text {
                        text: showItem.libraryType >= 0 ? Globals.libraryTypeNames[showItem.libraryType] : ""
                        color: Theme.textPrimary
                        font.pixelSize: Globals.sp(Theme.bodySize)
                        font.weight: Font.Medium
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }

            Rectangle {
                visible: showItem.badgeText.length > 0 && opacity > 0.01
                // Yields to the add button, which shares this corner.
                opacity: showItem.showAddAction && showItem.isHovered ? 0.0 : 1.0
                Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                anchors {
                    bottom: parent.bottom
                    left: parent.left
                    bottomMargin: 8
                    leftMargin: 8
                }
                implicitWidth: badgeLabel.implicitWidth + 14
                height: 22
                radius: 11
                color: Theme.surface
                border.color: Theme.accent
                border.width: 1

                Text {
                    id: badgeLabel
                    anchors.centerIn: parent
                    text: showItem.badgeText
                    color: Theme.textPrimary
                    font.pixelSize: Globals.sp(18)
                    font.weight: Font.Medium
                }
            }

            MouseArea {
                id: imageArea
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                cursorShape: Qt.PointingHandCursor
                onClicked: (mouse) => showItem.imageClicked(mouse)
            }

            Item {
                id: quickActions
                anchors.fill: parent
                property bool hovered: playBtnArea.containsMouse || addBtnArea.containsMouse
                opacity: showItem.isHovered ? 1.0 : 0.0
                visible: opacity > 0.01
                Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

                Rectangle {
                    anchors { right: parent.right; bottom: parent.bottom; rightMargin: 8; bottomMargin: 8 }
                    width: 40
                    height: 40
                    radius: 20
                    color: playBtnArea.containsMouse ? Theme.accentLight : Theme.accent
                    scale: showItem.isHovered ? 1.0 : 0.7
                    Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutBack } }

                    AppIcon {
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: 1
                        name: "play"
                        size: 18
                        color: Theme.onAccent
                    }

                    MouseArea {
                        id: playBtnArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: showItem.playClicked()
                    }
                    AppToolTip { visible: playBtnArea.containsMouse; text: qsTr("Play") }
                }

                Rectangle {
                    visible: showItem.showAddAction
                    anchors { left: parent.left; bottom: parent.bottom; leftMargin: 8; bottomMargin: 8 }
                    width: 32
                    height: 32
                    radius: 16
                    color: addBtnArea.containsMouse ? Theme.accent : Theme.surface
                    border.color: Theme.accent
                    border.width: 1
                    scale: showItem.isHovered ? 1.0 : 0.7
                    Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutBack } }

                    AppIcon {
                        anchors.centerIn: parent
                        name: "plus"
                        size: 18
                        color: addBtnArea.containsMouse ? Theme.onAccent : Theme.textPrimary
                    }

                    MouseArea {
                        id: addBtnArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: showItem.addClicked()
                    }
                    AppToolTip {
                        visible: addBtnArea.containsMouse
                        text: showItem.libraryType >= 0 ? qsTr("Change Type") : qsTr("Add to Library")
                    }
                }
            }
        }

        Text {
            id: titleText
            anchors {
                top: imageClip.bottom
                left: parent.left
                right: parent.right
                bottom: parent.bottom
                topMargin: 6
                leftMargin: 6
                rightMargin: 6
                bottomMargin: 4
            }
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignTop
            font.pixelSize: Globals.sp(Theme.bodySize)
            color: showItem.isHovered ? Theme.textPrimary : Theme.textSecondary
            Behavior on color {
                ColorAnimation { duration: 200 }
            }
            transform: Translate {
                y: showItem.isHovered ? -2 : 0
                Behavior on y {
                    NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                }
            }
            clip: true

            MouseArea {
                id: titleArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                onClicked: (mouse) => showItem.imageClicked(mouse)
            }

            AppToolTip {
                text: titleText.text
                visible: titleArea.containsMouse && titleText.truncated
            }
        }
    }
}