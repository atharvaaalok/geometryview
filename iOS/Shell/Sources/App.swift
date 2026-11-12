import SwiftUI
import WebKit

/// The shell: the viewer's web files in a full-screen web view, served locally, with no network.
/// STEP and STL files opened in it from Files or the share sheet go to the viewer.
@main
struct GeometryViewShellApp: App {
    var body: some Scene {
        WindowGroup {
            WebView()
                .ignoresSafeArea()
                .onOpenURL { url in Host.shared.open(url) }
        }
    }
}

struct WebView: UIViewRepresentable {
    func makeCoordinator() -> Host { Host.shared }

    func makeUIView(context: Context) -> WKWebView {
        context.coordinator.makeWebView()
    }

    func updateUIView(_ view: WKWebView, context: Context) {}
}

/// Owns the web view, serves it the bundled files, and hands it the documents the person opens.
///
/// Launched as an app, it shows the viewer, and if iOS stops the page's process, as it may when the
/// phone runs short of memory, loads it again and reopens the last document. Launched with
/// `--runs`, it makes Phase 0's and later measurements instead: the capability census, the
/// renderer and import spikes, and the viewer's scripted checks. Pages are served two ways: from the geo:// scheme, and over HTTP on the phone's own
/// loopback address, which is what WebKit needs to make them cross-origin isolated. Memory runs
/// find how much iOS grants before it stops the page.
/// Reports go to Documents/census, where `devicectl` can copy them off the phone. A page stopped by
/// iOS has already saved its partial report, and the run moves on to the next measurement.
@MainActor
final class Host: NSObject, WKNavigationDelegate, WKScriptMessageHandler {
    private enum Transport: String { case scheme, loopback }
    private struct Run {
        let transport: Transport
        let page: String  // census, spike, or spike/import.html
        let label: String
        let query: String // without the label
        // The name the page reports under, for notes about a run the system stopped.
        let name: String
    }

    static let shared = Host()

    // Measurements when asked for at launch; otherwise the viewer itself.
    private let measuring = Host.requestedRuns() != nil
    private lazy var runs = Host.requestedRuns() ?? [
        Run(transport: .loopback, page: "viewer", label: "", query: "", name: "viewer"),
    ]
    private let opened = OpenedFiles()
    private lazy var library = ShellLibrary(opened: opened)
    private var pageReady = false
    private var pending: String?    // a document to hand the page once it is ready
    private var lastOpened: String? // the last document handed over, to reopen after a restart

    private static func label(_ transport: Transport, _ suffix: String?) -> String {
        let base = "iphone-\(transport == .scheme ? "wkwebview" : "loopback")"
        return suffix.map { "\(base)-\($0)" } ?? base
    }

    private static func census(_ transport: Transport, mode: String, suffix: String? = nil) -> Run {
        let label = label(transport, suffix)
        return Run(transport: transport, page: "census", label: label, query: "mode=\(mode)",
                   name: mode == "core" ? label : "\(label)-\(mode)")
    }

