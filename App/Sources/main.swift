import AppKit
import GeometryViewEngine

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
