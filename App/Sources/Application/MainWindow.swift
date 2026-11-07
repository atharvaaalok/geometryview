import AppKit

/// The single window: a full-bleed viewport with floating chrome above it.
@MainActor
final class MainWindow: NSWindow {
    private let engine: EngineHandle

    init(engine: EngineHandle) {
        self.engine = engine
        super.init(
            contentRect: NSRect(x: 0, y: 0, width: 1440, height: 900),
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered,
            defer: false)

        title = "Geometry"
        titlebarAppearsTransparent = true
        titleVisibility = .hidden
        isMovableByWindowBackground = false
        backgroundColor = Palette.paper
        isReleasedWhenClosed = false
        minSize = NSSize(width: 720, height: 480)
        center()

        let root = NSView()
        let viewport = ViewportView(engine: engine)
        let emptyState = StickerCard.emptyState()
        for view in [viewport, emptyState] {
            view.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(view)
        }
        NSLayoutConstraint.activate([
            viewport.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            viewport.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            viewport.topAnchor.constraint(equalTo: root.topAnchor),
            viewport.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            emptyState.centerXAnchor.constraint(equalTo: root.centerXAnchor),
            emptyState.centerYAnchor.constraint(equalTo: root.centerYAnchor),
            emptyState.widthAnchor.constraint(equalToConstant: 340),
        ])
        contentView = root
    }
}
