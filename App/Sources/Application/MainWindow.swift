import AppKit
import GeometryViewEngine
import UniformTypeIdentifiers

/// The single window: a full-bleed viewport with the chrome floating above it.
///
/// Every piece of chrome is its own layer tree over the Metal layer, so changing any of it never
/// draws a 3D frame.
@MainActor
final class MainWindow: NSWindow {
    private let engine: EngineHandle
    private let viewport: ViewportView
    private let dragArea = WindowDragArea()
    private let masthead = Masthead()
    private let structure: StructureCard
    private let selection = SelectionCard()
    private let toolBar = Toolbar()
    private let status = StatusCard()
    private let holdHint = HoldHint()
    private let hoverChip = Chip()
    private let dropZone = DropZone()
    private let shortcuts = ShortcutsSheet()
    private let lookCard = LookCard()
    private let library = LibraryStore()
    private lazy var libraryCard = LibraryCard(store: library)
    private lazy var watcher = DirectoryWatcher { [weak self] paths in self?.filesChanged(paths) }
    private var openURLs: [URL] = []
    private let sectionCard = SectionCard()
    private var look = Look.saved()
    private var exercise: Exercise?
    private var chromeCheck: ChromeCheck?
    private var accessedURLs: [URL] = []

    private var names: [String] = []
    private var progress: [UInt32: Double] = [:]
    private var perspective = false
    private var tool = GeoToolSelect
    private var hovered: (document: UInt32, node: UInt32, face: UInt32)?
    private var pointer: NSPoint?

    private static let cardWidth: CGFloat = 300
    private static let selectionWidth: CGFloat = 286
    private static let edge: CGFloat = 18
    private static let cardTop: CGFloat = 72
    private static let cardBottom: CGFloat = 96
    private static let toolbarBottom: CGFloat = 22
    private static let hintBottom: CGFloat = 86
    private static let selectionTop: CGFloat = 172
    // The view cube's square, drawn by the engine, with its margin.
    private static let cubeClearance: CGFloat = 144

    /// The height of the title bar the view extends under.
    private var titleBarHeight: CGFloat {
        let height = frame.height - contentLayoutRect.height
        return height > 0 ? height : 28
    }

    init(engine: EngineHandle) {
        self.engine = engine
        structure = StructureCard(engine: engine)
        viewport = ViewportView(engine: engine)
        super.init(
            contentRect: NSRect(x: 0, y: 0, width: 1440, height: 900),
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )

        title = "GeometryView"
        titlebarAppearsTransparent = true
        titleVisibility = .hidden
        isMovableByWindowBackground = false
        backgroundColor = Palette.paper
        isReleasedWhenClosed = false
        minSize = NSSize(width: 960, height: 600)
        center()

        contentView = makeLayout()
        connect()
        showEmpty()
    }

    // MARK: Layout

