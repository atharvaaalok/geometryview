import AppKit
import GeometryEngine

/// The assembly tree of everything open, with search. Selecting a row selects that part in the
/// view, and selecting in the view reveals its row.
@MainActor
final class StructureCard: StickerCard, NSTableViewDataSource, NSTableViewDelegate,
    NSSearchFieldDelegate
{
    private let engine: EngineHandle
    let tree = TreeRows()
    private let heading = NSTextField(labelWithString: "Structure")
    private let searchField = NSSearchField()
    private let matchLabel = NSTextField(labelWithString: "")
    private let table = TreeTableView()
    private let scroll = NSScrollView()
    private var applyingEngineSelection = false
    private var matches: [(document: UInt32, node: UInt32)] = []
    private var matchIndex = 0

    static let rowHeight: CGFloat = 24

    init(engine: EngineHandle) {
        self.engine = engine
        super.init(frame: .zero)

        heading.font = Typography.font(Typography.textLarge, .semibold)
        heading.textColor = Palette.ink

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
        table.selectionHighlightStyle = .none
        table.allowsMultipleSelection = false
        table.dataSource = self
        table.delegate = self
        table.target = self
        table.action = #selector(rowClicked)
        table.onHover = { [weak self] row in self?.hover(row) }
        table.onToggle = { [weak self] row in self?.toggle(row) }

        scroll.documentView = table
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.scrollerStyle = .overlay

        for view in [heading, searchField, matchLabel, scroll] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            addSubview(view)
        }
        let inset = Metrics.gap
        NSLayoutConstraint.activate([
            heading.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inset),
            heading.topAnchor.constraint(equalTo: topAnchor, constant: inset - 2),
            matchLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inset),
            matchLabel.firstBaselineAnchor.constraint(equalTo: heading.firstBaselineAnchor),
            searchField.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inset),
            searchField.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inset),
            searchField.topAnchor.constraint(equalTo: heading.bottomAnchor, constant: 10),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 6),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -6),
            scroll.topAnchor.constraint(equalTo: searchField.bottomAnchor, constant: 10),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -8),
        ])
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
            kind: tree.kind(of: row),
            isDocument: row.isDocument,
            disclosure: tree.hasChildren(row) ? (tree.isExpanded(row) ? .open : .closed) : .none
        )
        return cell
    }
}

/// A table that reports the row under the pointer and clicks on disclosure triangles.
@MainActor
final class TreeTableView: NSTableView {
    var onHover: ((Int?) -> Void)?
    var onToggle: ((Int) -> Void)?
    private var hoveredRow: Int?
    private var tracking: NSTrackingArea?

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking {
            removeTrackingArea(tracking)
        }
        let area = NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
            owner: self
        )
        addTrackingArea(area)
        tracking = area
    }

    override func mouseMoved(with event: NSEvent) {
        let row = self.row(at: convert(event.locationInWindow, from: nil))
        let hovered = row >= 0 ? row : nil
        if hovered != hoveredRow {
            hoveredRow = hovered
            onHover?(hovered)
        }
    }

    override func mouseExited(with event: NSEvent) {
        hoveredRow = nil
        onHover?(nil)
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

/// Draws selection as the cobalt wash, without the system's own highlight.
@MainActor
final class TreeRowView: NSTableRowView {
    override func drawSelection(in dirtyRect: NSRect) {
        Palette.cobaltWash.setFill()
        NSBezierPath(roundedRect: bounds.insetBy(dx: 4, dy: 1), xRadius: 6, yRadius: 6).fill()
    }
}

@MainActor
final class TreeCell: NSTableCellView {
    static let identifier = NSUserInterfaceItemIdentifier("TreeCell")

    enum Disclosure {
        case none, open, closed
    }

    private let label = NSTextField(labelWithString: "")
    private let marker = NSTextField(labelWithString: "")
    private let glyph = NSTextField(labelWithString: "")
    private var indent: NSLayoutConstraint?
    private static let indentStep: CGFloat = 14
    private static let markerWidth: CGFloat = 14

    init() {
        super.init(frame: .zero)
        identifier = Self.identifier
        for field in [marker, glyph, label] {
            field.translatesAutoresizingMaskIntoConstraints = false
            field.lineBreakMode = .byTruncatingTail
            addSubview(field)
        }
        marker.font = Typography.font(Typography.textSmall)
        marker.textColor = Palette.muted
        glyph.font = Typography.font(Typography.textExtraSmall)
        glyph.textColor = Palette.faint
        label.font = Typography.font(Typography.textMedium)
        label.textColor = Palette.ink
        label.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let indent = marker.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 8)
        self.indent = indent
        NSLayoutConstraint.activate([
            indent,
            marker.widthAnchor.constraint(equalToConstant: Self.markerWidth),
            marker.centerYAnchor.constraint(equalTo: centerYAnchor),
            glyph.leadingAnchor.constraint(equalTo: marker.trailingAnchor, constant: 2),
            glyph.centerYAnchor.constraint(equalTo: centerYAnchor),
            label.leadingAnchor.constraint(equalTo: glyph.trailingAnchor, constant: 6),
            label.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -8),
            label.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("TreeCell is not used from Interface Builder")
    }

    func configure(
        name: String,
        depth: Int,
        kind: GeoNodeKind?,
        isDocument: Bool,
        disclosure: Disclosure
    ) {
        indent?.constant = 8 + CGFloat(depth) * Self.indentStep
        label.stringValue = name
        label.font = Typography.font(Typography.textMedium, isDocument ? .semibold : .regular)
        switch disclosure {
        case .none: marker.stringValue = ""
        case .open: marker.stringValue = "▾"
        case .closed: marker.stringValue = "▸"
        }
        switch kind {
        case GeoNodeAssembly: glyph.stringValue = "◇"
        case GeoNodePart: glyph.stringValue = "□"
        case GeoNodeBody: glyph.stringValue = "▪"
        default: glyph.stringValue = isDocument ? "▤" : ""
        }
    }

    func disclosureHit(_ point: NSPoint) -> Bool {
        !marker.stringValue.isEmpty && marker.frame.insetBy(dx: -4, dy: -4).contains(point)
    }
}
