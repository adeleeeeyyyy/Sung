pragma Singleton
import QtQuick
QtObject {
    readonly property string fontFamily: "Google Sans Flex"
    readonly property bool followDesktop: app.theme === "system" && desktopTheme.available
    readonly property bool dark: followDesktop ? desktopTheme.dark : app.theme === "dark" || (app.theme === "system" && Application.styleHints.colorScheme === Qt.Dark)
    readonly property bool useDynamic: app.dynamicAlbumColors && app.hasAlbumColors
    readonly property var dyn: useDynamic ? (dark ? app.albumColors.dark : app.albumColors.light) : null

    readonly property color background: dyn && dyn.background ? dyn.background : (followDesktop ? desktopTheme.colors.background : (dark ? "#181211" : "#fff8f6"))
    readonly property color surface: dyn && dyn.surface ? dyn.surface : (followDesktop ? desktopTheme.colors.surface : (dark ? "#201a18" : "#fff1ec"))
    readonly property color container: dyn && dyn.container ? dyn.container : (followDesktop ? desktopTheme.colors.container : (dark ? "#2b2320" : "#f6e5de"))
    readonly property color high: dyn && dyn.high ? dyn.high : (followDesktop ? desktopTheme.colors.high : (dark ? "#382c28" : "#efddd5"))
    readonly property color text: dyn && dyn.text ? dyn.text : (followDesktop ? desktopTheme.colors.text : (dark ? "#f5ded5" : "#281912"))
    readonly property color muted: dyn && dyn.muted ? dyn.muted : (followDesktop ? desktopTheme.colors.muted : (dark ? "#d5bfb5" : "#705c53"))
    readonly property color outline: dyn && dyn.outline ? dyn.outline : (followDesktop ? desktopTheme.colors.outline : (dark ? "#57443b" : "#dcc5b9"))
    readonly property color primary: dyn && dyn.primary ? dyn.primary : (followDesktop ? desktopTheme.colors.primary : (dark ? "#ffb596" : "#964829"))
    readonly property color primaryText: dyn && dyn.primaryText ? dyn.primaryText : (followDesktop ? desktopTheme.colors.primaryText : (dark ? "#572008" : "#ffffff"))
    readonly property color primaryContainer: dyn && dyn.primaryContainer ? dyn.primaryContainer : (followDesktop ? desktopTheme.colors.primaryContainer : (dark ? "#75351b" : "#ffdbcb"))
    readonly property color containerText: dyn && dyn.containerText ? dyn.containerText : (followDesktop ? desktopTheme.colors.containerText : (dark ? "#ffdbcb" : "#743419"))
    readonly property color secondary: dyn && dyn.secondary ? dyn.secondary : (followDesktop ? desktopTheme.colors.secondary : (dark ? "#d8c4a0" : "#6c5b3b"))
    readonly property int fast: app.motion ? 150 : 0
    readonly property int normal: app.motion ? 200 : 0
    readonly property int slow: app.motion ? 500 : 0
    // M3 published spring-to-curve conversions for non-gesture transitions.
    readonly property var fastSpatialCurve: [0.42,1.67,0.21,0.90,1,1]
    readonly property var effectsCurve: [0.34,0.80,0.34,1,1,1]
    readonly property var fastEffectsCurve: [0.31,0.94,0.34,1,1,1]
    readonly property var curve: [0.27,1.06,0.18,1,1,1]
}
