import AppKit
import GeometryViewEngine

/// The Library: the switcher and the two ways in, opening files and connecting a folder, then the
/// connected folders and the recent files, as rows like the structure tree's. A click on a file
/// opens it alone; Command-click adds it to what is shown. The web viewer's Library is the same.
@MainActor
final class LibraryCard: StickerCard, NSTableViewDataSource, NSTableViewDelegate {
    let switcher = SegmentedPill(
        ["Library", "Structure"],
        selected: 0,
        help: "Show files or parts"
    )
    private let store: LibraryStore
    private let table = TreeTableView()
    private let scroll = NSScrollView()
    private let add = PillButton(icon: .plus, help: "Open files (⌘O)")
    private let connect = PillButton(icon: .folderPlus, help: "Connect a folder (⌥⌘O)")
    private var rows: [LibraryRow] = []
    /// The paths of the folders shown open.
    private var expanded: Set<String> = []
    private var collapsed: Set<LibrarySection>
    private static let collapsedKey = "library.collapsed"

    /// The paths of the files shown now, marked in the rows.
    var openPaths: Set<String> = [] {
        didSet {
            if openPaths != oldValue {
                reload()
            }
        }
    }

    var onOpen: ((URL, _ adding: Bool) -> Void)?
    var onReopen: ((RecentFile, _ adding: Bool) -> Void)?
    var onChoose: (() -> Void)?
    var onConnect: (() -> Void)?
    /// A folder was clicked: its gallery shows, the connected folder it is in first.
    var onShowFolder: ((_ root: URL, _ folder: URL) -> Void)?
    /// Called with the search typed while the gallery shows, and when Esc leaves the field.
    var onSearch: ((String) -> Void)?
    var onSearchDone: (() -> Void)?
    /// Searches the folder the gallery shows: only while it shows. Typing in the gallery searches
    /// too.
    private let searchBox = SearchBox(placeholder: "Search this folder")
    private var listBelowSwitcher: NSLayoutConstraint!
    private var listBelowSearch: NSLayoutConstraint!

