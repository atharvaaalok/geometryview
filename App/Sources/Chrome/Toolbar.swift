import AppKit
import GeometryEngine

/// The bottom toolbar: what clicks pick, what to do with a selection, history, projection and
/// framing. Every control here also has a key.
@MainActor
final class Toolbar: StickerCard {
    let pick = SegmentedPill(
        ["Face", "Body", "Part"],
        selected: 0,
        help: "What a click picks (1, 2, 3)"
    )
    let projection = SegmentedPill(["Ortho", "Persp"], selected: 0, help: "Projection (P)")
    private let hide = PillButton(
        symbol: "eye.slash",
        help: "Hide the selection (hold H and click)"
    )
    private let ghost = PillButton(
        symbol: "circle.dashed",
        help: "Ghost the selection (hold G and click)"
    )
    private let isolate = PillButton(title: "Isolate", help: "Hide everything else (I)")
    private let showAll = PillButton(symbol: "eye", help: "Show everything (⇧H)")
    private let undo = PillButton(symbol: "arrow.uturn.backward", help: "Undo (⌘Z)")
    private let redo = PillButton(symbol: "arrow.uturn.forward", help: "Redo (⇧⌘Z)")
    private let fit = PillButton(
        symbol: "arrow.up.left.and.arrow.down.right",
        help: "Fit to view (F)"
    )
    private let views = PillButton(symbol: "cube", help: "Standard views")
    private let look = PillButton(symbol: "paintpalette", help: "Style and colour")
    private let cut = PillButton(symbol: "scissors", help: "Section (S)")
    private let seeThrough = PillButton(
        symbol: "circle.lefthalf.filled",
        help: "See through the housing (T)"
    )
    private let shortcuts = PillButton(title: "?", style: .ghostly, help: "Shortcuts (?)")

    var onAction: ((GeoAction) -> Void)?
    var onShowShortcuts: (() -> Void)?
    var onShowLook: (() -> Void)?
    var onShowSection: (() -> Void)?

    init() {
        super.init(frame: .zero)
        let actions: [(PillButton, GeoAction)] = [
            (hide, GeoActionHideSelection), (ghost, GeoActionGhostSelection),
            (isolate, GeoActionIsolate), (showAll, GeoActionShowAll), (undo, GeoActionUndo),
            (redo, GeoActionRedo), (fit, GeoActionFit), (seeThrough, GeoActionToggleSeeThrough),
        ]
        for (button, action) in actions {
            button.onPress = { [weak self] in self?.onAction?(action) }
        }
        views.onPress = { [weak self] in self?.showViewsMenu() }
        shortcuts.onPress = { [weak self] in self?.onShowShortcuts?() }
        look.onPress = { [weak self] in self?.onShowLook?() }
        cut.onPress = { [weak self] in self?.onShowSection?() }

        let stack = NSStackView(views: [
            pick, rule(), hide, ghost, isolate, showAll, rule(), undo, redo, rule(), seeThrough,
            cut, look,
            projection, views, fit, shortcuts,
        ])
        stack.orientation = .horizontal
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -8),
        ])
    }

    private func rule() -> NSView {
        let view = NSView()
        view.wantsLayer = true
        view.layer?.backgroundColor = Palette.rule.cgColor
        view.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            view.widthAnchor.constraint(equalToConstant: Metrics.line),
            view.heightAnchor.constraint(equalToConstant: 22),
        ])
        return view
    }

    private func showViewsMenu() {
        let menu = NSMenu()
        let entries: [(String, GeoAction)] = [
            ("Front", GeoActionViewFront), ("Back", GeoActionViewBack), ("Left", GeoActionViewLeft),
            ("Right", GeoActionViewRight), ("Top", GeoActionViewTop),
            ("Bottom", GeoActionViewBottom),
            ("Isometric", GeoActionViewIso),
        ]
        for (title, action) in entries {
            let item = NSMenuItem(
                title: title,
                action: #selector(chooseView(_:)),
                keyEquivalent: ""
            )
            item.target = self
            item.tag = Int(action.rawValue)
            menu.addItem(item)
        }
        menu.popUp(positioning: nil, at: NSPoint(x: views.frame.minX, y: bounds.maxY + 6), in: self)
    }

    @objc private func chooseView(_ sender: NSMenuItem) {
        onAction?(GeoAction(rawValue: UInt32(sender.tag)))
    }

    /// Reflects what the engine can do now.
    func update(hasSelection: Bool, hasModel: Bool) {
        for button in [hide, ghost, isolate] {
            button.isEnabled = hasSelection
        }
        for button in [showAll, fit, views, undo, redo, seeThrough] {
            button.isEnabled = hasModel
        }
    }
}