    private func makeLayout() -> NSView {
        let root = NSView()
        root.wantsLayer = true
        let views: [NSView] = [
            viewport, dragArea, masthead, structure, libraryCard, selection, toolBar, lookCard,
            sectionCard,
            holdHint,
            hoverChip,
            status, dropZone, shortcuts,
        ]
        for view in views {
            view.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(view)
        }
        // The space right of the tree card, where the toolBar and hints centre themselves.
        let free = NSLayoutGuide()
        root.addLayoutGuide(free)

        NSLayoutConstraint.activate([
            viewport.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            viewport.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            viewport.topAnchor.constraint(equalTo: root.topAnchor),
            viewport.bottomAnchor.constraint(equalTo: root.bottomAnchor),

            // As tall as a title bar, and clear of the view cube in the top right.
            dragArea.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            dragArea.trailingAnchor.constraint(
                equalTo: root.trailingAnchor,
                constant: -Self.cubeClearance
            ),
            dragArea.topAnchor.constraint(equalTo: root.topAnchor),
            dragArea.heightAnchor.constraint(equalToConstant: titleBarHeight),

            // Clear of the window's buttons, in line with them.
            masthead.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 88),
            masthead.topAnchor.constraint(equalTo: root.topAnchor, constant: 10),
            masthead.trailingAnchor.constraint(lessThanOrEqualTo: root.centerXAnchor),

            structure.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: Self.edge),
            structure.topAnchor.constraint(equalTo: root.topAnchor, constant: Self.cardTop),
            structure.bottomAnchor.constraint(
                equalTo: root.bottomAnchor,
                constant: -Self.cardBottom
            ),
            structure.widthAnchor.constraint(equalToConstant: Self.cardWidth),
            libraryCard.leadingAnchor.constraint(equalTo: structure.leadingAnchor),
            libraryCard.topAnchor.constraint(equalTo: structure.topAnchor),
            libraryCard.bottomAnchor.constraint(equalTo: structure.bottomAnchor),
            libraryCard.widthAnchor.constraint(equalTo: structure.widthAnchor),

            selection.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -Self.edge),
            selection.topAnchor.constraint(equalTo: root.topAnchor, constant: Self.selectionTop),
            selection.widthAnchor.constraint(equalToConstant: Self.selectionWidth),

            free.leadingAnchor.constraint(equalTo: structure.trailingAnchor),
            free.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            toolBar.centerXAnchor.constraint(equalTo: free.centerXAnchor),
            toolBar.bottomAnchor.constraint(
                equalTo: root.bottomAnchor,
                constant: -Self.toolbarBottom
            ),
            sectionCard.centerXAnchor.constraint(equalTo: toolBar.centerXAnchor),
            sectionCard.bottomAnchor.constraint(equalTo: toolBar.topAnchor, constant: -12),
            lookCard.centerXAnchor.constraint(equalTo: toolBar.centerXAnchor),
            lookCard.bottomAnchor.constraint(equalTo: toolBar.topAnchor, constant: -12),
            holdHint.centerXAnchor.constraint(equalTo: free.centerXAnchor),
            holdHint.bottomAnchor.constraint(
                equalTo: root.bottomAnchor,
                constant: -Self.hintBottom
            ),

            status.centerXAnchor.constraint(equalTo: root.centerXAnchor),
            status.centerYAnchor.constraint(equalTo: root.centerYAnchor),
            status.widthAnchor.constraint(equalToConstant: 360),