    // Other runs can be asked for at launch, comma-separated, each one of
    //   transport:census:mode[:suffix]     for example loopback:census:wasm-memory:again
    //   transport:spike:query[:suffix]     for example loopback:spike:model=pack&w=804&h=1748
    //   transport:import:query[:suffix]    for example loopback:import:model=ple120&workers=2
    //   transport:step:query[:suffix]      for example loopback:step:model=hard&workers=2
    //   transport:renderer:query[:suffix]  for example loopback:renderer:model=pack&w=804&h=1748
    //   transport:viewer:query[:suffix]    for example loopback:viewer:model=pack&test=idle
    // where a suffix keeps a repeat's report apart.
    private static func requestedRuns() -> [Run]? {
        let arguments = CommandLine.arguments
        guard let at = arguments.firstIndex(of: "--runs"), at + 1 < arguments.count else {
            return nil
        }
        return arguments[at + 1].split(separator: ",").compactMap { item in
            let parts = item.split(separator: ":").map(String.init)
            guard parts.count >= 3, let transport = Transport(rawValue: parts[0]) else {
                return nil
            }
            let suffix = parts.count > 3 ? parts[3] : nil
            if parts[1] == "census" {
                return census(transport, mode: parts[2], suffix: suffix)
            }
            let label = label(transport, suffix)
            let pages = ["import": "spike/import.html", "step": "spike/step.html",
                         "renderer": "spike/renderer.html", "viewer": "viewer/index.html"]
            let page = pages[parts[1]] ?? "spike"
            return Run(transport: transport, page: page, label: label, query: parts[2],
                       name: "\(label)-\(parts[1])")
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
        let files = WebFiles(bundle: Bundle.main.resourceURL!,
                             files: caches.appendingPathComponent("files", isDirectory: true),
                             opened: opened)
        if measuring {
            Self.makeStreamFile(in: files.files)
        }
        if runs.contains(where: { $0.query.contains("test=library") }) {
            library.connectTestFolder(bundle: Bundle.main.resourceURL!)
        }
        // The gallery's check: the folder it names, copied into Documents beforehand.
        for run in runs where run.query.contains("test=gallery") {
            let folder = URLComponents(string: "?" + run.query)?.queryItems?
                .first { $0.name == "folder" }?.value
            library.connectDocumentsFolder(named: folder ?? "test-models")
        }

        let configuration = WKWebViewConfiguration()
        configuration.setURLSchemeHandler(SchemeHandler(files: files), forURLScheme: SchemeHandler.scheme)
        configuration.userContentController.add(self, name: "census")
        configuration.userContentController.add(self, name: "shell")
        configuration.userContentController.addScriptMessageHandler(library, contentWorld: .page, name: "library")
        let view = WKWebView(frame: .zero, configuration: configuration)
        library.webView = view
        view.navigationDelegate = self
        view.isInspectable = true
        view.isOpaque = false
        view.backgroundColor = UIColor(red: 0xF2 / 255, green: 0xEF / 255, blue: 0xE7 / 255, alpha: 1)
        webView = view
        try? FileManager.default.createDirectory(at: reports, withIntermediateDirectories: true)
        // Measurements run for minutes with nobody touching the phone; keep it awake meanwhile.
        UIApplication.shared.isIdleTimerDisabled = measuring
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
        let page = run.page.hasSuffix(".html") ? "/\(run.page)" : "/\(run.page)/"
        let path = measuring ? "\(page)?label=\(run.label)&\(run.query)" : page
        pageReady = false
        switch run.transport {
        case .scheme:
            webView?.load(URLRequest(url: URL(string: "\(SchemeHandler.scheme)://local\(path)")!))
        case .loopback:
            guard let port else {
                waitingForPort = true
                return
            }
            webView?.load(URLRequest(url: URL(string: "http://127.0.0.1:\(port)\(path)")!))
        }
    }

    private func next() {
        runIndex += 1
        load()
    }

    // MARK: Documents

    /// A STEP or STL file opened from Files or the share sheet: served to the page under a path of
    /// its own, and opened there as soon as the page is ready.
    func open(_ url: URL) {
        guard ["step", "stp", "stl", "geoblob"].contains(url.pathExtension.lowercased()) else {
            return
        }
        let id = opened.add(url)
        let name = url.lastPathComponent
        let modified = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?
            .contentModificationDate?.timeIntervalSince1970 ?? 0
        let path = "/open/\(id)/\(name.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? id)"
        let call = "geometryViewShell.open(\(Self.quoted(path)), \(Self.quoted(name)), \(Int(modified * 1000)))"
        lastOpened = call
        if pageReady {
            webView?.evaluateJavaScript(call)
        } else {
            pending = call
        }
    }

    private static func quoted(_ text: String) -> String {
        let data = try? JSONSerialization.data(withJSONObject: [text])
        let array = data.map { String(decoding: $0, as: UTF8.self) } ?? "[\"\"]"
        return String(array.dropFirst().dropLast())
    }

    // The viewer says it is ready for documents.
    private func viewerReady() {
        pageReady = true
        if let call = pending {
            pending = nil
            webView?.evaluateJavaScript(call)
        }
    }

    // The page posts its report after each step, and once more, not partial, when done. The viewer
    // says, on the shell's own channel, when it is ready.
    func userContentController(_ controller: WKUserContentController,
                               didReceive message: WKScriptMessage) {
        if message.name == "shell" {
            if (message.body as? [String: Any])?["ready"] as? Bool == true {
                viewerReady()
            }
            return
        }
        guard let text = message.body as? String,
            let received = text.data(using: .utf8),
            var report = try? JSONSerialization.jsonObject(with: received) as? [String: Any],
            let name = report["label"] as? String
        else {
            return
        }
        // How hot the phone ran, since iOS slows the GPU as it heats.
        report["thermalState"] = Self.thermalNames[ProcessInfo.processInfo.thermalState] ?? "unknown"
        let data = (try? JSONSerialization.data(withJSONObject: report)) ?? received
        let partial = report["partial"] as? Bool ?? false
        let file = reports.appendingPathComponent("\(name)\(partial ? ".partial" : "").json")
        try? data.write(to: file, options: .atomic)
        if !partial {
            try? FileManager.default.removeItem(
                at: reports.appendingPathComponent("\(name).partial.json"))
            next()
        }
    }

    private static let thermalNames: [ProcessInfo.ThermalState: String] = [
        .nominal: "nominal", .fair: "fair", .serious: "serious", .critical: "critical",
    ]

    private var current: String {
        runs[min(runIndex, runs.count - 1)].name
    }

    // iOS stopped the page. In a measurement, as the memory runs expect, its last partial report is
    // already saved, so note why and go on; in the viewer, load it again and reopen the document it
    // had, which its library then opens without importing again.
    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        guard measuring else {
            pending = lastOpened
            load()
            return
        }
        note("terminated", ["terminatedAt": ISO8601DateFormatter().string(from: Date())])
        next()
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!,
                 withError error: any Error) {
        guard measuring else {
            return
        }
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
