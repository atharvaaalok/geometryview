import AppKit

/// The strip along the top of the view where a title bar would be: dragging it moves the window.
/// The view fills the window, so without it every drag turned the model.
@MainActor
final class WindowDragArea: NSView {
    override func mouseDown(with event: NSEvent) {
        WindowDragging.handle(event, in: window)
    }
}

/// What a press on a window's title does, for the views that stand in for one.
@MainActor
enum WindowDragging {
    /// Moves the window with a press, or does what the system does on a double-click of a title
    /// bar: zoom, minimise or nothing, as chosen in System Settings.
    static func handle(_ event: NSEvent, in window: NSWindow?) {
        guard let window else {
            return
        }
        guard event.clickCount == 2 else {
            window.performDrag(with: event)
            return
        }
        switch UserDefaults.standard.string(forKey: "AppleActionOnDoubleClick") {
        case "Minimize": window.performMiniaturize(nil)
        case "None": break
        default: window.performZoom(nil)
        }
    }
}
