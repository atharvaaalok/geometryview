import AppKit

/// Builds the native menu bar.
@MainActor
enum MainMenu {
    static func make() -> NSMenu {
        let bar = NSMenu()
        bar.addItem(applicationMenu())
        return bar
    }

    private static func applicationMenu() -> NSMenuItem {
        let menu = NSMenu()
        menu.addItem(
            withTitle: "About Geometry",
            action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
            keyEquivalent: "")
        menu.addItem(.separator())
        menu.addItem(
            withTitle: "Hide Geometry",
            action: #selector(NSApplication.hide(_:)),
            keyEquivalent: "h")
        menu.addItem(
            withTitle: "Quit Geometry",
            action: #selector(NSApplication.terminate(_:)),
            keyEquivalent: "q")

        let item = NSMenuItem()
        item.submenu = menu
        return item
    }
}
