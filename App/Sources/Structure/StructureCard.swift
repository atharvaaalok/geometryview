import AppKit
import GeometryViewEngine

/// The assembly tree of everything open, with search. Selecting a row selects that part in the
/// view, and selecting in the view reveals its row.
@MainActor
final class StructureCard: StickerCard, NSTableViewDataSource, NSTableViewDelegate,
    NSSearchFieldDelegate
{
    private let engine: EngineHandle
    let tree = TreeRows()
    /// Switches the left card between the library and this tree.
    let switcher = SegmentedPill(
        ["Library", "Structure"],
        selected: 1,
        help: "Show files or parts"
    )
    private let searchField = NSSearchField()
    private let matchLabel = NSTextField(labelWithString: "")
    private let table = TreeTableView()
    private let scroll = NSScrollView()
    private let footer = VisibilityFooter()
    private var scrollAboveFooter: NSLayoutConstraint?
    private var scrollAtBottom: NSLayoutConstraint?
    private var applyingEngineSelection = false
    private var matches: [(document: UInt32, node: UInt32)] = []
    private var matchIndex = 0

    static let rowHeight: CGFloat = 28
    /// The row of the part under the pointer in the view, lit like a row under the pointer.
    private var highlighted: (document: UInt32, node: UInt32)?

    init(engine: EngineHandle) {
        self.engine = engine
        super.init(frame: .zero)

        searchField.placeholderString = "Search parts"
        searchField.font = Typography.font(Typography.textMedium)
        searchField.delegate = self
        searchField.sendsSearchStringImmediately = true
        searchField.target = self
        searchField.action = #selector(searchChanged)

        matchLabel.font = Typography.font(Typography.textExtraSmall)
        matchLabel.textColor = Palette.muted

        let column = NSTableColumn(identifier: .init("name"))
        table.addTableColumn(column)
        table.headerView = nil
        table.rowHeight = Self.rowHeight
        table.intercellSpacing = .zero
        table.backgroundColor = .clear
        table.style = .plain
        table.selectionHighlightStyle = .regular
        table.allowsMultipleSelection = false
        table.dataSource = self
        table.delegate = self
        table.target = self
        table.action = #selector(rowClicked)
        table.onHover = { [weak self] row in self?.hover(row) }
        table.onToggle = { [weak self] row in self?.toggle(row) }
        table.isLit = { [weak self] row in self?.isHighlighted(row) ?? false }

        scroll.documentView = table
        scroll.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(scrolled),
            name: NSView.boundsDidChangeNotification,
            object: scroll.contentView
        )
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.scrollerStyle = .overlay

        footer.onShowAll = { [weak self] in self?.engine.perform(GeoActionShowAll) }
        footer.onUnghostAll = { [weak self] in self?.engine.perform(GeoActionUnghostAll) }
        footer.isHidden = true

        for view in [switcher, searchField, matchLabel, scroll, footer] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            addSubview(view)
        }
        let inset = Metrics.gap
        NSLayoutConstraint.activate([
            switcher.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inset),
            switcher.topAnchor.constraint(equalTo: topAnchor, constant: inset - 4),
            matchLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inset),
            matchLabel.centerYAnchor.constraint(equalTo: switcher.centerYAnchor),
            searchField.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inset),
            searchField.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inset),
            searchField.topAnchor.constraint(equalTo: switcher.bottomAnchor, constant: 10),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 6),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -6),
            scroll.topAnchor.constraint(equalTo: searchField.bottomAnchor, constant: 10),
            footer.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inset),
            footer.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inset),
            footer.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -10),
        ])
        let atBottom = scroll.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -8)
        let aboveFooter = scroll.bottomAnchor.constraint(
            equalTo: footer.topAnchor,
            constant: -6
        )
        atBottom.isActive = true
        scrollAtBottom = atBottom
        scrollAboveFooter = aboveFooter
    }

    // MARK: Documents

    func reset() {
        tree.reset()
        clearSearch()
        table.reloadData()
    }

    func addDocument(named name: String) {
        tree.addDocument(named: name)
        table.reloadData()
    }

    func modelReady(document: UInt32) {
        guard let model = engine.model(document: document) else {
            return
        }
        tree.setModel(model, for: Int(document))
        table.reloadData()
    }

    /// Shows the row of what the view selected.
    func reveal(document: UInt32, node: UInt32?) {
        applyingEngineSelection = true
        defer { applyingEngineSelection = false }
        guard let node, let row = tree.reveal(document: document, node: node) else {
            table.deselectAll(nil)
            return
        }
        table.reloadData()
        table.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
        table.scrollRowToVisible(row)
    }

    // MARK: Interaction

    @objc private func rowClicked() {
        let index = table.clickedRow
        guard index >= 0, index < tree.rows.count else {
            return
        }
        let row = tree.rows[index]
        if row.isDocument {
            tree.toggle(row)
            table.reloadData()
            return
        }
        let extend =
            NSApp.currentEvent?.modifierFlags.contains(.command) == true
            || NSApp.currentEvent?.modifierFlags.contains(.shift) == true
        engine.selectNode(document: row.document, node: row.node, extend: extend)
        table.selectRowIndexes(IndexSet(integer: index), byExtendingSelection: false)
    }

    private func toggle(_ index: Int) {
        guard index >= 0, index < tree.rows.count, tree.hasChildren(tree.rows[index]) else {
            return
        }
        tree.toggle(tree.rows[index])
        table.reloadData()
    }

    private func hover(_ index: Int?) {
        guard let index, index < tree.rows.count, !tree.rows[index].isDocument else {
            engine.hoverNode(document: 0, node: nil)
            return
        }
        let row = tree.rows[index]
        engine.hoverNode(document: row.document, node: row.node)
    }

    // MARK: Search

    @objc private func searchChanged() {
        let query = searchField.stringValue
        matches = []
        matchIndex = 0
        guard !query.isEmpty else {
            matchLabel.stringValue = ""
            return
        }
        var total = 0
        for (index, document) in tree.documents.enumerated() {
            guard let model = document.model else {
                continue
            }
            let found = model.search(query)
            total += found.total
            matches += found.matches.map { (UInt32(index), UInt32($0)) }
        }
        showMatch()
        matchLabel.stringValue = total == 0 ? "no match" : "1 of \(total)"
    }

    func control(
        _ control: NSControl,
        textView: NSTextView,
        doCommandBy selector: Selector
    ) -> Bool {
        // Return steps through the matches.
        guard selector == #selector(NSResponder.insertNewline(_:)), !matches.isEmpty else {
            return false
        }
        matchIndex = (matchIndex + 1) % matches.count
        showMatch()
        matchLabel.stringValue = "\(matchIndex + 1) of \(matches.count)"
        return true
    }

    private func showMatch() {
        guard matchIndex < matches.count else {
            return
        }
        let match = matches[matchIndex]
        reveal(document: match.document, node: match.node)
    }

    private func clearSearch() {
        searchField.stringValue = ""
        matches = []
        matchLabel.stringValue = ""
    }

    // MARK: Table

    func numberOfRows(in tableView: NSTableView) -> Int {
        tree.rows.count
    }

    func tableView(_ tableView: NSTableView, rowViewForRow row: Int) -> NSTableRowView? {
        TreeRowView()
    }

    func tableView(_ tableView: NSTableView, didAdd rowView: NSTableRowView, forRow row: Int) {
        (rowView as? TreeRowView)?.isHovered = table.isHovered(row)
    }

    func tableView(
        _ tableView: NSTableView,
        viewFor tableColumn: NSTableColumn?,
        row index: Int
    ) -> NSView? {
        let cell =
            tableView.makeView(withIdentifier: TreeCell.identifier, owner: nil) as? TreeCell
            ?? TreeCell()
        let row = tree.rows[index]
        cell.configure(
            name: tree.name(of: row),
            depth: row.depth,
            mark: mark(of: row),
            isDocument: row.isDocument,
            disclosure: tree.hasChildren(row) ? (tree.isExpanded(row) ? .open : .closed) : .none,
            count: tree.count(of: row),
            state: row.isDocument ? nil : engine.nodeState(document: row.document, node: row.node)
        )
        cell.showsActions = table.isUnderPointer(index)
        cell.onAction = { [weak self, weak cell] action in
            guard let self, let cell else {
                return
            }
            rowAction(action, at: table.row(for: cell))
        }
        return cell
    }

    // A row button: hide or show, ghost or unghost, by what the row is now, or isolate.
    private func rowAction(_ action: TreeCell.Action, at index: Int) {
        guard index >= 0, index < tree.rows.count, !tree.rows[index].isDocument else {
            return
        }
        let row = tree.rows[index]
        let state = engine.nodeState(document: row.document, node: row.node)
        let nodeAction: GeoNodeAction =
            switch action {
            case .ghost: state == GeoNodeGhosted ? GeoNodeUnghost : GeoNodeGhost
            case .hide: state == GeoNodeHidden ? GeoNodeShow : GeoNodeHide
            case .isolate: GeoNodeIsolate
            }
        engine.nodeAction(document: row.document, node: row.node, action: nodeAction)
    }

    // MARK: Hidden and ghosted

    /// Shows each row's state again, and the footer's counts, after what is hidden or ghosted
    /// changed.
    func visibilityChanged(hidden: UInt32, ghosted: UInt32) {
        footer.show(hidden: hidden, ghosted: ghosted)
        let showing = hidden > 0 || ghosted > 0
        footer.isHidden = !showing
        scrollAtBottom?.isActive = !showing
        scrollAboveFooter?.isActive = showing
        looksChanged()
    }

    // A file's or an assembly's glyph, or a swatch of the colour the part is drawn in.
    private func mark(of row: TreeRow) -> TreeCell.Mark {
        if row.isDocument {
            return .document
        }
        let kind = tree.kind(of: row)
        if kind == GeoNodeAssembly {
            return .assembly
        }
        let color = tree.firstOccurrence(of: row).flatMap {
            engine.swatch(document: row.document, occurrence: $0)
        }
        return .swatch(color, small: kind == GeoNodeBody)
    }

    // MARK: Colours and highlights

    /// Repaints the rows in view, after their colours or states changed.
    func looksChanged() {
        let visible = table.rows(in: table.visibleRect)
        guard visible.length > 0 else {
            return
        }
        table.reloadData(
            forRowIndexes: IndexSet(integersIn: visible.location..<NSMaxRange(visible)),
            columnIndexes: IndexSet(integer: 0)
        )
    }

    /// Lights the row of the part under the pointer in the view, if it is shown.
    func highlight(document: UInt32, node: UInt32?) {
        highlighted = node.map { (document, $0) }
        table.refreshHover()
    }

    private func isHighlighted(_ index: Int) -> Bool {
        guard let highlighted, index < tree.rows.count else {
            return false
        }
        let row = tree.rows[index]
        return !row.isDocument && row.document == highlighted.document
            && row.node == highlighted.node
    }

    // Scrolling moves rows under a still pointer, which sends no move.
    @objc private func scrolled() {
        table.syncPointer()
    }
}

