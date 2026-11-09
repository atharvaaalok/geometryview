import AppKit
import GeometryViewEngine

/// The bottom toolbar: what clicks pick, what to do with a selection, history, projection and
/// framing. Every control here also has a key.
@MainActor
final class Toolbar: StickerCard {
    let pick = SegmentedPill(
        ["Face", "Body", "Part"],
        selected: 0,
        help: "What a click selects (1, 2, 3)",
        tips: [
            "Clicks select single faces (1)",
            "Clicks select whole bodies (2)",
            "Clicks select whole parts (3)",
        ]
    )
    let projection = SegmentedPill(["Ortho", "Persp"], selected: 0, help: "Projection (P)")
    private let hide = PillButton(
        icon: .eyeSlash,
        help: "Hide the selection (hold H and click)"
    )
    private let ghost = PillButton(
        icon: .ghost,
        help: "Ghost the selection (hold G and click)"
    )
    private let isolate = PillButton(title: "Isolate", help: "Hide everything else (I)")
    private let showAll = PillButton(icon: .eye, help: "Show everything (⇧H)")
    private let undo = PillButton(icon: .undo, help: "Undo (⌘Z)")
    private let redo = PillButton(icon: .redo, help: "Redo (⇧⌘Z)")
    private let fit = PillButton(
        icon: .fit,
        help: "Fit to view (F)"
    )
    private let views = PillButton(icon: .cube, help: "Standard views")
    private let look = PillButton(icon: .palette, help: "Style and colour")
    private let cut = PillButton(icon: .scissors, help: "Section (S)")
    private let seeThrough = PillButton(
        icon: .seeThrough,
        help: "See through the housing (T)"
    )
    private let shortcuts = PillButton(title: "?", style: .ghostly, help: "Shortcuts (?)")
    // The two ways in, as on the first page and in the Library.
    private let openFiles = PillButton(icon: .plus, help: "Open files (⌘O)")
    private let connectFolder = PillButton(icon: .folderPlus, help: "Connect a folder (⌥⌘O)")
    var onOpen: (() -> Void)?
    var onConnect: (() -> Void)?

    var onAction: ((GeoAction) -> Void)?
    var onShowShortcuts: (() -> Void)?
    var onShowLook: (() -> Void)?
    var onShowSection: (() -> Void)?

    init() {
        super.init()
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

        openFiles.onPress = { [weak self] in self?.onOpen?() }
        connectFolder.onPress = { [weak self] in self?.onConnect?() }
        let stack = NSStackView(views: [
            openFiles, connectFolder, rule(), pick, rule(), hide, ghost, isolate, showAll, rule(), undo, redo, rule(), seeThrough,
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

    /// The standard views, in a menu above their button, which the window places over everything.
    lazy var viewsMenu = MenuCard(items: [
        ("Front", GeoActionViewFront), ("Back", GeoActionViewBack), ("Left", GeoActionViewLeft),
        ("Right", GeoActionViewRight), ("Top", GeoActionViewTop), ("Bottom", GeoActionViewBottom),
        ("Isometric", GeoActionViewIso),
    ].map { title, action in (title, { [weak self] in self?.onAction?(action) }) })

    func showViewsMenu() {
        viewsMenu.toggle(above: views)
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
