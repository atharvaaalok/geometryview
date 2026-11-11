import Foundation

/// Where the shell's web files come from, shared by both ways of serving them: the pages' own files
/// from the app bundle, under /viewer/, /census/ and /spike/; generated test files from Caches,
/// under /files/; documents the person opened in the shell, under /open/<id>/<name>; and the
/// folders they connected, a file in one under /open/<folder id>/<path inside it>, a folder's
/// listing under /list/<folder id>/<path inside it>, and a walk of one for the gallery under
/// /scan/<folder id>/<path inside it>. Nothing else is served, and nothing outside a connected
/// folder is reached through it.
struct WebFiles: Sendable {
    let bundle: URL
    let files: URL
    let opened: OpenedFiles

    init(bundle: URL, files: URL, opened: OpenedFiles) {
        self.bundle = bundle.standardizedFileURL
        self.files = files.standardizedFileURL
        self.opened = opened
    }

    /// The file a request path names, or nil if it names nothing inside the roots.
    func resolve(_ path: String) -> URL? {
        if path.hasPrefix("/open/") {
            let rest = path.dropFirst("/open/".count)
            let id = String(rest.prefix(while: { $0 != "/" }))
            guard let grant = opened.grant(for: id) else {
                return nil
            }
            guard grant.isFolder else {
                return grant.url
            }
            let inner = String(rest.dropFirst(min(rest.count, id.count + 1))).removingPercentEncoding ?? ""
            return Self.inside(grant.url, inner)
        }
        let path = path.hasSuffix("/") ? path + "index.html" : path
        let root: URL
        let relative: String
        if path.hasPrefix("/files/") {
            (root, relative) = (files, String(path.dropFirst("/files/".count)))
        } else if Self.pages.contains(where: { path.hasPrefix("/\($0)/") }) {
            (root, relative) = (bundle, String(path.dropFirst()))
        } else {
            return nil
        }
        let file = root.appendingPathComponent(relative).standardizedFileURL
        return file.path.hasPrefix(root.path + "/") ? file : nil
    }

    private static let pages = ["viewer", "census", "spike"]