/// A table that reports the row under the pointer and clicks on disclosure chevrons, and lights
/// the rows under the pointer here or in the view.
@MainActor
final class TreeTableView: NSTableView {
    var onHover: ((Int?) -> Void)?
    var onToggle: ((Int) -> Void)?
    /// Whether a row's part is under the pointer in the view.
    var isLit: ((Int) -> Bool)?
    private var hoveredRow: Int?
    private var tracking: NSTrackingArea?

    func isHovered(_ row: Int) -> Bool {
        row == hoveredRow || isLit?(row) == true
    }

    /// Lights exactly the rows that should be lit, among those on screen.
    func refreshHover() {
        let visible = rows(in: visibleRect)
        for row in visible.location..<NSMaxRange(visible) {
            (rowView(atRow: row, makeIfNecessary: false) as? TreeRowView)?.isHovered =
                isHovered(row)
            (view(atColumn: 0, row: row, makeIfNecessary: false) as? TreeCell)?.showsActions =
                isUnderPointer(row)
        }
    }

    /// Whether the pointer itself is on a row, which shows its buttons, as a row lit from the
    /// view does not.
    func isUnderPointer(_ row: Int) -> Bool {
        row == hoveredRow
    }

    /// Works out the row under the pointer from where it really is, for when the rows moved under
    /// a still pointer: a scroll, or rows opening and closing.
    func syncPointer() {
        guard let window else {
            return
        }
        let point = convert(window.mouseLocationOutsideOfEventStream, from: nil)
        let inside = visibleRect.contains(point) && !isHiddenOrHasHiddenAncestor
        setHoveredRow(inside ? row(at: point) : -1)
    }