    init(store: LibraryStore) {
        self.store = store
        collapsed = Set(
            (UserDefaults.standard.stringArray(forKey: Self.collapsedKey) ?? []).compactMap(LibrarySection.init)
        )
        super.init()

        let column = NSTableColumn(identifier: .init("name"))
        table.addTableColumn(column)
        table.headerView = nil
        table.rowHeight = StructureCard.rowHeight
        table.intercellSpacing = .zero
        table.backgroundColor = .clear
        table.style = .plain
        table.dataSource = self
        table.delegate = self
        table.target = self
        table.action = #selector(rowClicked)
        table.onToggle = { [weak self] row in self?.toggle(row) }
        table.menu = NSMenu()
        table.menu?.delegate = self

        scroll.documentView = table
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.scrollerStyle = .overlay

        add.onPress = { [weak self] in self?.onChoose?() }
        connect.onPress = { [weak self] in self?.onConnect?() }

        searchBox.field.delegate = self
        searchBox.isHidden = true
        ChromeLayout.name(searchBox, "library.search")
        for view in [switcher, add, connect, searchBox, scroll] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            addSubview(view)
        }
        let inset = Metrics.gap
        listBelowSwitcher = scroll.topAnchor.constraint(
            equalTo: switcher.bottomAnchor,
            constant: 10
        )
        listBelowSearch = scroll.topAnchor.constraint(
            equalTo: searchBox.bottomAnchor,
            constant: 10
        )
        NSLayoutConstraint.activate([
            searchBox.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inset),
            searchBox.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inset),
            searchBox.topAnchor.constraint(equalTo: switcher.bottomAnchor, constant: 10),
            switcher.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inset),
            switcher.topAnchor.constraint(equalTo: topAnchor, constant: inset - 4),
            connect.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inset),
            connect.centerYAnchor.constraint(equalTo: switcher.centerYAnchor),
            add.trailingAnchor.constraint(equalTo: connect.leadingAnchor, constant: -8),
            add.centerYAnchor.constraint(equalTo: switcher.centerYAnchor),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 6),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -6),
            listBelowSwitcher,
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -8),
        ])
        reload()
    }

    /// Shows the gallery's search, with its query and how many files it found (nil: no search).
    func showSearch(_ shown: Bool, query: String = "", found: UInt32? = nil) {
        searchBox.isHidden = !shown
        listBelowSwitcher.isActive = !shown
        listBelowSearch.isActive = shown
        // The field keeps what is typed in it; typing in the gallery shows there too.
        if searchBox.field.currentEditor() == nil, searchBox.field.stringValue != query {
            searchBox.field.stringValue = query
        }
        searchBox.match.stringValue = found.map { "\($0) found" } ?? ""
    }

    /// Shows the store's folders and recent files afresh.
    func reload() {
        rows = LibraryRows.rows(
            roots: store.roots,
            expanded: expanded,
            collapsed: collapsed,
            recent: store.recent,
            open: openPaths
        )
        table.reloadData()
    }

    /// Opens a folder's row, listing it.
    func expand(_ node: FileNode) {
        expanded.insert(node.url.path)
        node.list { [weak self] in self?.reload() }
        reload()
    }

    /// Lists a folder again after it changed, if it is open.
    func refresh(_ node: FileNode) {
        guard expanded.contains(node.url.path) else {
            return
        }
        node.list { [weak self] in self?.reload() }
    }

    // MARK: Clicks

    @objc private func rowClicked() {
        let index = table.clickedRow
        guard rows.indices.contains(index) else {
            return
        }
        let row = rows[index]
        let adding = NSApp.currentEvent?.modifierFlags.contains(.command) == true
        switch row.kind {
        case .section:
            toggle(index)
        case .folder:
            toggle(index)
            if let node = row.node, !node.isMissing {
                var top = node
                while let parent = top.parent {
                    top = parent
                }
                onShowFolder?(top.url, node.url)
            }
        case .file:
            if let node = row.node {
                onOpen?(node.url, adding)
            }
        case .recent:
            if let file = row.recent {
                onReopen?(file, adding)
            }
        case .message:
            break
        }
    }

    private func toggle(_ index: Int) {
        guard rows.indices.contains(index) else {
            return
        }
        let row = rows[index]
        if let section = row.section {
            if collapsed.contains(section) {
                collapsed.remove(section)
            } else {
                collapsed.insert(section)
            }
            UserDefaults.standard.set(collapsed.map(\.rawValue), forKey: Self.collapsedKey)
            reload()
        } else if let node = row.node, row.kind == .folder, !node.isMissing {
            // A folder is listed each time it opens, so opening it again shows what changed.
            if expanded.contains(node.url.path) {
                expanded.remove(node.url.path)
                reload()
            } else {
                expand(node)
            }
        }
    }

    fileprivate func remove(_ row: LibraryRow) {
        if let file = row.recent {
            store.forget(file)
        } else if let node = row.node, node.parent == nil {
            expanded = expanded.filter { !$0.hasPrefix(node.url.path) }
            store.disconnect(node)
        }
    }

    // MARK: NSTableViewDataSource and NSTableViewDelegate

    func numberOfRows(in tableView: NSTableView) -> Int {
        rows.count
    }

    func tableView(_ tableView: NSTableView, shouldSelectRow row: Int) -> Bool {
        false
    }

    func tableView(_ tableView: NSTableView, rowViewForRow row: Int) -> NSTableRowView? {
        TreeRowView()
    }

    func tableView(_ tableView: NSTableView, didAdd rowView: NSTableRowView, forRow row: Int) {
        guard let view = rowView as? TreeRowView, rows.indices.contains(row) else {
            return
        }
        view.isHovered = table.isHovered(row)
        view.isChosen = rows[row].isOpen
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row index: Int) -> NSView? {
        let cell = tableView.makeView(withIdentifier: TreeCell.identifier, owner: self) as? TreeCell ?? TreeCell()
        let row = rows[index]
        let disclosure: TreeCell.Disclosure = row.isExpanded ? .open : .closed
        switch row.kind {
        case .section:
            cell.configure(
                name: row.name, depth: row.depth, mark: .none, isDocument: true, disclosure: disclosure,
                count: row.end, state: nil, actions: [])
        case .folder:
            let missing = row.node?.isMissing == true
            let top = row.node?.parent == nil
            cell.configure(
                name: row.name, depth: row.depth, mark: .folder, isDocument: false,
                disclosure: missing ? .none : disclosure, count: row.end, state: nil,
                actions: top ? [.remove] : [], tone: missing ? .dimmed : .normal, removeHelp: "Disconnect")
        case .file:
            cell.configure(
                name: row.name, depth: row.depth, mark: .file, isDocument: false, disclosure: .none,
                count: "", state: nil, actions: [])
        case .recent:
            cell.configure(
                name: row.name, depth: row.depth, mark: .file, isDocument: false, disclosure: .none,
                count: row.end, state: nil, actions: [.remove], removeHelp: "Remove from Recent")
        case .message:
            cell.configure(
                name: row.name, depth: row.depth, mark: .blank, isDocument: false, disclosure: .none,
                count: "", state: nil, actions: [], tone: .message)
        }
        cell.toolTip = row.recent?.path ?? row.node?.url.path
        cell.onAction = { [weak self] action in
            if action == .remove {
                self?.remove(row)
            }
        }
        cell.showsActions = table.isUnderPointer(index)
        return cell
    }
}

