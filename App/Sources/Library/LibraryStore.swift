import AppKit

/// A file or folder in a connected directory. A folder lists its children when it is opened, off
/// the main thread, by names and kinds alone; nothing reads a file's contents, so a tree of a
/// hundred thousand files costs only the folders someone looks into.
@MainActor
final class FileNode {
    enum Listing {
        case unlisted, reading, listed, failed
    }

    let url: URL
    let isDirectory: Bool
    weak var parent: FileNode?
    /// A connected folder whose bookmark no longer leads anywhere.
    let isMissing: Bool
    private(set) var listing = Listing.unlisted
    /// Set once a listing has taken long enough to say "Reading…".
    private(set) var isSlow = false
    private(set) var children: [FileNode] = []

    /// The file types the viewer opens.
    nonisolated static let extensions: Set<String> = ["step", "stp", "stl"]

    init(url: URL, isDirectory: Bool, parent: FileNode?, isMissing: Bool = false) {
        self.url = url
        self.isDirectory = isDirectory
        self.parent = parent
        self.isMissing = isMissing
    }

    var name: String { url.lastPathComponent }

    /// Lists the folder afresh, folders first, then the files the viewer opens, each in the order
    /// Finder sorts names; `changed` runs on the main thread as the listing moves along.
    func list(changed: @escaping @MainActor () -> Void) {
        guard isDirectory, !isMissing else {
            return
        }
        listing = .reading
        isSlow = false
        let url = url
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(150))
            if let self, listing == .reading {
                isSlow = true
                changed()
            }
        }
        Task.detached(priority: .userInitiated) {
            let found = Self.read(url)
            await MainActor.run { [weak self] in
                guard let self else {
                    return
                }
                if let found {
                    children = found.map { FileNode(url: $0.url, isDirectory: $0.isDirectory, parent: self) }
                    listing = .listed
                } else {
                    children = []
                    listing = .failed
                }
                changed()
            }
        }
    }

    private nonisolated static func read(_ url: URL) -> [(url: URL, isDirectory: Bool)]? {
        guard
            let entries = try? FileManager.default.contentsOfDirectory(
                at: url,
                includingPropertiesForKeys: [.isDirectoryKey],
                options: [.skipsHiddenFiles]
            )
        else {
            return nil
        }
        var folders: [(url: URL, isDirectory: Bool)] = []
        var files: [(url: URL, isDirectory: Bool)] = []
        for entry in entries {
            if (try? entry.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true {
                folders.append((entry, true))
            } else if extensions.contains(entry.pathExtension.lowercased()) {
                files.append((entry, false))
            }
        }
        let byName = { (a: (url: URL, isDirectory: Bool), b: (url: URL, isDirectory: Bool)) in
            a.url.lastPathComponent.localizedStandardCompare(b.url.lastPathComponent) == .orderedAscending
        }
        return folders.sorted(by: byName) + files.sorted(by: byName)
    }

    /// The listed node at `path`, if it has been listed, searching down from here.
    func find(_ path: String) -> FileNode? {
        if url.path == path {
            return self
        }
        guard path.hasPrefix(url.path + "/"), listing == .listed else {
            return nil
        }
        for child in children where child.isDirectory || child.url.path == path {
            if let found = child.find(path) {
                return found
            }
        }
        return nil
    }
}

/// A file opened before, to open again from Recent: its security-scoped bookmark carries the
/// sandbox's permission to read it across launches.
struct RecentFile: Codable, Equatable {
    var path: String
    var name: String
    var openedAt: Date
    var bookmark: Data
}

/// The folders the user has connected, and the files opened recently, remembered across launches
/// with security-scoped bookmarks, so access survives a relaunch of the sandboxed app.
@MainActor
final class LibraryStore {
    private(set) var roots: [FileNode] = []
    private(set) var recent: [RecentFile] = []
    private var bookmarks: [Data] = []
    private static let key = "library.bookmarks"
    private static let recentKey = "library.recent"
    static let recentLimit = 12
    var onChange: (() -> Void)?

    init() {
        for data in UserDefaults.standard.array(forKey: Self.key) as? [Data] ?? [] {
            var stale = false
            if let url = try? URL(
                resolvingBookmarkData: data,
                options: [.withSecurityScope, .withoutUI, .withoutMounting],
                bookmarkDataIsStale: &stale
            ), url.startAccessingSecurityScopedResource() {
                roots.append(FileNode(url: url, isDirectory: true, parent: nil))
                bookmarks.append(Self.bookmark(url) ?? data)
            } else {
                // Kept, and shown as missing, until the person disconnects it.
                let name =
                    URL.resourceValues(forKeys: [.nameKey], fromBookmarkData: data)?.name ?? "Folder"
                roots.append(
                    FileNode(url: URL(fileURLWithPath: "/\(name)"), isDirectory: true, parent: nil, isMissing: true)
                )
                bookmarks.append(data)
            }
        }
        saveFolders()
        if let data = UserDefaults.standard.data(forKey: Self.recentKey),
            let saved = try? JSONDecoder().decode([RecentFile].self, from: data)
        {
            recent = saved
        }
    }

    private static func bookmark(_ url: URL) -> Data? {
        try? url.bookmarkData(
            options: [.withSecurityScope, .securityScopeAllowOnlyReadAccess],
            includingResourceValuesForKeys: nil,
            relativeTo: nil
        )
    }

    /// Connects a folder the user chose or dropped. Returns its node.
    @discardableResult
    func connect(_ url: URL) -> FileNode {
        if let existing = roots.first(where: { $0.url == url }) {
            return existing
        }
        _ = url.startAccessingSecurityScopedResource()
        let node = FileNode(url: url, isDirectory: true, parent: nil)
        roots.append(node)
        bookmarks.append(Self.bookmark(url) ?? Data())
        saveFolders()
        onChange?()
        return node
    }

    func disconnect(_ root: FileNode) {
        guard let index = roots.firstIndex(where: { $0 === root }) else {
            return
        }
        roots.remove(at: index)
        bookmarks.remove(at: index)
        if !root.isMissing {
            root.url.stopAccessingSecurityScopedResource()
        }
        saveFolders()
        onChange?()
    }

    private func saveFolders() {
        UserDefaults.standard.set(bookmarks.filter { !$0.isEmpty }, forKey: Self.key)
    }

    // MARK: Recent

    /// Puts a file that opened at the top of Recent, while the app can still read it.
    func remember(_ url: URL) {
        guard let bookmark = Self.bookmark(url) else {
            return
        }
        recent.removeAll { $0.path == url.path }
        recent.insert(RecentFile(path: url.path, name: url.lastPathComponent, openedAt: Date(), bookmark: bookmark), at: 0)
        recent = Array(recent.prefix(Self.recentLimit))
        saveRecent()
        onChange?()
    }

    func forget(_ file: RecentFile) {
        recent.removeAll { $0.path == file.path }
        saveRecent()
        onChange?()
    }

    /// Where a recent file is now, with the sandbox's permission to read it, or nil if it is gone.
    func resolve(_ file: RecentFile) -> URL? {
        var stale = false
        guard
            let url = try? URL(
                resolvingBookmarkData: file.bookmark,
                options: [.withSecurityScope, .withoutUI, .withoutMounting],
                bookmarkDataIsStale: &stale
            ), FileManager.default.fileExists(atPath: url.path)
        else {
            return nil
        }
        return url
    }

    private func saveRecent() {
        UserDefaults.standard.set(try? JSONEncoder().encode(recent), forKey: Self.recentKey)
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
