import AppKit

/// Writes where the chrome is, for comparing with the web viewer's chrome, which
/// Tools/web_chrome.mjs measures under the same names.
///
/// Launching with `--chrome-layout` writes `chrome-layout.json` to the app's temporary directory
/// once the view has settled, after the file's scene is ready or, with no file, the empty window,
/// and quits. Each name maps to a box, or a list of boxes from the top, in points from the window's
/// top left: {x, y, w, h}. Tools/compare_chrome.py compares the two.
@MainActor
enum ChromeLayout {
    static var isRequested: Bool { CommandLine.arguments.contains("--chrome-layout") }

    /// Names a view for the report.
    static func name(_ view: NSView, _ name: String) {
        view.identifier = NSUserInterfaceItemIdentifier("layout:\(name)")
    }

    static func write(_ window: NSWindow, after delay: Double = 1.0) {
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
            guard let root = window.contentView else {
                return
            }
            var boxes: [String: [[String: Double]]] = [:]
            collect(root, root: root, into: &boxes)
            var report: [String: Any] = [:]
            for (name, list) in boxes {
                // In reading order: a row's items, such as the toolbar's, may differ in top by a few points.
                let sorted = list.sorted { (($0["y"]! / 10).rounded(), $0["x"]!) < (($1["y"]! / 10).rounded(), $1["x"]!) }
                report[name] = listed.contains(name) ? Array(sorted.prefix(8)) : sorted.first
            }
            let output = FileManager.default.temporaryDirectory
                .appendingPathComponent("chrome-layout.json")
            if let data = try? JSONSerialization.data(
                withJSONObject: report,
                options: [.prettyPrinted, .sortedKeys]
            ) {
                try? data.write(to: output)
                print("Wrote \(output.path)")
            }
            NSApp.terminate(nil)
        }
    }

    /// The names reported as lists, as the web tool reports them.
    private static let listed: Set<String> = [
        "structure.segment", "tree.row", "tree.chevron", "tree.glyph", "tree.swatch", "tree.name",
        "tree.count", "toolbar.item", "status.button", "library.button",
    ]

    private static func collect(_ view: NSView, root: NSView, into boxes: inout [String: [[String: Double]]]) {
        guard !view.isHidden, view.alphaValue > 0 else {
            return
        }
        if let name = reportedName(view) {
            var frame = view.convert(view.bounds, to: root)
            // A label's frame pads its text by 2 on each side; the page's boxes hug it.
            if view is NSTextField {
                frame = frame.insetBy(dx: 2, dy: 0)
            }
            let top = root.isFlipped ? frame.minY : root.bounds.height - frame.maxY
            boxes[name, default: []].append([
                "x": frame.minX, "y": top, "w": frame.width, "h": frame.height,
            ])
        }
        for subview in view.subviews {
            collect(subview, root: root, into: &boxes)
        }
    }

    private static func reportedName(_ view: NSView) -> String? {
        if let id = view.identifier?.rawValue, id.hasPrefix("layout:") {
            let name = String(id.dropFirst("layout:".count))
            // Only the structure card's switcher's segments: the toolbar has segments too.
            if name == "structure.segment", !view.ancestor(StructureCard.self) {
                return nil
            }
            return name
        }
        switch view {
        case is StructureCard: return "structure.card"
        case is LibraryCard: return "library.card"
        case is TreeRowView: return "tree.row"
        case is Toolbar: return "toolbar.card"
        case is StatusCard: return "status.card"
        default: break
        }
        if view is SegmentedPill, view.superview is StructureCard {
            return "structure.switcher"
        }
        if view is SegmentedPill, view.superview is LibraryCard {
            return "library.switcher"
        }
        if view is PillButton, view.superview is LibraryCard {
            return "library.button"
        }
        if view is NSSearchField, view.superview is StructureCard {
            return "structure.search"
        }
        if view.superview is NSStackView, view.superview?.superview is Toolbar {
            return "toolbar.item"
        }
        if view is PillButton, view.ancestor(StatusCard.self) {
            return "status.button"
        }
        return nil
    }
}

extension NSView {
    fileprivate func ancestor<T: NSView>(_ type: T.Type) -> Bool {
        var view = superview
        while let current = view {
            if current is T {
                return true
            }
            view = current.superview
        }
        return false
    }
}
