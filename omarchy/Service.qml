import QtQuick
import Quickshell.Io

// hyprtitles is a Hyprland plugin, built and loaded by hyprpm. This service only makes
// sure it is: at every shell start it loads the plugin, and when it isn't built yet, or
// was built for another Hyprland or another hyprtitles version, it opens a terminal to
// build it (hyprpm needs sudo for the Hyprland headers).
Item {
  id: root

  property var shell: null

  readonly property string setupScript: decodeURIComponent(Qt.resolvedUrl("hyprtitles-setup").toString().replace(/^file:\/\//, ""))

  Process {
    id: ensure
    command: ["bash", root.setupScript, "ensure"]
    running: true
  }
}
