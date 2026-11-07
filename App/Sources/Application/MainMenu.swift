import AppKit
import GeometryEngine

/// Builds the native menu bar.
@MainActor
enum MainMenu {
    static func make() -> NSMenu {
        let bar = NSMenu()
        bar.addItem(applicationMenu())
        bar.addItem(fileMenu())
        bar.addItem(editMenu())
        bar.addItem(viewMenu())
        return bar
    }

    private static func actionItem(
        _ title: String,
        _ action: GeoAction,
        key: String = "",
        modifiers: NSEvent.ModifierFlags = []
    ) -> NSMenuItem {
        let item = NSMenuItem(
            title: title,
            action: #selector(MainWindow.performAction(_:)),
            keyEquivalent: key
        )
        item.keyEquivalentModifierMask = modifiers
        item.tag = Int(action.rawValue)
        return item
    }

    private static func editMenu() -> NSMenuItem {
        let menu = NSMenu(title: "Edit")
        menu.addItem(actionItem("Undo", GeoActionUndo, key: "z", modifiers: .command))
        menu.addItem(actionItem("Redo", GeoActionRedo, key: "z", modifiers: [.command, .shift]))
        menu.addItem(.separator())
        for (title, mode) in [
            ("Select Faces", GeoSelectFaces), ("Select Bodies", GeoSelectBodies),
            ("Select Parts", GeoSelectParts),
        ] {
            let item = NSMenuItem(
                title: title,
                action: #selector(MainWindow.chooseSelectionMode(_:)),
                keyEquivalent: ""
            )
            item.tag = Int(mode.rawValue)
            menu.addItem(item)
        }
        menu.addItem(actionItem("Clear Selection", GeoActionClearSelection))
        let item = NSMenuItem()
        item.submenu = menu
        return item
    }

    private static func viewMenu() -> NSMenuItem {
        let menu = NSMenu(title: "View")
        menu.addItem(actionItem("Fit to View", GeoActionFit))
        menu.addItem(actionItem("Switch Projection", GeoActionToggleProjection))
        menu.addItem(.separator())
        menu.addItem(actionItem("Hide Selection", GeoActionHideSelection))
        menu.addItem(actionItem("Ghost Selection", GeoActionGhostSelection))
        menu.addItem(actionItem("Isolate Selection", GeoActionIsolate))
        menu.addItem(actionItem("Show All", GeoActionShowAll))
        menu.addItem(actionItem("Unghost All", GeoActionUnghostAll))
        let item = NSMenuItem()
        item.submenu = menu
        return item
    }

    private static func fileMenu() -> NSMenuItem {
        let menu = NSMenu(title: "File")
        menu.addItem(
            withTitle: "Open…",
            action: #selector(AppDelegate.openDocument(_:)),
            keyEquivalent: "o"
        )
        let add = NSMenuItem(
            title: "Add to View…",
            action: #selector(AppDelegate.addDocument(_:)),
            keyEquivalent: "o"
        )
        add.keyEquivalentModifierMask = [.command, .shift]
        menu.addItem(add)
        let item = NSMenuItem()
        item.submenu = menu
        return item
    }

    private static func applicationMenu() -> NSMenuItem {
        let menu = NSMenu()
        menu.addItem(
            withTitle: "About Geometry",
            action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
            keyEquivalent: ""
        )
        menu.addItem(.separator())
        menu.addItem(
            withTitle: "Hide Geometry",
            action: #selector(NSApplication.hide(_:)),
            keyEquivalent: "h"
        )
        menu.addItem(
            withTitle: "Quit Geometry",
            action: #selector(NSApplication.terminate(_:)),
            keyEquivalent: "q"
        )

        let item = NSMenuItem()
        item.submenu = menu
        return item
    }
}
