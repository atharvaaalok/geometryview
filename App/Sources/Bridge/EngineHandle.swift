import Foundation
import GeometryEngine
import QuartzCore

/// What the engine reports back to the interface.
enum EngineEvent {
    case loading(String)
    case sceneReady
    case failed(String)
}

/// Owns the engine's lifetime on the Swift side. All calls are non-blocking.
@MainActor
final class EngineHandle {
    private var engine: OpaquePointer?
    private let eventBox: EventBox

    /// Receives engine events on the main thread.
    var onEvent: ((EngineEvent) -> Void)? {
        didSet { eventBox.handler = onEvent }
    }

    init?() {
        guard let library = Bundle.main.path(forResource: "default", ofType: "metallib"),
            let helper = Bundle.main.path(forAuxiliaryExecutable: "geometry-import")
        else {
            return nil
        }
        guard let caches = Self.cacheDirectory() else {
            return nil
        }

        let box = EventBox()
        eventBox = box
        let created = library.withCString { libraryPath in
            helper.withCString { helperPath in
                caches.withCString { cachePath -> OpaquePointer? in
                    var descriptor = GeoEngineDescriptor(
                        shader_library_path: libraryPath,
                        import_helper_path: helperPath,
                        cache_directory: cachePath,
                        event_callback: receiveEngineEvent,
                        event_context: Unmanaged.passUnretained(box).toOpaque()
                    )
                    return geo_engine_create(&descriptor)
                }
            }
        }
        guard let created else {
            return nil
        }
        engine = created
    }

    isolated deinit {
        geo_engine_destroy(engine)
    }

    private static func cacheDirectory() -> String? {
        let manager = FileManager.default
        guard
            let caches = manager.urls(for: .cachesDirectory, in: .userDomainMask).first?
                .appendingPathComponent("Geometry", isDirectory: true)
        else {
            return nil
        }
        try? manager.createDirectory(at: caches, withIntermediateDirectories: true)
        return caches.path
    }

    func attach(layer: CAMetalLayer) {
        geo_engine_attach_layer(engine, Unmanaged.passUnretained(layer).toOpaque())
    }

    func resize(width: UInt32, height: UInt32, scale: Float) {
        geo_engine_resize(engine, width, height, scale)
    }

    func post(_ event: GeoPointerEvent) {
        var event = event
        geo_engine_post_pointer(engine, &event)
    }

    func reportLatency() {
        geo_engine_report_latency(engine)
    }

    func stats() -> GeoEngineStats {
        geo_engine_stats(engine)
    }

    func open(path: String) {
        geo_engine_open_file(engine, path)
    }

    func perform(_ action: GeoAction) {
        geo_engine_perform(engine, action)
    }
}

/// Runs on the engine thread. It must stay at file scope: a closure written inside `EngineHandle` would
/// inherit main-actor isolation, and calling it from another thread is a runtime trap.
private func receiveEngineEvent(
    context: UnsafeMutableRawPointer?,
    kind: GeoEventKind,
    message: UnsafePointer<CChar>?
) {
    guard let context else {
        return
    }
    let box = Unmanaged<EventBox>.fromOpaque(context).takeUnretainedValue()
    box.deliver(kind, message.map { String(cString: $0) } ?? "")
}

/// Carries events from the engine thread to the main thread.
private final class EventBox: @unchecked Sendable {
    // Only touched on the main thread, which is why the type can be Sendable.
    nonisolated(unsafe) var handler: ((EngineEvent) -> Void)?

    func deliver(_ kind: GeoEventKind, _ message: String) {
        DispatchQueue.main.async { [self] in
            switch kind {
            case GeoEventLoading: handler?(.loading(message))
            case GeoEventSceneReady: handler?(.sceneReady)
            case GeoEventFailed: handler?(.failed(message))
            default: break
            }
        }
    }
}
