import Foundation
import WebKit

/// Serves the shell's web files at geo://local/..., with the right media types and the isolation
/// headers. WebKit honours those headers only over HTTP, so a geo:// page is not cross-origin
/// isolated and gets no WebAssembly threads; `LoopbackServer` is the way to get them.
///
/// WebKit calls this on the main thread and it answers there, in 1 MB chunks. That is enough for
/// the census; serving large model files will need reading off the main thread.
@MainActor
final class SchemeHandler: NSObject, WKURLSchemeHandler {
    static let scheme = "geo"

    private let files: WebFiles
    // Tasks WebKit has stopped. Answering one of those crashes, so every answer checks first.
    private var stopped = Set<ObjectIdentifier>()

    init(files: WebFiles) {
        self.files = files
    }

    func webView(_ webView: WKWebView, start task: any WKURLSchemeTask) {
        let id = ObjectIdentifier(task)
        stopped.remove(id)
        guard let url = task.request.url else {
            return
        }
        if let listing = files.listing(url.path) {
            respond(task, status: 200, type: "application/json", length: listing.count)
            task.didReceive(listing)
            task.didFinish()
            return
        }
        guard let file = files.resolve(url.path),
            let handle = try? FileHandle(forReadingFrom: file),
            let size = try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize
        else {
            respond(task, status: 404, type: "text/plain", length: 0)
            task.didFinish()
            return
        }
        defer { try? handle.close() }
        respond(task, status: 200, type: WebFiles.type(of: file), length: size,
                modified: url.path.hasPrefix("/open/") ? WebFiles.modified(file) : nil)
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

    private func respond(
        _ task: any WKURLSchemeTask,
        status: Int,
        type: String,
        length: Int,
        modified: String? = nil
    ) {
        guard let url = task.request.url else {
            return
        }
        var headers = WebFiles.isolation
        headers["Content-Type"] = type
        headers["Content-Length"] = String(length)
        headers["GeometryView-Modified"] = modified
        if let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1",
                                          headerFields: headers) {
            task.didReceive(response)
        }
    }
}
