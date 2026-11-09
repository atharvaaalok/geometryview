import Foundation

/// The Library's two sections.
enum LibrarySection: String, CaseIterable {
    case folders, recent
}

/// A row of the Library: a section's heading, a folder or a file in one, a recent file, or a line
/// of text where rows would be.
struct LibraryRow {
    enum Kind {
        case section, folder, file, recent, message
    }

    var kind: Kind
    var depth: Int
    var name = ""
    /// The text at the row's end: a count, how long ago, or a folder's state.
    var end = ""
    var isExpanded = false
    /// A file shown now.
    var isOpen = false
    var node: FileNode?
    var recent: RecentFile?
    var section: LibrarySection?
}

/// What the Library lists, as rows: the connected folders, each a tree read a level at a time as
/// it is opened, then the files opened recently. The web viewer's library-rows.js does the same.
@MainActor
enum LibraryRows {
    /// `expanded` holds the paths of the folders shown open, `open` the paths of the files shown.
    static func rows(
        roots: [FileNode],
        expanded: Set<String>,
        collapsed: Set<LibrarySection>,
        recent: [RecentFile],
        open: Set<String>,
        now: Date = Date()
    ) -> [LibraryRow] {
        var rows: [LibraryRow] = []
        let foldersShown = !collapsed.contains(.folders)
        rows.append(
            LibraryRow(kind: .section, depth: 0, name: "Folders", end: "\(roots.count)", isExpanded: foldersShown,
                       section: .folders))
        if foldersShown {
            if roots.isEmpty {
                rows.append(LibraryRow(kind: .message, depth: 1, name: "Connect a folder to browse it."))
            }
            for root in roots {
                let isOpen = !root.isMissing && expanded.contains(root.url.path)
                rows.append(
                    LibraryRow(kind: .folder, depth: 1, name: root.name, end: root.isMissing ? "Missing" : "",
                               isExpanded: isOpen, node: root))
                if isOpen {
                    addListing(of: root, depth: 2, expanded: expanded, open: open, into: &rows)
                }
            }
        }
        let recentShown = !collapsed.contains(.recent)
        rows.append(
            LibraryRow(kind: .section, depth: 0, name: "Recent", end: "\(recent.count)", isExpanded: recentShown,
                       section: .recent))
        if recentShown {
            if recent.isEmpty {
                rows.append(LibraryRow(kind: .message, depth: 1, name: "Files you open appear here."))
            }
            for file in recent {
                rows.append(
                    LibraryRow(kind: .recent, depth: 1, name: file.name, end: age(file.openedAt, now: now),
                               isOpen: open.contains(file.path), recent: file))
            }
        }
        return rows
    }

    private static func addListing(
        of folder: FileNode,
        depth: Int,
        expanded: Set<String>,
        open: Set<String>,
        into rows: inout [LibraryRow]
    ) {
        switch folder.listing {
        case .unlisted, .reading:
            if folder.isSlow {
                rows.append(LibraryRow(kind: .message, depth: depth, name: "Reading…"))
            }
            return
        case .failed:
            rows.append(LibraryRow(kind: .message, depth: depth, name: "This folder could not be read."))
            return
        case .listed:
            break
        }
        if folder.children.isEmpty {
            rows.append(LibraryRow(kind: .message, depth: depth, name: "No STEP or STL files here."))
        }
        for child in folder.children {
            if child.isDirectory {
                let isOpen = expanded.contains(child.url.path)
                rows.append(LibraryRow(kind: .folder, depth: depth, name: child.name, isExpanded: isOpen, node: child))
                if isOpen {
                    addListing(of: child, depth: depth + 1, expanded: expanded, open: open, into: &rows)
                }
            } else {
                rows.append(
                    LibraryRow(kind: .file, depth: depth, name: child.name, isOpen: open.contains(child.url.path),
                               node: child))
            }
        }
    }

    /// How long ago, in a few characters: "now", "5 min", "3 h", "2 d".
    static func age(_ date: Date, now: Date = Date()) -> String {
        let minutes = Int(max(0, now.timeIntervalSince(date)) / 60)
        if minutes < 1 {
            return "now"
        }
        if minutes < 60 {
            return "\(minutes) min"
        }
        let hours = minutes / 60
        return hours < 24 ? "\(hours) h" : "\(hours / 24) d"
    }
}