    /// Takes `row` as the row under the pointer, or none when negative.
    func setHoveredRow(_ row: Int) {
        let hovered = row >= 0 ? row : nil
        guard hovered != hoveredRow else {
            return
        }
        hoveredRow = hovered
        refreshHover()
        onHover?(hovered)
    }

    override func reloadData() {
        super.reloadData()
        syncPointer()
    }

    // A table keeps clicks on its rows' views for selecting rows, unless it is told a view takes
    // its own: the row buttons do.
    override func validateProposedFirstResponder(
        _ responder: NSResponder,
        for event: NSEvent?
    ) -> Bool {
        responder is ChromeControl || super.validateProposedFirstResponder(responder, for: event)
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking {
            removeTrackingArea(tracking)
        }
        let area = NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways, .inVisibleRect],
            owner: self
        )
        addTrackingArea(area)
        tracking = area
    }

    override func mouseMoved(with event: NSEvent) {
        setHoveredRow(row(at: convert(event.locationInWindow, from: nil)))
    }

    override func mouseExited(with event: NSEvent) {
        setHoveredRow(-1)
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        let row = self.row(at: point)
        if row >= 0, let cell = view(atColumn: 0, row: row, makeIfNecessary: false) as? TreeCell,
            cell.disclosureHit(convert(point, to: cell))
        {
            onToggle?(row)
            return
        }
        super.mouseDown(with: event)
    }
}
