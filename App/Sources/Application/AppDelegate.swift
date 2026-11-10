import AppKit
import GeometryViewEngine

/// Owns the application's one window and its menu bar.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: MainWindow?
    private var engine: EngineHandle?
    private var settings: SettingsWindow?
    // Files opened from Finder arrive before launch finishes, while there is no window yet.
    private var pendingURLs: [URL] = []
    private var pendingShows: [MainWindow.ShowRequest] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        geo_trace_memory("did finish launching")
        NSApp.mainMenu = MainMenu.make()

        guard let engine = EngineHandle() else {
            presentUnsupportedHardware()
            return
        }
        geo_trace_memory("engine created")
        self.engine = engine
        engine.setMouse(MouseSettings.saved())
        let window = MainWindow(engine: engine)
        geo_trace_memory("window created")
        window.makeKeyAndOrderFront(nil)
        self.window = window
        NSApp.activate()
        geo_trace_memory("window shown")
        let openingFile = !pendingURLs.isEmpty
        if ChromeLayout.isRequested, !openingFile {
            ChromeLayout.write(window)
        }
        open(pendingURLs, in: window)
        pendingURLs = []
        pendingShows.forEach(window.show)
        pendingShows = []

        // GEOMETRYVIEW_SNAPSHOT_SETTINGS saves the Settings window to settings.png in the temporary
        // directory, for checking its layout.
        if ProcessInfo.processInfo.environment["GEOMETRYVIEW_SNAPSHOT_SETTINGS"] != nil {
            showSettings(nil)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [self] in
                if let view = settings?.contentView,
                    let image = view.bitmapImageRepForCachingDisplay(in: view.bounds)
                {
                    view.cacheDisplay(in: view.bounds, to: image)
                    let url = FileManager.default.temporaryDirectory.appendingPathComponent(
                        "settings.png"
                    )
                    try? image.representation(using: .png, properties: [:])?.write(to: url)
                }
                NSApp.terminate(nil)
            }
            return
        }

        // With a file, the window takes its snapshot once the file has loaded.
        if ChromeSnapshot.isRequested, !openingFile {
            window.contentView?.layoutSubtreeIfNeeded()
            window.displayIfNeeded()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                ChromeSnapshot.capture(window, engine: engine)
                DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
                    NSApp.terminate(nil)
                }
            }
        }
    }

    @objc func showSettings(_ sender: Any?) {
        if settings == nil {
            let window = SettingsWindow()
            window.onMouseChange = { [weak self] mouse in self?.engine?.setMouse(mouse) }
            settings = window
        }
        settings?.makeKeyAndOrderFront(nil)
    }

    @objc func openDocument(_ sender: Any?) {
        window?.chooseFile(adding: false)
    }

    @objc func addDocument(_ sender: Any?) {
        window?.chooseFile(adding: true)
    }

    /// File ▸ Close: closes what the main window shows, returning to the Library, or, with nothing
    /// open there or another window in front, that window, as ⌘W always does.
    @objc func closeDocument(_ sender: Any?) {
        let key = NSApp.keyWindow
        if let window, key === window, window.hasFiles {
            window.closeFiles(sender)
        } else {
            key?.performClose(sender)
        }
    }

    /// Files to open, and geometryview://show requests from AI apps' tools, in the order sent.
    func application(_ application: NSApplication, open urls: [URL]) {
        let files = urls.filter(\.isFileURL)
        let shows = urls.compactMap(MainWindow.ShowRequest.init(url:))
        if let window {
            open(files, in: window)
            shows.forEach(window.show)
        } else {
            pendingURLs += files
            pendingShows += shows
        }
    }

    /// Several files opened together are shown together.
    private func open(_ urls: [URL], in window: MainWindow) {
        for (index, url) in urls.enumerated() {
            if index == 0 {
                window.open(url)
            } else {
                window.add(url)
            }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    private func presentUnsupportedHardware() {
        let alert = NSAlert()
        alert.messageText = "This Mac can't run GeometryView"
        alert.informativeText = "GeometryView needs a GPU that supports Metal 4."
        alert.runModal()
        NSApp.terminate(nil)
    }
}