extension LibraryCard: NSMenuDelegate {
    // A row's menu: disconnect a folder or forget a recent file, and show either in Finder.
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        guard rows.indices.contains(table.clickedRow) else {
            return
        }
        let row = rows[table.clickedRow]
        if let node = row.node, node.parent == nil {
            menu.addItem(item("Disconnect \(node.name)", #selector(removeChosen(_:)), RowBox(row)))
        }
        if row.recent != nil {
            menu.addItem(item("Remove from Recent", #selector(removeChosen(_:)), RowBox(row)))
        }
        let url = row.node.flatMap { $0.isMissing ? nil : $0.url } ?? row.recent.flatMap { store.resolve($0) }
        if let url {
            menu.addItem(item("Show in Finder", #selector(showInFinder(_:)), url))
        }
    }

    private func item(_ title: String, _ action: Selector, _ object: Any) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: "")
        item.target = self
        item.representedObject = object
        return item
    }

    @objc private func removeChosen(_ sender: NSMenuItem) {
        if let box = sender.representedObject as? RowBox {
            remove(box.row)
        }
    }

    @objc private func showInFinder(_ sender: NSMenuItem) {
        if let url = sender.representedObject as? URL {
            NSWorkspace.shared.activateFileViewerSelecting([url])
        }
    }
}

/// A row, as a menu item's object.
@MainActor
private final class RowBox: NSObject {
    let row: LibraryRow
    init(_ row: LibraryRow) {
        self.row = row
    }
}

extension LibraryCard: NSTextFieldDelegate {
    func controlTextDidChange(_ notification: Notification) {
        guard notification.object as? NSTextField === searchBox.field else {
            return
        }
        onSearch?(searchBox.field.stringValue)
    }

    func controlTextDidEndEditing(_ notification: Notification) {
        if notification.object as? NSTextField === searchBox.field {
            searchBox.editingEnded()
        }
    }

    /// Esc in the field clears the search and gives the keys back to the gallery.
    func control(
        _ control: NSControl,
        textView: NSTextView,
        doCommandBy selector: Selector
    ) -> Bool {
        guard control === searchBox.field,
            selector == #selector(NSResponder.cancelOperation(_:))
        else {
            return false
        }
        searchBox.field.stringValue = ""
        onSearch?("")
        window?.makeFirstResponder(nil)
        onSearchDone?()
        return true
    }
}
