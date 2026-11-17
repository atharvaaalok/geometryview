import AppKit
import GeometryViewEngine

// --navigation-check runs the back-and-forward cases it is given and exits, with no window.
if NavigationCheck.isRequested {
    exit(NavigationCheck.run())
}

MainActor.assumeIsolated {
    geo_trace_memory("main")
    let application = NSApplication.shared
    let delegate = AppDelegate()
    application.delegate = delegate
    application.setActivationPolicy(.regular)
    withExtendedLifetime(delegate) {
        application.run()
    }
}
