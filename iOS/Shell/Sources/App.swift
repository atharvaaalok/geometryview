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
/// For Phase 0 it runs the capability census three times: once for features and limits, then once
/// each finding how much GPU and WebAssembly memory iOS grants before it stops the page. Reports go
/// to Documents/census, where `devicectl` can copy them off the phone. A page stopped by iOS has
/// already saved its partial report, and the run moves on to the next measurement.
@MainActor
final class Host: NSObject, WKNavigationDelegate, WKScriptMessageHandler {
    private var webView: WKWebView?
    private let modes = ["core", "gpu-memory", "wasm-memory"]
    private var modeIndex = 0
    private let label = "iphone-wkwebview"

    private var reports: URL {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return documents.appendingPathComponent("census", isDirectory: true)
    }

    func makeWebView() -> WKWebView {
        let configuration = WKWebViewConfiguration()
        let root = Bundle.main.resourceURL!.appendingPathComponent("census", isDirectory: true)
        configuration.setURLSchemeHandler(SchemeHandler(root: root), forURLScheme: SchemeHandler.scheme)
        configuration.userContentController.add(self, name: "census")
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.navigationDelegate = self
        view.isInspectable = true
        webView = view
        try? FileManager.default.createDirectory(at: reports, withIntermediateDirectories: true)
        NotificationCenter.default.addObserver(
            self, selector: #selector(memoryWarning),
            name: UIApplication.didReceiveMemoryWarningNotification, object: nil)
        load()
        return view
    }

    private func load() {
        guard modeIndex < modes.count else {
            return
        }
        let url = URL(string: "\(SchemeHandler.scheme)://local/?label=\(label)&mode=\(modes[modeIndex])")!
        webView?.load(URLRequest(url: url))
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
            modeIndex += 1
            load()
        }
    }

    // iOS stopped the page, as the memory measurements expect it to. Its last partial report is
    // already saved, so note why and go on.
    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        let mode = modes[min(modeIndex, modes.count - 1)]
        let note = ["mode": mode, "terminatedAt": ISO8601DateFormatter().string(from: Date())]
        if let data = try? JSONSerialization.data(withJSONObject: note) {
            try? data.write(to: reports.appendingPathComponent("\(label)-\(mode).terminated.json"))
        }
        modeIndex += 1
        load()
    }

    @objc private func memoryWarning() {
        let mode = modes[min(modeIndex, modes.count - 1)]
        let note = ["mode": mode, "warnedAt": ISO8601DateFormatter().string(from: Date())]
        if let data = try? JSONSerialization.data(withJSONObject: note) {
            try? data.write(to: reports.appendingPathComponent("\(label)-\(mode).memory-warning.json"))
        }
    }
}
