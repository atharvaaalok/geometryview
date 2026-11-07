import SwiftUI
import WebKit

/// The shell: the viewer's web files in a full-screen web view, served locally, with no network.
@main
struct GeometryShellApp: App {
    var body: some Scene {
        WindowGroup {
            WebView()
                .ignoresSafeArea()
        }
    }
}

struct WebView: UIViewRepresentable {
    func makeCoordinator() -> Host { Host() }

    func makeUIView(context: Context) -> WKWebView {
        context.coordinator.makeWebView()
    }

    func updateUIView(_ view: WKWebView, context: Context) {}
}

/// Owns the web view, serves it the bundled files, and saves what the page reports.
///
/// For Phase 0 it runs the capability census several times. The page is served two ways: from the
/// geo:// scheme, and over HTTP on the phone's own loopback address, which is what WebKit needs to
/// make it cross-origin isolated. Memory runs find how much iOS grants before it stops the page.
/// Reports go to Documents/census, where `devicectl` can copy them off the phone. A page stopped by
/// iOS has already saved its partial report, and the run moves on to the next measurement.
@MainActor
final class Host: NSObject, WKNavigationDelegate, WKScriptMessageHandler {
    private enum Transport: String { case scheme, loopback }
    private struct Run {
        let transport: Transport
        let label: String
        let mode: String
    }

    private let runs = Host.requestedRuns() ?? [
        Run(transport: .scheme, label: "iphone-wkwebview", mode: "core"),
        Run(transport: .loopback, label: "iphone-loopback", mode: "core"),
        Run(transport: .loopback, label: "iphone-loopback", mode: "shared-memory"),
    ]

    // Other runs can be asked for at launch, as `--runs scheme:wasm-memory,loopback:core:again`:
    // each is a transport, a mode and an optional label suffix, so repeats keep separate reports.
    private static func requestedRuns() -> [Run]? {
        let arguments = CommandLine.arguments
        guard let at = arguments.firstIndex(of: "--runs"), at + 1 < arguments.count else {
            return nil
        }
        return arguments[at + 1].split(separator: ",").compactMap { item in
            let parts = item.split(separator: ":").map(String.init)
            guard parts.count >= 2, let transport = Transport(rawValue: parts[0]) else {
                return nil
            }
            let label = "iphone-\(transport == .scheme ? "wkwebview" : "loopback")"
            return Run(transport: transport, label: parts.count > 2 ? "\(label)-\(parts[2])" : label,
                       mode: parts[1])
        }
    }
    private var runIndex = 0
    private var webView: WKWebView?
    private var server: LoopbackServer?
    private var port: UInt16?
    private var waitingForPort = false

    private var reports: URL {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return documents.appendingPathComponent("census", isDirectory: true)
    }

    func makeWebView() -> WKWebView {
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        let files = WebFiles(
            bundle: Bundle.main.resourceURL!.appendingPathComponent("census", isDirectory: true),
            files: caches.appendingPathComponent("files", isDirectory: true))
        Self.makeStreamFile(in: files.files)

        let configuration = WKWebViewConfiguration()
        configuration.setURLSchemeHandler(SchemeHandler(files: files), forURLScheme: SchemeHandler.scheme)
        configuration.userContentController.add(self, name: "census")
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.navigationDelegate = self
        view.isInspectable = true
        webView = view
        try? FileManager.default.createDirectory(at: reports, withIntermediateDirectories: true)
        NotificationCenter.default.addObserver(
            self, selector: #selector(memoryWarning),
            name: UIApplication.didReceiveMemoryWarningNotification, object: nil)

        server = try? LoopbackServer(files: files)
        server?.start { [weak self] port in
            Task { @MainActor in self?.serverReady(port) }
        }
        load()
        return view
    }

    // A 256 MB file for the page to stream, measuring how fast each way of serving delivers it.
    private static func makeStreamFile(in folder: URL) {
        let file = folder.appendingPathComponent("stream.bin")
        let size = 256 << 20
        if (try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize) == size {
            return
        }
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: file.path, contents: nil)
        guard let handle = try? FileHandle(forWritingTo: file) else {
            return
        }
        let chunk = Data((0..<(8 << 20)).map { UInt8(truncatingIfNeeded: ($0 &* 2654435761) >> 13) })
        for _ in 0..<(size / chunk.count) {
            handle.write(chunk)
        }
        try? handle.close()
    }

    private func serverReady(_ port: UInt16) {
        self.port = port
        if waitingForPort {
            waitingForPort = false
            load()
        }
    }

    private func load() {
        guard runIndex < runs.count else {
            return
        }
        let run = runs[runIndex]
        let query = "?label=\(run.label)&mode=\(run.mode)"
        switch run.transport {
        case .scheme:
            webView?.load(URLRequest(url: URL(string: "\(SchemeHandler.scheme)://local/\(query)")!))
        case .loopback:
            guard let port else {
                waitingForPort = true
                return
            }
            webView?.load(URLRequest(url: URL(string: "http://127.0.0.1:\(port)/\(query)")!))
        }
    }

    private func next() {
        runIndex += 1
        load()
    }

    // The page posts its report after each step, and once more, not partial, when done.
    func userContentController(_ controller: WKUserContentController,
                               didReceive message: WKScriptMessage) {
        guard let text = message.body as? String,
            let data = text.data(using: .utf8),
            let report = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let name = report["label"] as? String
        else {
            return
        }
        let partial = report["partial"] as? Bool ?? false
        let file = reports.appendingPathComponent("\(name)\(partial ? ".partial" : "").json")
        try? data.write(to: file, options: .atomic)
        if !partial {
            try? FileManager.default.removeItem(
                at: reports.appendingPathComponent("\(name).partial.json"))
            next()
        }
    }

    private var current: String {
        let run = runs[min(runIndex, runs.count - 1)]
        return run.mode == "core" ? run.label : "\(run.label)-\(run.mode)"
    }

    // iOS stopped the page, as the memory measurements expect it to. Its last partial report is
    // already saved, so note why and go on.
    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        note("terminated", ["terminatedAt": ISO8601DateFormatter().string(from: Date())])
        next()
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!,
                 withError error: any Error) {
        note("failed", ["error": String(describing: error)])
        next()
    }

    @objc private func memoryWarning() {
        note("memory-warning", ["warnedAt": ISO8601DateFormatter().string(from: Date())])
    }

    private func note(_ kind: String, _ fields: [String: String]) {
        if let data = try? JSONSerialization.data(withJSONObject: fields) {
            try? data.write(to: reports.appendingPathComponent("\(current).\(kind).json"))
        }
    }
}
