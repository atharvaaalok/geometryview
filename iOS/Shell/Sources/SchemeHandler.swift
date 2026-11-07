import Foundation
import WebKit

/// Serves the bundled web files at geo://local/..., with the headers the viewer needs: the right
/// media types, and cross-origin isolation, which WebAssembly threads require.
///
/// WebKit calls this on the main thread, and the census files are small, so it answers there, in
/// 1 MB chunks. Serving large model files will need reading off the main thread.
@MainActor
final class SchemeHandler: NSObject, WKURLSchemeHandler {
    static let scheme = "geo"

    private let root: URL
    // Tasks WebKit has stopped. Answering one of those crashes, so every answer checks first.
    private var stopped = Set<ObjectIdentifier>()

    init(root: URL) {
        self.root = root.standardizedFileURL
    }

    private static let types: [String: String] = [
        "html": "text/html; charset=utf-8", "js": "text/javascript; charset=utf-8",
        "mjs": "text/javascript; charset=utf-8", "wasm": "application/wasm",
        "json": "application/json", "css": "text/css; charset=utf-8",
    ]

    func webView(_ webView: WKWebView, start task: any WKURLSchemeTask) {
        let id = ObjectIdentifier(task)
        stopped.remove(id)
        guard let url = task.request.url else {
            return
        }
        let path = url.path.isEmpty || url.path == "/" ? "/index.html" : url.path
        let file = root.appendingPathComponent(String(path.dropFirst())).standardizedFileURL
        guard file.path.hasPrefix(root.path),
            let handle = try? FileHandle(forReadingFrom: file),
            let size = try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize
        else {
            respond(task, status: 404, type: "text/plain", length: 0)
            task.didFinish()
            return
        }
        defer { try? handle.close() }
        respond(task, status: 200, type: Self.types[file.pathExtension] ?? "application/octet-stream",
                length: size)
        while let chunk = try? handle.read(upToCount: 1 << 20), !chunk.isEmpty {
            guard !stopped.contains(id) else {
                return
            }
            task.didReceive(chunk)
        }
        if !stopped.contains(id) {
            task.didFinish()
        }
    }

    func webView(_ webView: WKWebView, stop task: any WKURLSchemeTask) {
        stopped.insert(ObjectIdentifier(task))
    }

    private func respond(_ task: any WKURLSchemeTask, status: Int, type: String, length: Int) {
        guard let url = task.request.url else {
            return
        }
        let headers = [
            "Content-Type": type,
            "Content-Length": String(length),
            "Cross-Origin-Opener-Policy": "same-origin",
            "Cross-Origin-Embedder-Policy": "require-corp",
            "Cross-Origin-Resource-Policy": "same-origin",
            "Cache-Control": "no-store",
        ]
        if let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1",
                                          headerFields: headers) {
            task.didReceive(response)
        }
    }
}
