import AppKit

/// A file or folder in a connected directory. Folders list their children only when first
/// opened, and nothing ever reads a file's contents, so a tree of a hundred thousand files costs
/// only the folders someone looks into.
@MainActor
final class FileNode {
    let url: URL
    let isDirectory: Bool
    weak var parent: FileNode?
    private var listed: [FileNode]?

    /// The file types the viewer opens.
    static let extensions: Set<String> = ["step", "stp", "stl"]

    init(url: URL, isDirectory: Bool, parent: FileNode?) {
        self.url = url
        self.isDirectory = isDirectory
        self.parent = parent
    }

    var name: String { url.lastPathComponent }

    /// The folder's subfolders and openable files, folders first, listed on first use.
    var children: [FileNode] {
        if let listed {
            return listed
        }
        let keys: [URLResourceKey] = [.isDirectoryKey]
        let entries =
            (try? FileManager.default.contentsOfDirectory(
                at: url,
                includingPropertiesForKeys: keys,
                options: [.skipsHiddenFiles]
            )) ?? []
        var folders: [FileNode] = []
        var files: [FileNode] = []
        for entry in entries {
            let directory = (try? entry.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory
            if directory == true {
                folders.append(FileNode(url: entry, isDirectory: true, parent: self))
            } else if Self.extensions.contains(entry.pathExtension.lowercased()) {
                files.append(FileNode(url: entry, isDirectory: false, parent: self))
            }
        }
        let byName: (FileNode, FileNode) -> Bool = {
            $0.name.localizedStandardCompare($1.name) == .orderedAscending
        }
        let all = folders.sorted(by: byName) + files.sorted(by: byName)
        listed = all
        return all
    }

    /// Forgets the listing, so it is read again when next needed.
    func invalidate() {
        listed = nil
    }

    /// The listed node at `path`, if it has been listed, searching down from here.
    func find(_ path: String) -> FileNode? {
        if url.path == path {
            return self
        }
        guard path.hasPrefix(url.path + "/"), let listed else {
            return nil
        }
        for child in listed where child.isDirectory || child.url.path == path {
            if let found = child.find(path) {
                return found
            }
        }
        return nil
    }
}

/// The folders the user has connected, remembered across launches with security-scoped
/// bookmarks, so access survives a relaunch of the sandboxed app.
@MainActor
final class LibraryStore {
    private(set) var roots: [FileNode] = []
    private static let key = "library.bookmarks"
    var onChange: (() -> Void)?

    init() {
        for data in UserDefaults.standard.array(forKey: Self.key) as? [Data] ?? [] {
            var stale = false
            guard
                let url = try? URL(
                    resolvingBookmarkData: data,
                    options: .withSecurityScope,
                    bookmarkDataIsStale: &stale
                ),
                url.startAccessingSecurityScopedResource()
            else {
                continue
            }
            roots.append(FileNode(url: url, isDirectory: true, parent: nil))
        }
        if roots.contains(where: { _ in true }) {
            save()  // refreshes any stale bookmark
        }
    }

    /// Connects a folder the user chose in an open panel.
    func connect(_ url: URL) {
        guard !roots.contains(where: { $0.url == url }) else {
            return
        }
        _ = url.startAccessingSecurityScopedResource()
        roots.append(FileNode(url: url, isDirectory: true, parent: nil))
        save()
        onChange?()
    }

    func disconnect(_ root: FileNode) {
        roots.removeAll { $0 === root }
        root.url.stopAccessingSecurityScopedResource()
        save()
        onChange?()
    }

    private func save() {
        let bookmarks = roots.compactMap {
            try? $0.url.bookmarkData(
                options: [.withSecurityScope, .securityScopeAllowOnlyReadAccess],
                includingResourceValuesForKeys: nil,
                relativeTo: nil
            )
        }
        UserDefaults.standard.set(bookmarks, forKey: Self.key)
    }
}

/// Watches folders for changes with FSEvents, and reports the folders whose contents changed.
final class DirectoryWatcher: @unchecked Sendable {
    private var stream: FSEventStreamRef?
    private let onChange: @MainActor ([String]) -> Void

    init(onChange: @escaping @MainActor ([String]) -> Void) {
        self.onChange = onChange
    }

    deinit {
        stop()
    }

    /// Watches these folders, and only these, from now on.
    func watch(_ paths: [String]) {
        stop()
        guard !paths.isEmpty else {
            return
        }
        var context = FSEventStreamContext(
            version: 0,
            info: Unmanaged.passUnretained(self).toOpaque(),
            retain: nil,
            release: nil,
            copyDescription: nil
        )
        let callback: FSEventStreamCallback = { _, info, count, paths, _, _ in
            guard let info else {
                return
            }
            let watcher = Unmanaged<DirectoryWatcher>.fromOpaque(info).takeUnretainedValue()
            let array = unsafeBitCast(paths, to: NSArray.self)
            let changed = (0..<count).compactMap { array[$0] as? String }
            let report = watcher.onChange
            DispatchQueue.main.async {
                MainActor.assumeIsolated { report(changed) }
            }
        }
        stream = FSEventStreamCreate(
            nil,
            callback,
            &context,
            paths as CFArray,
            FSEventStreamEventId(kFSEventStreamEventIdSinceNow),
            0.3,
            FSEventStreamCreateFlags(
                kFSEventStreamCreateFlagUseCFTypes | kFSEventStreamCreateFlagFileEvents
            )
        )
        if let stream {
            FSEventStreamSetDispatchQueue(stream, DispatchQueue.main)
            FSEventStreamStart(stream)
        }
    }

    func stop() {
        if let stream {
            FSEventStreamStop(stream)
            FSEventStreamInvalidate(stream)
            FSEventStreamRelease(stream)
        }
        stream = nil
    }
}
