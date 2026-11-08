import Foundation

/// Where the shell's web files come from, shared by both ways of serving them: the pages' own files
/// from the app bundle, under /census/ and /spike/, and generated test files from Caches, under
/// /files/. Nothing else in the bundle is served.
struct WebFiles: Sendable {
    let bundle: URL
    let files: URL

    init(bundle: URL, files: URL) {
        self.bundle = bundle.standardizedFileURL
        self.files = files.standardizedFileURL
    }

    /// The file a request path names, or nil if it names nothing inside the two roots.
    func resolve(_ path: String) -> URL? {
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

    private static let pages = ["census", "spike"]

    static func type(of file: URL) -> String {
        types[file.pathExtension] ?? "application/octet-stream"
    }

    private static let types: [String: String] = [
        "html": "text/html; charset=utf-8", "js": "text/javascript; charset=utf-8",
        "mjs": "text/javascript; charset=utf-8", "wasm": "application/wasm",
        "json": "application/json", "css": "text/css; charset=utf-8",
    ]

    /// The isolation headers every response carries, which WebAssembly threads require.
    static let isolation = [
        "Cross-Origin-Opener-Policy": "same-origin",
        "Cross-Origin-Embedder-Policy": "require-corp",
        "Cross-Origin-Resource-Policy": "same-origin",
        "Cache-Control": "no-store",
    ]
}
