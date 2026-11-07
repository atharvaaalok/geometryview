import Foundation
import GeometryEngine
import QuartzCore

/// Owns the engine's lifetime on the Swift side. All calls are non-blocking.
@MainActor
final class EngineHandle {
    private let engine: OpaquePointer

    init?() {
        guard let library = Bundle.main.path(forResource: "default", ofType: "metallib"),
            let engine = geo_engine_create(library)
        else {
            return nil
        }
        self.engine = engine
    }

    isolated deinit {
        geo_engine_destroy(engine)
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
}
