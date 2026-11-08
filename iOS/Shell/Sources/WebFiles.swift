import Foundation

/// Where the shell's web files come from, shared by both ways of serving them: the pages' own files
/// from the app bundle, under /viewer/, /census/ and /spike/; generated test files from Caches,
/// under /files/; and documents the person opened in the shell, under /open/<id>/<name>. Nothing
/// else is served.
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
            let id = path.dropFirst("/open/".count).prefix(while: { $0 != "/" })
            return opened.url(for: String(id))
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

/// Documents opened in the shell from Files or the share sheet, each under a random id, so the page
/// can fetch exactly those and nothing else of the person's. Read from the server's queue and
/// written from the main thread, behind a lock.
final class OpenedFiles: @unchecked Sendable {
    private let lock = NSLock()
    private var urls: [String: URL] = [:]
    private var accessed: [URL] = []

    /// Keeps `url` readable for as long as the shell runs, and returns its id.
    func add(_ url: URL) -> String {
        let id = UUID().uuidString.lowercased()
        let scoped = url.startAccessingSecurityScopedResource()
        lock.lock()
        defer { lock.unlock() }
        urls[id] = url
        if scoped {
            accessed.append(url)
        }
        return id
    }

    func url(for id: String) -> URL? {
        lock.lock()
        defer { lock.unlock() }
        return urls[id]
    }
}