    /// A connected folder's listing, as JSON [{name, kind}], kind "folder" or "file", names only and
    /// hidden ones left out: the answer to /list/<folder id>/<path inside it>.
    func listing(_ path: String) -> Data? {
        guard path.hasPrefix("/list/") else {
            return nil
        }
        let rest = path.dropFirst("/list/".count)
        let id = String(rest.prefix(while: { $0 != "/" }))
        guard let grant = opened.grant(for: id), grant.isFolder else {
            return nil
        }
        let inner = String(rest.dropFirst(min(rest.count, id.count + 1))).removingPercentEncoding ?? ""
        guard let folder = inner.isEmpty ? grant.url : Self.inside(grant.url, inner),
            let entries = try? FileManager.default.contentsOfDirectory(
                at: folder,
                includingPropertiesForKeys: [.isDirectoryKey],
                options: [.skipsHiddenFiles]
            )
        else {
            return nil
        }
        let items = entries.map { entry -> [String: String] in
            let folder = (try? entry.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true
            return ["name": entry.lastPathComponent, "kind": folder ? "folder" : "file"]
        }
        return try? JSONSerialization.data(withJSONObject: items)
    }

    /// A connected folder walked for the gallery, as NDJSON, the answer to
    /// /scan/<folder id>/<path inside it>?depth=<n>&limit=<n>: a line for each folder and each
    /// STEP or STL file under it, at most `depth` levels down (1 is the folder's own entries, 32 at
    /// most) and `limit` files (20,000 at most), then a last line with how many and whether a
    /// limit stopped the walk. One endpoint serves both the gallery's tree and its walk.
    ///
    ///   {"path": "motors/nema17.step", "kind": "file", "bytes": 120331, "modified": 1731000000000}
    ///   {"path": "motors", "kind": "folder"}
    ///   {"done": true, "files": 1, "folders": 1, "truncated": false}
    ///
    /// Paths are relative to the folder walked, and times in milliseconds since 1970. A file that
    /// lives only in iCloud is marked "cloud": reading it would download it, so the gallery imports
    /// it only when it is in view and the person says so. Hidden files and folders and the insides
    /// of packages are left out, and links are not followed.
    func scan(_ path: String, query: String?) -> Data? {
        guard path.hasPrefix("/scan/") else {
            return nil
        }
        let rest = path.dropFirst("/scan/".count)
        let id = String(rest.prefix(while: { $0 != "/" }))
        guard let grant = opened.grant(for: id), grant.isFolder else {
            return nil
        }
        let inner = String(rest.dropFirst(min(rest.count, id.count + 1))).removingPercentEncoding ?? ""
        guard let folder = inner.isEmpty ? grant.url : Self.inside(grant.url, inner) else {
            return nil
        }
        let items = URLComponents(string: "scan?" + (query ?? ""))?.queryItems ?? []
        let number = { (name: String, fallback: Int, most: Int) -> Int in
            let value = items.first(where: { $0.name == name })?.value.flatMap(Int.init) ?? fallback
            return min(max(value, 1), most)
        }
        let depth = number("depth", 32, 32)
        let limit = number("limit", 20_000, 20_000)
        let keys: [URLResourceKey] = [
            .isDirectoryKey, .isSymbolicLinkKey, .fileSizeKey, .contentModificationDateKey,
            .isUbiquitousItemKey, .ubiquitousItemDownloadingStatusKey,
        ]
        guard
            let walk = FileManager.default.enumerator(
                at: folder, includingPropertiesForKeys: keys,
                options: [.skipsHiddenFiles, .skipsPackageDescendants])
        else {
            return nil
        }
        let base = folder.standardizedFileURL.path + "/"
        var out = Data()
        var files = 0
        var folders = 0
        var truncated = false
        let line = { (object: [String: Any]) in
            if let json = try? JSONSerialization.data(
                withJSONObject: object, options: [.sortedKeys, .withoutEscapingSlashes]) {
                out.append(json)
                out.append(0x0A)
            }
        }
        while let entry = walk.nextObject() as? URL {
            if walk.level > depth {
                walk.skipDescendants()
                truncated = true
                continue
            }
            guard let values = try? entry.resourceValues(forKeys: Set(keys)),
                values.isSymbolicLink != true
            else {
                continue
            }
            let full = entry.standardizedFileURL.path
            let relative = full.hasPrefix(base) ? String(full.dropFirst(base.count)) : entry.lastPathComponent
            if values.isDirectory == true {
                if walk.level == depth {
                    walk.skipDescendants()
                }
                line(["path": relative, "kind": "folder"])
                folders += 1
                continue
            }
            guard Self.modelExtensions.contains(entry.pathExtension.lowercased()) else {
                continue
            }
            if files == limit {
                truncated = true
                break
            }
            var object: [String: Any] = [
                "path": relative, "kind": "file", "bytes": values.fileSize ?? 0,
                "modified": Int64((values.contentModificationDate?.timeIntervalSince1970 ?? 0) * 1000),
            ]
            if values.isUbiquitousItem == true && values.ubiquitousItemDownloadingStatus != .current {
                object["cloud"] = true
            }
            line(object)
            files += 1
        }
        line(["done": true, "files": files, "folders": folders, "truncated": truncated])
        return out
    }

    private static let modelExtensions: Set<String> = ["step", "stp", "stl"]

    /// `inner` within `root`, or nil if it would lead outside it.
    static func inside(_ root: URL, _ inner: String) -> URL? {
        let base = root.standardizedFileURL
        let file = base.appendingPathComponent(inner).standardizedFileURL
        return file.path.hasPrefix(base.path + "/") ? file : nil
    }

    /// When a person's file last changed, in milliseconds since 1970, which the page compares with
    /// what it kept of the file: sent as GeometryView-Modified.
    static func modified(_ file: URL) -> String? {
        (try? file.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate)
            .map { String(Int64($0.timeIntervalSince1970 * 1000)) }
    }

    static func type(of file: URL) -> String {
        types[file.pathExtension.lowercased()] ?? "application/octet-stream"
    }

    private static let types: [String: String] = [
        "html": "text/html; charset=utf-8", "js": "text/javascript; charset=utf-8",
        "mjs": "text/javascript; charset=utf-8", "wasm": "application/wasm",
        "json": "application/json", "css": "text/css; charset=utf-8", "ttf": "font/ttf",
        "svg": "image/svg+xml", "png": "image/png",
    ]

    /// The isolation headers every response carries, which WebAssembly threads require.
    static let isolation = [
        "Cross-Origin-Opener-Policy": "same-origin",
        "Cross-Origin-Embedder-Policy": "require-corp",
        "Cross-Origin-Resource-Policy": "same-origin",
        "Cache-Control": "no-store",
    ]
}

/// What of the person's the shell may serve: documents opened from Files, the share sheet or the
/// picker, and connected folders, each under a random id, so the page can fetch exactly those and
/// nothing else. Read from the server's queue and written from the main thread, behind a lock.
final class OpenedFiles: @unchecked Sendable {
    struct Grant {
        let url: URL
        let isFolder: Bool
    }

    private let lock = NSLock()
    private var grants: [String: Grant] = [:]

    /// Keeps `url` readable for as long as the shell runs, and returns its id.
    func add(_ url: URL) -> String {
        let id = UUID().uuidString.lowercased()
        _ = url.startAccessingSecurityScopedResource()
        lock.lock()
        defer { lock.unlock() }
        grants[id] = Grant(url: url, isFolder: false)
        return id
    }

    /// Serves a connected folder under `id`, its access already started.
    func addFolder(_ url: URL, id: String) {
        lock.lock()
        defer { lock.unlock() }
        grants[id] = Grant(url: url, isFolder: true)
    }

    func remove(_ id: String) {
        lock.lock()
        defer { lock.unlock() }
        grants[id] = nil
    }

    func grant(for id: String) -> Grant? {
        lock.lock()
        defer { lock.unlock() }
        return grants[id]
    }

    func url(for id: String) -> URL? {
        grant(for: id)?.url
    }
}
