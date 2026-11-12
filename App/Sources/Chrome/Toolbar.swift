import AppKit
import GeometryViewEngine

/// The bottom toolbar: what clicks pick, what to do with a selection, history, projection and
/// framing. Every control here also has a key. When the window is too narrow for all of it, the
/// least used tools fold into a More menu, by the rule the page follows (Web/viewer/toolbar-fold.js).
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
    private let picture = PillButton(icon: .camera, help: "Copy or save a picture of the view")
    var onCopyPicture: (() -> Void)?
    var onSavePicture: (() -> Void)?
    // The two ways in, as on the first page and in the Library.
    private let openFiles = PillButton(icon: .plus, help: "Open files (⌘O)")
    private let connectFolder = PillButton(icon: .folderPlus, help: "Connect a folder (⌥⌘O)")
    /// Brings the sidebar back, shown only while it is put away.
    private let showSidebar = PillButton(icon: .sidebar, help: "Show the sidebar (⌃⌘S)")
    private var sidebarShown = true
    var onShowSidebar: (() -> Void)?
    var onOpen: (() -> Void)?
    var onConnect: (() -> Void)?

    var onAction: ((GeoAction) -> Void)?
    var onShowShortcuts: (() -> Void)?
    var onShowLook: (() -> Void)?
    var onShowSection: (() -> Void)?
    /// Called with the projection chosen from the More menu: true for perspective.
    var onProjection: ((Bool) -> Void)?

    private let more = PillButton(icon: .more, help: "More tools")
    /// The tools, by name, in order, each in a group; groups are divided by rules.
    private var tools: [(key: String, view: NSView, group: Int)] = []
    private var rules: [NSView] = []
    private var widths: [String: CGFloat] = [:]
    private var folded: Set<String> = []
    private var fittedTo: CGFloat = -1
    /// What folds, in toolbar order, for the More menu.
    lazy var moreMenu = MenuCard(iconColumn: true)
    /// The tools showing, by name, in order: for the layout check.
    var shownTools: [String] {
        tools.filter { !$0.view.isHidden }.map(\.key) + (more.isHidden ? [] : ["more"])
    }

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
        picture.onPress = { [weak self] in self?.showPictureMenu() }

        openFiles.onPress = { [weak self] in self?.onOpen?() }
        connectFolder.onPress = { [weak self] in self?.onConnect?() }
        showSidebar.onPress = { [weak self] in self?.onShowSidebar?() }
        more.onPress = { [weak self] in self?.toggleMore() }
        more.isHidden = true
        let groups: [[(String, NSView)]] = [
            [("parts", showSidebar), ("open", openFiles), ("connect", connectFolder)],
            [("pick", pick)],
            [("hide", hide), ("ghost", ghost), ("isolate", isolate), ("showAll", showAll)],
            [("undo", undo), ("redo", redo)],
            [("seeThrough", seeThrough), ("section", cut), ("look", look), ("projection", projection),
             ("views", views), ("fit", fit), ("picture", picture), ("shortcuts", shortcuts)],
        ]
        var views: [NSView] = []
        for (group, members) in groups.enumerated() {
            if group > 0 {
                let divider = rule()
                rules.append(divider)
                views.append(divider)
            }
            for (key, view) in members {
                tools.append((key, view, group))
                views.append(view)
            }
        }
        views.append(more)
        let stack = NSStackView(views: views)
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

    /// Copy Picture and Save Picture, in a menu above the camera pill.
    lazy var pictureMenu = MenuCard(items: [
        ("Copy Picture", { [weak self] in self?.onCopyPicture?() }),
        ("Save Picture\u{2026}", { [weak self] in self?.onSavePicture?() }),
    ])

    func showPictureMenu() {
        pictureMenu.toggle(above: picture)
    }

    // MARK: Folding

    /// Folds the least used tools into More until the toolbar fits `available` points: the
    /// window's width less the cards' room, as the page leaves it. Measures each tool once.
    func fit(available: CGFloat) {
        guard abs(available - fittedTo) >= 0.5 else {
            return
        }
        fittedTo = available
        if widths.isEmpty {
            for tool in tools {
                widths[tool.key] = tool.view.fittingSize.width
            }
        }
        // The sidebar's pill takes no room, and shows not at all, while the sidebar is out.
        let absent = { (key: String) in key == "parts" && self.sidebarShown }
        let items = tools.map {
            FoldItem(key: $0.key, width: absent($0.key) ? 0 : widths[$0.key] ?? 0, group: $0.group)
        }
        folded = Self.foldPlan(items, available: available)
        for tool in tools {
            tool.view.isHidden = folded.contains(tool.key) || absent(tool.key)
        }
        for (group, divider) in rules.enumerated() {
            divider.isHidden = !Self.ruleShows(items, folded: folded, group: group)
        }
        more.isHidden = folded.isEmpty
        if folded.isEmpty {
            moreMenu.close()
        }
    }

    /// The sidebar out, or put away: its pill shows only while it is away.
    func setSidebarShown(_ shown: Bool) {
        sidebarShown = shown
        let room = fittedTo
        fittedTo = -1
        fit(available: room)
    }

    struct FoldItem {
        let key: String
        let width: CGFloat
        let group: Int
    }

    /// The order tools fold in, as kFoldRanks in toolbar-fold.js: lower first, a rank at a time.
    static let foldRanks: [String: Int] = [
        "shortcuts": 0, "projection": 1, "views": 2, "picture": 2, "fit": 3, "open": 4,
        "connect": 4, "look": 5,
        "section": 6, "seeThrough": 7, "isolate": 8, "ghost": 9, "hide": 9, "undo": 10, "redo": 10,
        "showAll": 11,
    ]
    // As kToolbarMeasures: the inset each side, the gap, a rule's width and More's.
    private static let inset: CGFloat = 10, gap: CGFloat = 8, ruleWidth = Metrics.line, moreWidth: CGFloat = 30

    static func foldPlan(_ items: [FoldItem], available: CGFloat) -> Set<String> {
        let ranks = Set(items.filter { $0.width > 0 }.compactMap { foldRanks[$0.key] }).sorted()
        var folded: Set<String> = []
        for n in 0...ranks.count {
            folded = n == 0 ? [] : Set(items.filter { (foldRanks[$0.key] ?? .max) <= ranks[n - 1] }.map(\.key))
            if width(items, folded: folded) <= available {
                break
            }
        }
        return folded
    }

    static func width(_ items: [FoldItem], folded: Set<String>) -> CGFloat {
        var shown = items.filter { $0.width > 0 && !folded.contains($0.key) }
        if !folded.isEmpty {
            shown.append(FoldItem(key: "more", width: moreWidth, group: items.last?.group ?? 0))
        }
        let groups = Set(shown.map(\.group)).count
        let rules = max(0, groups - 1)
        let count = shown.count + rules
        return 2 * inset + shown.reduce(0) { $0 + $1.width } + CGFloat(rules) * ruleWidth
            + CGFloat(max(0, count - 1)) * gap
    }

    static func ruleShows(_ items: [FoldItem], folded: Set<String>, group: Int) -> Bool {
        let last = items.last?.group ?? 0
        let showing = { (g: Int) in
            items.contains { $0.group == g && $0.width > 0 && !folded.contains($0.key) }
                || (!folded.isEmpty && g == last)
        }
        let groups = Array(Set(items.map(\.group))).sorted()
        guard let at = groups.firstIndex(of: group) else {
            return false
        }
        return showing(group) && groups[(at + 1)...].contains(where: showing)
    }

    /// The folded tools, as rows in toolbar order: the views under a heading of their own.
    func toggleMore() {
        if moreMenu.isOpen {
            moreMenu.close()
            return
        }
        let act = { [weak self] (action: GeoAction) -> () -> Void in { self?.onAction?(action) } }
        let row = { (key: String, icon: Icons.Name?, title: String, action: @escaping () -> Void) -> MenuItem in
            let enabled = (self.tools.first { $0.key == key }?.view as? ChromeControl)?.isEnabled ?? true
            return .row(title: title, icon: icon, checked: false, enabled: enabled, action: action)
        }
        var items: [MenuItem] = []
        for tool in tools where folded.contains(tool.key) {
            switch tool.key {
            case "open": items.append(row("open", .plus, "Open files") { [weak self] in self?.onOpen?() })
            case "connect":
                items.append(row("connect", .folderPlus, "Connect a folder") { [weak self] in self?.onConnect?() })
            case "hide": items.append(row("hide", .eyeSlash, "Hide the selection", act(GeoActionHideSelection)))
            case "ghost": items.append(row("ghost", .ghost, "Ghost the selection", act(GeoActionGhostSelection)))
            case "isolate": items.append(row("isolate", .scope, "Isolate", act(GeoActionIsolate)))
            case "showAll": items.append(row("showAll", .eye, "Show everything", act(GeoActionShowAll)))
            case "undo": items.append(row("undo", .undo, "Undo", act(GeoActionUndo)))
            case "redo": items.append(row("redo", .redo, "Redo", act(GeoActionRedo)))
            case "seeThrough":
                items.append(row("seeThrough", .seeThrough, "See through", act(GeoActionToggleSeeThrough)))
            case "section": items.append(row("section", .scissors, "Section") { [weak self] in self?.onShowSection?() })
            case "look": items.append(row("look", .palette, "Look") { [weak self] in self?.onShowLook?() })
            case "projection":
                for (index, title) in ["Orthographic", "Perspective"].enumerated() {
                    items.append(
                        .row(title: title, icon: nil, checked: projection.selected == index, enabled: true) {
                            [weak self] in
                            guard let self, projection.selected != index else {
                                return
                            }
                            projection.selected = index
                            onProjection?(index == 1)
                        })
                }
            case "views":
                items.append(.heading("Views"))
                let views: [(String, GeoAction)] = [
                    ("Front", GeoActionViewFront), ("Back", GeoActionViewBack), ("Left", GeoActionViewLeft),
                    ("Right", GeoActionViewRight), ("Top", GeoActionViewTop), ("Bottom", GeoActionViewBottom),
                    ("Isometric", GeoActionViewIso),
                ]
                for (title, action) in views {
                    items.append(row("views", nil, title, act(action)))
                }
            case "fit": items.append(row("fit", .fit, "Fit to view", act(GeoActionFit)))
            case "picture":
                items.append(
                    row("picture", .camera, "Copy picture") { [weak self] in
                        self?.onCopyPicture?()
                    }
                )
                items.append(
                    row("picture", nil, "Save picture\u{2026}") { [weak self] in
                        self?.onSavePicture?()
                    }
                )
            case "shortcuts": items.append(row("shortcuts", nil, "Shortcuts") { [weak self] in self?.onShowShortcuts?() })
            default: break
            }
        }
        moreMenu.setItems(items)
        moreMenu.toggle(above: more)
    }

    /// Reflects what the engine can do now.
    func update(hasSelection: Bool, hasModel: Bool) {
        for button in [hide, ghost, isolate] {
            button.isEnabled = hasSelection
        }
        for button in [showAll, fit, views, undo, redo, seeThrough, picture] {
            button.isEnabled = hasModel
        }
    }
}