            dropZone.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 14),
            dropZone.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -14),
            dropZone.topAnchor.constraint(equalTo: root.topAnchor, constant: 14),
            dropZone.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -14),

            shortcuts.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            shortcuts.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            shortcuts.topAnchor.constraint(equalTo: root.topAnchor),
            shortcuts.bottomAnchor.constraint(equalTo: root.bottomAnchor),
        ])
        hoverChip.isHidden = true
        lookCard.isHidden = true
        sectionCard.isHidden = true
        return root
    }

    private func connect() {
        viewport.onDropFiles = { [weak self] urls in
            // Holding Command adds files to what is shown, as in the library.
            let adding = NSApp.currentEvent?.modifierFlags.contains(.command) == true
            for (index, url) in urls.enumerated() {
                if adding || index > 0 {
                    self?.add(url)
                } else {
                    self?.open(url)
                }
            }
        }
        viewport.onKeyCommand = { [weak self] command in self?.run(command) }
        viewport.onToolChange = { [weak self] tool in self?.toolChanged(tool) }
        viewport.onPointerMoved = { [weak self] point in self?.pointerMoved(point) }
        viewport.onDragging = { [weak self] dragging in self?.dropZone.isHidden = !dragging }

        toolBar.onAction = { [weak self] action in self?.engine.perform(action) }
        toolBar.onShowShortcuts = { [weak self] in self?.shortcuts.isHidden = false }
        toolBar.onShowLook = { [weak self] in self?.toggleLookCard(nil) }
        structure.switcher.onChange = { [weak self] index in self?.showLibrary(index == 0) }
        libraryCard.switcher.onChange = { [weak self] index in self?.showLibrary(index == 0) }
        libraryCard.onOpen = { [weak self] url, adding in
            adding ? self?.add(url) : self?.open(url)
        }
        libraryCard.onConnect = { [weak self] in self?.connectFolder(nil) }
        library.onChange = { [weak self] in
            guard let self else {
                return
            }
            libraryCard.reload()
            watchFiles()
        }
        watchFiles()
        toolBar.onShowSection = { [weak self] in self?.toggleSectionCard() }
        sectionCard.onChange = { [weak self] section in self?.engine.setSection(section) }
        lookCard.onChange = { [weak self] look in self?.apply(look) }
        lookCard.show(look)
        masthead.setOnDarkSheet(look.style.onDark)
        engine.setLook(look)
        toolBar.pick.onChange = { [weak self] index in
            self?.engine.setSelectionMode(GeoSelectionMode(rawValue: UInt32(index)))
            self?.refreshSelection()
        }
        toolBar.projection.onChange = { [weak self] index in
            guard let self, (index == 1) != perspective else {
                return
            }
            perspective = index == 1
            engine.perform(GeoActionToggleProjection)
        }
        selection.onAction = { [weak self] action in self?.engine.perform(action) }
        shortcuts.onClose = { [weak self] in self?.shortcuts.isHidden = true }

        engine.onEvent = { [weak self] event in
            self?.handle(event)
        }
    }

    // MARK: Files

    /// Opens a file in place of whatever is shown.
    func open(_ url: URL) {
        for accessed in accessedURLs {
            accessed.stopAccessingSecurityScopedResource()
        }
        accessedURLs = []
        openURLs = []
        names = []
        progress = [:]
        structure.reset()
        selection.isHidden = true
        add(url, replacing: true)
    }

    /// Opens a file alongside what is shown.
    func add(_ url: URL) {
        add(url, replacing: false)
    }

    private func add(_ url: URL, replacing: Bool) {
        // The sandbox grant travels with the URL, and a path alone would not carry it. Access stays
        // active while the file is open, because the import helper reads it after this returns.
        if url.startAccessingSecurityScopedResource() {
            accessedURLs.append(url)
        }
        names.append(url.lastPathComponent)
        openURLs.append(url)
        watchFiles()
        structure.addDocument(named: url.lastPathComponent)
        showLibrary(false)
        status.isHidden = true
        if replacing {
            engine.open(path: url.path)
        } else {
            engine.add(path: url.path)
        }
        refreshMasthead()
    }

    func chooseFile(adding: Bool) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = adding
        panel.canChooseDirectories = false
        panel.allowedContentTypes = ["stl", "step", "stp"].compactMap {
            UTType(filenameExtension: $0)
        }
        panel.beginSheetModal(for: self) { [weak self] response in
            guard response == .OK else {
                return
            }
            for (index, url) in panel.urls.enumerated() {
                if adding || index > 0 {
                    self?.add(url)
                } else {
                    self?.open(url)
                }
            }
        }
    }

    // MARK: Commands

    @objc func performAction(_ sender: NSMenuItem) {
        engine.perform(GeoAction(rawValue: UInt32(sender.tag)))
    }

    @objc func chooseSelectionMode(_ sender: NSMenuItem) {
        setMode(GeoSelectionMode(rawValue: UInt32(sender.tag)))
    }

    @objc func toggleProjection(_ sender: Any?) {
        run(.toggleProjection)
    }

    // MARK: Requests from AI apps

    /// What an AI app's tool asked to see, through geometryview://show?file=…&isolate=…&style=…
    /// &colour=…, sent just after the file itself: some parts shown alone, a style and colours.
    /// The request names the file, which the app opens through the usual route, with its sandbox
    /// grant; the request itself reads nothing.
    struct ShowRequest {
        var path: String
        var isolate: [String]
        var style: Look.Style?
        var colour: Look.Colour?

        init?(url: URL) {
            guard url.scheme == "geometryview", url.host == "show",
                let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems,
                let path = items.first(where: { $0.name == "file" })?.value
            else {
                return nil
            }
            let value = { (name: String) in items.first { $0.name == name }?.value?.lowercased() }
            self.path = path
            isolate = items.filter { $0.name == "isolate" }.compactMap(\.value).filter { !$0.isEmpty }
            style = value("style").flatMap { name in Look.Style.allCases.first { $0.title.lowercased() == name } }
            colour = value("colour").flatMap { name in Look.Colour.allCases.first { $0.title.lowercased() == name } }
        }
    }

    private var requestedShow: ShowRequest?

    func show(_ request: ShowRequest) {
        if request.style != nil || request.colour != nil {
            var next = look
            if let style = request.style {
                next.choose(style)
            }
            if let colour = request.colour {
                next.colour = colour
            }
            apply(next, remember: false)
        }
        requestedShow = request.isolate.isEmpty ? nil : request
        showAsRequested()
    }

    /// Shows the requested parts alone, once the file's model is in and the scene is complete.
    private func showAsRequested() {
        guard let request = requestedShow, progress.isEmpty,
            let document = openURLs.firstIndex(where: { $0.path == request.path }),
            document < structure.tree.documents.count,
            let model = structure.tree.documents[document].model
        else {
            return
        }
        requestedShow = nil
        engine.perform(GeoActionShowAll)
        engine.perform(GeoActionClearSelection)
        // Only the outermost of nested matches: "rotor" finds the rotor assembly and its
        // rotor_core, and selecting the core again, inside the selected assembly, would take it out.
        let matches = Set(request.isolate.flatMap { model.search($0).matches })
        var found = 0
        var insideMatch = 0
        for node in matches.sorted() where node >= insideMatch {
            guard model.nodes[node].kind != GeoNodeBody.rawValue else {
                continue
            }
            engine.selectNode(document: UInt32(document), node: UInt32(node), extend: true)
            found += 1
            insideMatch = Int(model.nodes[node].subtree_end)
        }
        if found > 0 {
            engine.perform(GeoActionIsolate)
            engine.perform(GeoActionClearSelection)
        }
    }

    // MARK: Look

    /// Shows `newLook`, and remembers it unless an AI app's request chose it.
    private func apply(_ newLook: Look, remember: Bool = true) {
        look = newLook
        if remember {
            look.save()
        }
        lookCard.show(look)
        masthead.setOnDarkSheet(look.style.onDark)
        engine.setLook(look)
    }

    @objc func chooseStyle(_ sender: NSMenuItem) {
        guard let style = Look.Style(rawValue: sender.tag) else {
            return
        }
        var next = look
        next.choose(style)
        apply(next)
    }

    @objc func chooseColour(_ sender: NSMenuItem) {
        guard let colour = Look.Colour(rawValue: sender.tag) else {
            return
        }
        var next = look
        next.colour = colour
        apply(next)
    }

    @objc func chooseGhosts(_ sender: NSMenuItem) {
        guard let ghosts = Look.Ghosts(rawValue: sender.tag) else {
            return
        }
        var next = look
        next.ghosts = ghosts
        apply(next)
    }

    @objc func toggleShadow(_ sender: Any?) {
        var next = look
        next.shadow.toggle()
        apply(next)
    }

    @objc func toggleLookCard(_ sender: Any?) {
        lookCard.isHidden.toggle()
        sectionCard.isHidden = true
    }

    // MARK: Library

    /// Shows the library, or the structure of what is open, in the left card.
    private func showLibrary(_ show: Bool) {
        libraryCard.isHidden = !show
        structure.isHidden = show
        libraryCard.switcher.selected = 0
        structure.switcher.selected = 1
        updateInsets()
    }

    /// Watches the connected folders, and the open files wherever they are, so an edit reloads.
    private func watchFiles() {
        watcher.watch(library.roots.map(\.url.path) + openURLs.map(\.path))
    }

    @objc func connectFolder(_ sender: Any?) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = true
        panel.prompt = "Connect"
        panel.beginSheetModal(for: self) { [weak self] response in
            guard response == .OK, let self else {
                return
            }
            for url in panel.urls {
                library.connect(url)
            }
            showLibrary(true)
        }
    }

    /// Lists changed folders again, and reloads an open file that changed on disk.
    private func filesChanged(_ paths: [String]) {
        var folders = Set<String>()
        var reload = false
        for path in paths {
            folders.insert((path as NSString).deletingLastPathComponent)
            folders.insert(path)
            reload = reload || openURLs.contains { $0.path == path }
        }
        for folder in folders {
            for root in library.roots {
                if let node = root.find(folder), node.isDirectory {
                    libraryCard.refresh(node)
                }
            }
        }
        if reload {
            // An edited file has a new cache key, so opening it again imports what changed.
            let urls = openURLs
            if let first = urls.first {
                open(first)
                urls.dropFirst().forEach { add($0) }
            }
        }
    }

    // MARK: Section

    private func toggleSectionCard() {
        sectionCard.isHidden.toggle()
        lookCard.isHidden = true
    }

    /// S cuts across X at the middle, or, with a cut open, removes every cut.
    @objc func toggleSection(_ sender: Any?) {
        var section = sectionCard.section
        if sectionCard.isCutting {
            section.enabled = (false, false, false)
            sectionCard.isHidden = true
        } else {
            section.enabled.0 = true
            sectionCard.isHidden = false
            lookCard.isHidden = true
        }
        sectionCard.show(section)
        engine.setSection(section)
    }

    /// Ticks the menu's look items.
    override func validateMenuItem(_ item: NSMenuItem) -> Bool {
        switch item.action {
        case #selector(chooseStyle(_:)):
            item.state = item.tag == look.style.rawValue ? .on : .off
        case #selector(chooseColour(_:)):
            item.state = item.tag == look.colour.rawValue ? .on : .off
            return look.style.usesColour
        case #selector(chooseGhosts(_:)):
            item.state = item.tag == look.ghosts.rawValue ? .on : .off
        case #selector(toggleShadow(_:)):
            item.state = look.shadow ? .on : .off
        default:
            return super.validateMenuItem(item)
        }
        return true
    }

    @objc func showShortcuts(_ sender: Any?) {
        shortcuts.isHidden = false
    }

    private func run(_ command: KeyCommand) {
        switch command {
        case .perform(let action):
            engine.perform(action)
        case .selectionMode(let mode):
            setMode(mode)
        case .toggleProjection:
            perspective.toggle()
            toolBar.projection.selected = perspective ? 1 : 0
            engine.perform(GeoActionToggleProjection)
        case .showShortcuts:
            shortcuts.isHidden.toggle()
        case .toggleSection:
            toggleSection(nil)
        }
    }

    private func setMode(_ mode: GeoSelectionMode) {
        toolBar.pick.selected = Int(mode.rawValue)
        engine.setSelectionMode(mode)
        refreshSelection()
    }

    override func resignKey() {
        super.resignKey()
        viewport.releaseHeldTool()
    }

    override func cancelOperation(_ sender: Any?) {
        if !shortcuts.isHidden {
            shortcuts.isHidden = true
        }
        lookCard.isHidden = true
        sectionCard.isHidden = true
    }

    // MARK: Pointer and tools

    private func toolChanged(_ newTool: GeoTool) {
        tool = newTool
        holdHint.show(newTool)
        refreshCursor()
    }

    private func pointerMoved(_ point: NSPoint?) {
        pointer = point
        placeHoverChip()
    }

    private func refreshCursor() {
        switch tool {
        case GeoToolHide, GeoToolGhost: viewport.cursor = .crosshair
        default: viewport.cursor = hovered == nil ? .arrow : .pointingHand
        }
    }

    // A label naming what is under the pointer, beside it.
    private func showHover() {
        guard let hovered else {
            hoverChip.isHidden = true
            refreshCursor()
            return
        }
        let row = TreeRow(document: hovered.document, node: hovered.node, depth: 0)
        hoverChip.setContent([
            Chip.text(structure.tree.name(of: row), weight: .medium),
            Chip.text("face \(hovered.face + 1)", color: Palette.muted),
        ])
        refreshCursor()
        placeHoverChip()
    }

    private func placeHoverChip() {
        guard hovered != nil, let pointer, let content = contentView else {
            hoverChip.isHidden = true
            return
        }
        hoverChip.isHidden = false
        hoverChip.layoutSubtreeIfNeeded()
        let size = hoverChip.fittingSize
        let x = min(pointer.x + 16, content.bounds.width - size.width - 8)
        let y = max(pointer.y - 14 - size.height, 8)
        hoverChip.frame = NSRect(origin: NSPoint(x: x, y: y), size: size)
    }

    // MARK: Engine events

    private func handle(_ event: EngineEvent) {
        switch event {
        case .loading(let document, _):
            progress[document] = 0
            refreshMasthead()
        case .progress(let document, let done, let total):
            progress[document] = total > 0 ? Double(done) / Double(total) : 0
            refreshMasthead()
        case .modelReady(let document):
            structure.modelReady(document: document)
            refreshMasthead()
            toolBar.update(hasSelection: false, hasModel: true)
        case .sceneReady:
            progress = [:]
            refreshMasthead()
            showAsRequested()
            startAutomation()
        case .failed(_, let message):
            progress = [:]
            refreshMasthead()
            status.showFailure(message)
        case .selectionChanged(let document, let node):
            structure.reveal(document: document, node: node)
            refreshSelection()
        case .hoverChanged(let document, let node, let face):
            hovered = node.map { (document, $0, face) }
            showHover()
            structure.highlight(document: document, node: node)
        case .looksChanged:
            structure.looksChanged()
        case .visibilityChanged(let hidden, let ghosted):
            structure.visibilityChanged(hidden: hidden, ghosted: ghosted)
        }
    }

    private func refreshSelection() {
        let summary = engine.selection()
        selection.show(summary, tree: structure.tree)
        toolBar.update(hasSelection: summary.occurrences > 0, hasModel: !names.isEmpty)
    }

    private func refreshMasthead() {
        guard !names.isEmpty else {
            masthead.isHidden = true
            return
        }
        let title = names.count == 1 ? names[0] : "\(names.count) files"
        var parts = 0
        var bodies = 0
        for document in structure.tree.documents {
            guard let model = document.model else {
                continue
            }
            for node in model.nodes where node.kind == GeoNodePart.rawValue {
                parts += 1
            }
            bodies += Int(model.nodes.map(\.occurrence_end).max() ?? 0)
        }
        var detail = names.count == 1 ? "" : names.joined(separator: ", ")
        if bodies > 0 {
            let count = { (n: Int, one: String, many: String) in
                "\(n.formatted()) \(n == 1 ? one : many)"
            }
            let counts = "\(count(parts, "part", "parts")) · \(count(bodies, "body", "bodies"))"
            detail = detail.isEmpty ? counts : "\(counts) · \(detail)"
        } else {
            detail = "Reading…"
        }
        masthead.show(title: title, subtitle: detail)
        let fractions = Array(progress.values)
        masthead.setProgress(
            fractions.isEmpty ? nil : fractions.reduce(0, +) / Double(fractions.count)
        )
    }

    // The tree card covers the left, the masthead the top, and the toolbar the bottom. Framing
    // keeps the model in the rest.
    private func updateInsets() {
        let left = structure.isHidden && libraryCard.isHidden ? 0 : Self.edge + Self.cardWidth
        engine.setViewInsets(
            left: left,
            top: Self.cardTop - 16,
            right: 0,
            bottom: Self.cardBottom - 16
        )
    }

    private func showEmpty() {
        status.showInvitation()
        structure.isHidden = true
        libraryCard.isHidden = library.roots.isEmpty
        updateInsets()
        toolBar.update(hasSelection: false, hasModel: false)
    }

    // MARK: Automation

    /// Changes only the chrome: shows and hides the sheet, the drop frame and the hold hint. The
    /// exercise checks that none of this draws a 3D frame.
    func exerciseChrome(step: Int) {
        shortcuts.isHidden = step % 2 == 1
        dropZone.isHidden = step % 3 != 1
        holdHint.show(step % 2 == 0 ? GeoToolGhost : GeoToolSelect)
        toolBar.update(hasSelection: step % 2 == 0, hasModel: true)
    }

    private func startAutomation() {
        if ChromeSnapshot.isRequested {
            // With GEOMETRYVIEW_SNAPSHOT_SELECT set, the first part is selected, to show its card.
            let environment = ProcessInfo.processInfo.environment
            if environment["GEOMETRYVIEW_SNAPSHOT_SELECT"] != nil {
                engine.selectNode(document: 0, node: 1, extend: false)
            }
            // GEOMETRYVIEW_SNAPSHOT_ISOLATE isolates the first part, and waits for the view to
            // frame it.
            let isolating = environment["GEOMETRYVIEW_SNAPSHOT_ISOLATE"] != nil
            if isolating {
                engine.selectNode(document: 0, node: 1, extend: false)
                engine.perform(GeoActionIsolate)
            }
            // With GEOMETRYVIEW_SNAPSHOT_HOLD set, the hide tool's hint shows, as while H is held.
            if environment["GEOMETRYVIEW_SNAPSHOT_HOLD"] != nil {
                toolChanged(GeoToolHide)
            }
            // GEOMETRYVIEW_SNAPSHOT_STYLE picks a style by number, and GEOMETRYVIEW_SNAPSHOT_LOOK
            // opens the look card.
            if let style = environment["GEOMETRYVIEW_SNAPSHOT_STYLE"].flatMap(Int.init)
                .flatMap(Look.Style.init(rawValue:))
            {
                var next = look
                next.choose(style)
                apply(next)
            }
            if environment["GEOMETRYVIEW_SNAPSHOT_LOOK"] != nil {
                lookCard.isHidden = false
            }
            // GEOMETRYVIEW_SNAPSHOT_SEE_THROUGH fades the shell, as T does.
            if environment["GEOMETRYVIEW_SNAPSHOT_SEE_THROUGH"] != nil {
                engine.perform(GeoActionToggleSeeThrough)
            }
            // GEOMETRYVIEW_SNAPSHOT_SECTION cuts across X, as S does, with the card open.
            if environment["GEOMETRYVIEW_SNAPSHOT_SECTION"] != nil {
                toggleSection(nil)
            }
            // GEOMETRYVIEW_SNAPSHOT_GHOSTS ghosts the selection, drawn as shell, lines or both (1
            // to 3), and then clears it.
            if let ghosts = environment["GEOMETRYVIEW_SNAPSHOT_GHOSTS"].flatMap(Int.init)
                .flatMap(Look.Ghosts.init(rawValue:))
            {
                var next = look
                next.ghosts = ghosts
                apply(next)
                engine.perform(GeoActionGhostSelection)
                engine.perform(GeoActionClearSelection)
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + (isolating ? 1.5 : 0.5)) { [self] in
                ChromeSnapshot.capture(self, engine: engine)
                DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
                    NSApp.terminate(nil)
                }
            }
        }
        if ChromeCheck.isRequested, chromeCheck == nil {
            let check = ChromeCheck(window: self, engine: engine)
            chromeCheck = check
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [self] in
                check.start(lookCard: lookCard)
            }
        }
        if Exercise.isRequested, exercise == nil {
            let run = Exercise(engine: engine, size: contentView?.bounds.size ?? .zero)
            exercise = run
            run.start(window: self)
        }
    }
}
