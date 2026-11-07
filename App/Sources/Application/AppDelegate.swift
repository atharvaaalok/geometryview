import AppKit
import GeometryEngine

/// Owns the application's one window and its menu bar.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: MainWindow?

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

        if ChromeSnapshot.isRequested {
            window.contentView?.layoutSubtreeIfNeeded()
            window.displayIfNeeded()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                ChromeSnapshot.capture(window)
                NSApp.terminate(nil)
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
