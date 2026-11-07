import AppKit
import UniformTypeIdentifiers

/// The single window: a full-bleed viewport with floating chrome above it.
@MainActor
final class MainWindow: NSWindow {
    private let engine: EngineHandle
    private let status = StatusCard()
    private var exercise: Exercise?
    private var accessedURL: URL?

    init(engine: EngineHandle) {
        self.engine = engine
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
        let viewport = ViewportView(engine: engine)
        for view in [viewport, status] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(view)
        }
        NSLayoutConstraint.activate([
            viewport.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            viewport.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            viewport.topAnchor.constraint(equalTo: root.topAnchor),
            viewport.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            status.centerXAnchor.constraint(equalTo: root.centerXAnchor),
            status.centerYAnchor.constraint(equalTo: root.centerYAnchor),
            status.widthAnchor.constraint(equalToConstant: 340),
        ])
        contentView = root

        viewport.onDropFiles = { [weak self] urls in
            if let url = urls.first {
                self?.open(url)
            }
        }
        engine.onEvent = { [weak self] event in
            self?.handle(event)
        }
    }

    func open(_ url: URL) {
        // The sandbox grant travels with the URL, and a path alone would not carry it. Access stays
        // active while the file is open, because the import helper reads it after this returns.
        accessedURL?.stopAccessingSecurityScopedResource()
        accessedURL = url.startAccessingSecurityScopedResource() ? url : nil
        engine.open(path: url.path)
    }

    func chooseFile() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.allowedContentTypes = ["stl", "step", "stp"].compactMap {
            UTType(filenameExtension: $0)
        }
        panel.beginSheetModal(for: self) { [weak self] response in
            if response == .OK, let url = panel.url {
                self?.open(url)
            }
        }
    }

    private func handle(_ event: EngineEvent) {
        switch event {
        case .loading(let name):
            status.showLoading(name)
        case .sceneReady:
            status.isHidden = true
            if Exercise.isRequested, exercise == nil {
                let run = Exercise(engine: engine, size: contentView?.bounds.size ?? .zero)
                exercise = run
                run.start(window: self)
            }
        case .failed(let message):
            status.showFailure(message)
        }
    }
}
