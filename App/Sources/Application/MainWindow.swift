import AppKit
import GeometryEngine
import UniformTypeIdentifiers

/// The single window: a full-bleed viewport with floating chrome above it.
@MainActor
final class MainWindow: NSWindow {
    private let engine: EngineHandle
    private let status = StatusCard()
    private let structure: StructureCard
    private let viewport: ViewportView
    private var exercise: Exercise?
    private var accessedURLs: [URL] = []

    init(engine: EngineHandle) {
        self.engine = engine
        structure = StructureCard(engine: engine)
        viewport = ViewportView(engine: engine)
        super.init(
            contentRect: NSRect(x: 0, y: 0, width: 1440, height: 900),
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )

        title = "Geometry"
        titlebarAppearsTransparent = true
        titleVisibility = .hidden
        isMovableByWindowBackground = false
        backgroundColor = Palette.paper
        isReleasedWhenClosed = false
        minSize = NSSize(width: 720, height: 480)
        center()

        let root = NSView()
        root.wantsLayer = true
        structure.isHidden = true
        for view in [viewport, structure, status] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(view)
        }
        NSLayoutConstraint.activate([
            viewport.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            viewport.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            viewport.topAnchor.constraint(equalTo: root.topAnchor),
            viewport.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            structure.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: Metrics.gap),
            structure.topAnchor.constraint(
                equalTo: root.topAnchor,
                constant: Self.titlebarClearance
            ),
            structure.bottomAnchor.constraint(
                equalTo: root.bottomAnchor,
                constant: -Metrics.gap - Metrics.shadowCard
            ),
            structure.widthAnchor.constraint(equalToConstant: Self.cardWidth),
            status.centerXAnchor.constraint(equalTo: root.centerXAnchor),
            status.centerYAnchor.constraint(equalTo: root.centerYAnchor),
            status.widthAnchor.constraint(equalToConstant: 340),
        ])
        contentView = root

        viewport.onDropFiles = { [weak self] urls in
            // Holding Command adds files to what is shown, as in the library.
            let adding = NSApp.currentEvent?.modifierFlags.contains(.command) == true
            for (index, url) in urls.enumerated() {
                if adding || index > 0 {
                    self?.add(url)
                } else {
                    self?.open(url)
                }
            }
        }
        engine.onEvent = { [weak self] event in
            self?.handle(event)
        }
    }

    private static let cardWidth: CGFloat = 300
    private static let titlebarClearance: CGFloat = 44

    /// Opens a file in place of whatever is shown.
    func open(_ url: URL) {
        for accessed in accessedURLs {
            accessed.stopAccessingSecurityScopedResource()
        }
        accessedURLs = []
        structure.reset()
        add(url, replacing: true)
    }

    /// Opens a file alongside what is shown.
    func add(_ url: URL) {
        add(url, replacing: false)
    }

    private func add(_ url: URL, replacing: Bool) {
        // The sandbox grant travels with the URL, and a path alone would not carry it. Access stays
        // active while the file is open, because the import helper reads it after this returns.
        if url.startAccessingSecurityScopedResource() {
            accessedURLs.append(url)
        }
        structure.addDocument(named: url.lastPathComponent)
        structure.isHidden = false
        if replacing {
            engine.open(path: url.path)
        } else {
            engine.add(path: url.path)
        }
    }

    func chooseFile(adding: Bool) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = adding
        panel.canChooseDirectories = false
        panel.allowedContentTypes = ["stl", "step", "stp"].compactMap {
            UTType(filenameExtension: $0)
        }
        panel.beginSheetModal(for: self) { [weak self] response in
            guard response == .OK else {
                return
            }
            for (index, url) in panel.urls.enumerated() {
                if adding || index > 0 {
                    self?.add(url)
                } else {
                    self?.open(url)
                }
            }
        }
    }

    override func resignKey() {
        super.resignKey()
        viewport.releaseHeldTool()
    }

    @objc func performAction(_ sender: NSMenuItem) {
        engine.perform(GeoAction(rawValue: UInt32(sender.tag)))
    }

    @objc func chooseSelectionMode(_ sender: NSMenuItem) {
        engine.setSelectionMode(GeoSelectionMode(rawValue: UInt32(sender.tag)))
    }

    private func handle(_ event: EngineEvent) {
        switch event {
        case .loading(_, let name):
            status.showLoading(name)
        case .progress(_, let done, let total):
            status.showProgress(done: done, total: total)
        case .modelReady(let document):
            structure.modelReady(document: document)
        case .sceneReady:
            status.isHidden = true
            if ChromeSnapshot.isRequested {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [self] in
                    ChromeSnapshot.capture(self)
                    NSApp.terminate(nil)
                }
            }
            if Exercise.isRequested, exercise == nil {
                let run = Exercise(engine: engine, size: contentView?.bounds.size ?? .zero)
                exercise = run
                run.start(window: self)
            }
        case .failed(_, let message):
            status.showFailure(message)
        case .selectionChanged(let document, let node):
            structure.reveal(document: document, node: node)
        }
    }
}
