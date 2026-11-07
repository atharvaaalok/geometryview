import AppKit
import GeometryEngine

/// Owns the application's one window and its menu bar.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: MainWindow?
    // Files opened from Finder arrive before launch finishes, while there is no window yet.
    private var pendingURLs: [URL] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        geo_trace_memory("did finish launching")
        NSApp.mainMenu = MainMenu.make()

        guard let engine = EngineHandle() else {
            presentUnsupportedHardware()
            return
        }
        geo_trace_memory("engine created")
        let window = MainWindow(engine: engine)
        geo_trace_memory("window created")
        window.makeKeyAndOrderFront(nil)
        self.window = window
        NSApp.activate()
        geo_trace_memory("window shown")
        let openingFile = !pendingURLs.isEmpty
        open(pendingURLs, in: window)
        pendingURLs = []

        // With a file, the window takes its snapshot once the file has loaded.
        if ChromeSnapshot.isRequested, !openingFile {
            window.contentView?.layoutSubtreeIfNeeded()
            window.displayIfNeeded()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                ChromeSnapshot.capture(window)
                NSApp.terminate(nil)
            }
        }
    }

    @objc func openDocument(_ sender: Any?) {
        window?.chooseFile(adding: false)
    }

    @objc func addDocument(_ sender: Any?) {
        window?.chooseFile(adding: true)
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        if let window {
            open(urls, in: window)
        } else {
            pendingURLs += urls
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
        alert.messageText = "This Mac can't run Geometry"
        alert.informativeText = "Geometry needs a GPU that supports Metal 4."
        alert.runModal()
        NSApp.terminate(nil)
    }
}
