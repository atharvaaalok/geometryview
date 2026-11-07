import AppKit

/// The connected folders as a tree of their STEP and STL files. Clicking a file opens it alone;
/// Command-clicking adds it to what is shown.
@MainActor
final class LibraryCard: StickerCard, NSOutlineViewDataSource, NSOutlineViewDelegate {
    let switcher = SegmentedPill(
        ["Library", "Structure"],
        selected: 0,
        help: "Show files or parts"
    )
    private let store: LibraryStore
    private let outline = NSOutlineView()
    private let scroll = NSScrollView()
    private let connect = PillButton(symbol: "plus", help: "Connect a folder")
    private let empty = NSTextField(
        wrappingLabelWithString: "Connect a folder to browse its STEP and STL files."
    )

    var onOpen: ((URL, _ adding: Bool) -> Void)?
    var onConnect: (() -> Void)?

    init(store: LibraryStore) {
        self.store = store
        super.init(frame: .zero)

        let column = NSTableColumn(identifier: .init("name"))
        outline.addTableColumn(column)
        outline.outlineTableColumn = column
        outline.headerView = nil
        outline.rowHeight = StructureCard.rowHeight
        outline.intercellSpacing = .zero
        outline.backgroundColor = .clear
        outline.style = .plain
        outline.indentationPerLevel = 14
        outline.dataSource = self
        outline.delegate = self
        outline.target = self
        outline.action = #selector(rowClicked)
        outline.menu = NSMenu()
        outline.menu?.delegate = self

        scroll.documentView = outline
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.scrollerStyle = .overlay

        empty.font = Typography.font(Typography.textSmall)
        empty.textColor = Palette.muted
        connect.onPress = { [weak self] in self?.onConnect?() }

        for view in [switcher, connect, scroll, empty] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            addSubview(view)
        }
        let inset = Metrics.gap
        NSLayoutConstraint.activate([
            switcher.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inset),
            switcher.topAnchor.constraint(equalTo: topAnchor, constant: inset - 4),
            connect.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inset),
            connect.centerYAnchor.constraint(equalTo: switcher.centerYAnchor),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 6),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -6),
            scroll.topAnchor.constraint(equalTo: switcher.bottomAnchor, constant: 10),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -8),
            empty.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inset),
            empty.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inset),
            empty.topAnchor.constraint(equalTo: switcher.bottomAnchor, constant: 14),
        ])
        reload()
    }

    /// Shows the store's folders afresh.
    func reload() {
        outline.reloadData()
        empty.isHidden = !store.roots.isEmpty
    }

    /// Lists a folder again after it changed, keeping what is open.
    func refresh(_ node: FileNode) {
        node.invalidate()
        outline.reloadItem(node, reloadChildren: true)
    }

    @objc private func rowClicked() {
        guard let node = outline.item(atRow: outline.clickedRow) as? FileNode else {
            return
        }
        if node.isDirectory {
            if outline.isItemExpanded(node) {
                outline.collapseItem(node)
            } else {
                outline.expandItem(node)
            }
            return
        }
        let adding = NSApp.currentEvent?.modifierFlags.contains(.command) == true
        onOpen?(node.url, adding)
    }

    // MARK: NSOutlineViewDataSource

    func outlineView(_ outlineView: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int {
        guard let node = item as? FileNode else {
            return store.roots.count
        }
        return node.children.count
    }

    func outlineView(_ outlineView: NSOutlineView, child index: Int, ofItem item: Any?) -> Any {
        guard let node = item as? FileNode else {
            return store.roots[index]
        }
        return node.children[index]
    }

    func outlineView(_ outlineView: NSOutlineView, isItemExpandable item: Any) -> Bool {
        (item as? FileNode)?.isDirectory == true
    }

    // MARK: NSOutlineViewDelegate

    func outlineView(_ outlineView: NSOutlineView, viewFor tableColumn: NSTableColumn?, item: Any)
        -> NSView?
    {
        guard let node = item as? FileNode else {
            return nil
        }
        let identifier = NSUserInterfaceItemIdentifier("file")
        let cell =
            outlineView.makeView(withIdentifier: identifier, owner: self) as? NSTableCellView
            ?? makeCell(identifier)
        cell.textField?.stringValue = node.name
        cell.imageView?.image = NSImage(
            systemSymbolName: node.isDirectory ? "folder" : "cube",
            accessibilityDescription: nil
        )
        return cell
    }

    private func makeCell(_ identifier: NSUserInterfaceItemIdentifier) -> NSTableCellView {
        let cell = NSTableCellView()
        cell.identifier = identifier
        let icon = NSImageView()
        icon.contentTintColor = Palette.muted
        let label = NSTextField(labelWithString: "")
        label.font = Typography.font(Typography.textMedium)
        label.textColor = Palette.ink
        label.lineBreakMode = .byTruncatingMiddle
        for view in [icon, label] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            cell.addSubview(view)
        }
        NSLayoutConstraint.activate([
            icon.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 2),
            icon.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
            icon.widthAnchor.constraint(equalToConstant: 14),
            label.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 6),
            label.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -4),
            label.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
        ])
        cell.imageView = icon
        cell.textField = label
        return cell
    }
}

extension LibraryCard: NSMenuDelegate {
    // A connected folder's menu offers to disconnect it.
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        guard let node = outline.item(atRow: outline.clickedRow) as? FileNode,
            node.parent == nil
        else {
            return
        }
        let item = NSMenuItem(
            title: "Disconnect \(node.name)",
            action: #selector(disconnect(_:)),
            keyEquivalent: ""
        )
        item.target = self
        item.representedObject = node
        menu.addItem(item)
    }

    @objc private func disconnect(_ sender: NSMenuItem) {
        if let node = sender.representedObject as? FileNode {
            store.disconnect(node)
        }
    }
}
