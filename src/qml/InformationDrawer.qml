import QtQuick 2.14
import QtQuick.Controls 2.14

import org.qfield 1.0
import Theme 1.0

Drawer {
  id: controller
  width: parent.width
  height: mainContent.height
  Overlay.modal: null
  modal: false
  edge: Qt.BottomEdge
  dragMargin: 0
  closePolicy: Popup.NoAutoClose

  property real realtimeHeight: controller.height * controller.position
  property bool closeRequested: true
  property bool openRequested: false
  property real itemRadius: 8
  property bool uiConflictFree: false

  // SensorInformationView
  property bool sensorInformationViewEnabled: sensorInformationView.activeSensors > 0

  // NavigationInformationView
  property bool navigationInformationViewEnabled: navigation.isActive

  // PositioningInformationView
  property Navigation navigation
  property bool positioningInformationViewEnabled: positioningSettings.showPositionInformation && uiConflictFree

  // PositioningPreciseView
  property alias positioningPreciseView: positioningPreciseView
  property PositioningSettings positioningSettings
  property Positioning positionSource
  property real positioningPreciseViewHeight
  property bool positioningPreciseEnabled: uiConflictFree
                                           && !isNaN(navigation.distance)
                                           && navigation.isActive
                                           && (positioningSettings.alwaysShowPreciseView
                                              || ( positioningPreciseView.hasAcceptableAccuracy
                                              &&  positioningPreciseView.projectDistance < positioningPreciseView.precision ))


  function resetHeight() {
    let newHeight = 0
    newHeight += (navigationInformationView.height + 8)
    newHeight += (positioningInformationView.height + 8)
    newHeight += (positioningPreciseView.height + 8)
    newHeight += (sensorInformationView.height + 8)
    newHeight += 16
    controller.height = newHeight
  }

  function updateDrawerHeight(visibleView, viewHeight){
    if (!visibleView) {
      controller.height -= (viewHeight + 8)
      controller.open()
    } else if (openRequested) {
      controller.height += (viewHeight + 8)
      controller.open()
    }
  }

  onNavigationInformationViewEnabledChanged: updateDrawerHeight(navigationInformationViewEnabled, navigationInformationView.contentHeight)

  onPositioningInformationViewEnabledChanged: updateDrawerHeight(positioningInformationViewEnabled, positioningInformationView.contentHeight)

  onPositioningPreciseEnabledChanged: updateDrawerHeight(positioningPreciseEnabled, positioningPreciseViewHeight)

  onSensorInformationViewEnabledChanged: updateDrawerHeight(sensorInformationViewEnabled, sensorInformationView.contentHeight)

  onOpenRequestedChanged: {
    if (openRequested) {
      closeRequested = false
      resetHeight()
      open()
    } else {
      closeRequested = true
      close()
    }
  }

  onOpened: {
    if (closeRequested) {
      controller.close()
    }
  }

  onClosed: {
    if (!closeRequested) {
      // view is dragged down but we must bring it back
      controller.open()
    }
  }

  background: Item {
    anchors.fill: parent
  }

  Column {
    id: mainContent
    width: parent.width - 8
    anchors.horizontalCenter: parent.horizontalCenter
    spacing: 8

    NavigationInformationView {
      id: navigationInformationView
      width: parent.width
      height: navigationInformationViewEnabled ? contentHeight : 0
      clip: true
      navigation: controller.navigation
      radius: itemRadius
    }

    PositioningInformationView {
      id: positioningInformationView
      width: parent.width
      height: positioningInformationViewEnabled ? contentHeight : 0
      clip: true
      visible: positioningInformationViewEnabled
      positionSource: controller.positionSource
      antennaHeight: positioningSettings.antennaHeightActivated ? positioningSettings.antennaHeight : NaN
      radius: itemRadius
    }

    PositioningPreciseView {
      id: positioningPreciseView
      width: parent.width
      height: positioningPreciseEnabled ? positioningPreciseViewHeight : 0
      clip: true
      precision: positioningSettings.preciseViewPrecision
    }

    SensorInformationView {
      id: sensorInformationView
      height: sensorInformationViewEnabled ? contentHeight : 0
    }
  }
}
