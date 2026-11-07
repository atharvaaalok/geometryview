import AppKit
import GeometryEngine
import QuartzCore

/// What the engine reports back to the interface.
enum EngineEvent: Sendable {
    case loading(document: UInt32, name: String)
    case progress(document: UInt32, done: UInt32, total: UInt32)
    case modelReady(document: UInt32)
    case sceneReady
    case failed(document: UInt32, message: String)
    /// The tree row a selection belongs to, so the tree can reveal it, or nil when cleared.
    case selectionChanged(document: UInt32, node: UInt32?)
    /// The part under the pointer and the face within it, or nil.
    case hoverChanged(document: UInt32, node: UInt32?, face: UInt32)
    /// The colours standing for each part changed, with the style or the colour mode.
    case looksChanged
    /// What is hidden or ghosted changed. The counts are of bodies with anything hidden, and with
    /// anything ghosted.
    case visibilityChanged(hidden: UInt32, ghosted: UInt32)
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

    func add(path: String) {
        geo_engine_add_file(engine, path)
    }

    func perform(_ action: GeoAction) {
        geo_engine_perform(engine, action)
    }

    func setSelectionMode(_ mode: GeoSelectionMode) {
        geo_engine_set_selection_mode(engine, mode)
    }

    func setTool(_ tool: GeoTool) {
        geo_engine_set_tool(engine, tool)
    }

    func setMouse(_ mouse: MouseSettings) {
        var settings = mouse.engineSettings
        geo_engine_set_mouse(engine, &settings)
    }

    func setSection(_ section: GeoSection) {
        var copy = section
        geo_engine_set_section(engine, &copy)
    }

    func setLook(_ look: Look) {
        var engineLook = look.engineLook
        geo_engine_set_look(engine, &engineLook)
    }

    func selectNode(document: UInt32, node: UInt32, extend: Bool) {
        geo_engine_select_node(engine, document, node, extend)
    }

    func setViewInsets(left: CGFloat, top: CGFloat, right: CGFloat, bottom: CGFloat) {
        geo_engine_set_view_insets(engine, Float(left), Float(top), Float(right), Float(bottom))
    }

    func writeSnapshot(to path: String) {
        geo_engine_write_snapshot(engine, path)
    }

    func selection() -> GeoSelectionSummary {
        geo_engine_selection(engine)
    }

    func hoverNode(document: UInt32, node: UInt32?) {
        geo_engine_hover_node(engine, document, node ?? UInt32.max)
    }

    /// The document's tree, or nil until its model has arrived.
    func model(document: UInt32) -> TreeModel? {
        geo_engine_copy_model(engine, document).map(TreeModel.init)
    }

    /// Whether what lies under a tree node is shown, hidden, ghosted or a mix.
    func nodeState(document: UInt32, node: UInt32) -> GeoNodeState {
        geo_engine_node_state(engine, document, node)
    }

    /// Hides, shows, ghosts, unghosts or isolates everything under a tree node.
    func nodeAction(document: UInt32, node: UInt32, action: GeoNodeAction) {
        geo_engine_node_action(engine, document, node, action)
    }

    /// The colour standing for an occurrence under the current look, or nil before it is known.
    /// `occurrence` counts within the document.
    func swatch(document: UInt32, occurrence: UInt32) -> NSColor? {
        let rgba = geo_engine_swatch(engine, document, occurrence)
        guard rgba != 0 else {
            return nil
        }
        return NSColor(
            srgbRed: CGFloat(rgba & 0xFF) / 255,
            green: CGFloat((rgba >> 8) & 0xFF) / 255,
            blue: CGFloat((rgba >> 16) & 0xFF) / 255,
            alpha: 1
        )
    }
}

/// A document's assembly tree. It keeps the file mapped, so it stays readable after the document
/// closes, until this is released.
final class TreeModel {
    private let model: OpaquePointer
    let count: Int
    let nodes: UnsafeBufferPointer<GeoNode>
    private let names: UnsafePointer<CChar>?

    init(_ model: OpaquePointer) {
        self.model = model
        count = Int(geo_model_node_count(model))
        nodes = UnsafeBufferPointer(start: geo_model_nodes(model), count: count)
        names = geo_model_names(model)
    }

    deinit {
        geo_model_release(model)
    }

    func name(_ node: Int) -> String {
        let record = nodes[node]
        guard let names, record.name_length > 0 else {
            return ""
        }
        let start = UnsafeRawPointer(names + Int(record.name_offset))
        let bytes = UnsafeRawBufferPointer(start: start, count: Int(record.name_length))
        return String(decoding: bytes, as: UTF8.self)
    }

    /// Nodes whose names contain `query`, ignoring case, in tree order.
    func search(_ query: String, limit: Int = 4096) -> (matches: [Int], total: Int) {
        var results = [UInt32](repeating: 0, count: limit)
        let total = query.withCString { text in
            geo_model_search(model, text, &results, UInt32(limit))
        }
        return (results.prefix(min(Int(total), limit)).map(Int.init), Int(total))
    }
}

/// Runs on the engine thread. It must stay at file scope: a closure written inside `EngineHandle` would
/// inherit main-actor isolation, and calling it from another thread is a runtime trap.
private func receiveEngineEvent(context: UnsafeMutableRawPointer?, event: UnsafePointer<GeoEvent>?)
{
    guard let context, let event = event?.pointee else {
        return
    }
    let box = Unmanaged<EventBox>.fromOpaque(context).takeUnretainedValue()
    let message = event.message.map { String(cString: $0) } ?? ""
    let translated: EngineEvent?
    switch event.kind {
    case GeoEventLoading: translated = .loading(document: event.document, name: message)
    case GeoEventProgress:
        translated = .progress(document: event.document, done: event.done, total: event.total)
    case GeoEventModelReady: translated = .modelReady(document: event.document)
    case GeoEventSceneReady: translated = .sceneReady
    case GeoEventFailed: translated = .failed(document: event.document, message: message)
    case GeoEventSelectionChanged:
        translated = .selectionChanged(
            document: event.document,
            node: event.node == UInt32.max ? nil : event.node
        )
    case GeoEventHoverChanged:
        translated = .hoverChanged(
            document: event.document,
            node: event.node == UInt32.max ? nil : event.node,
            face: event.done
        )
    case GeoEventLooksChanged: translated = .looksChanged
    case GeoEventVisibilityChanged:
        translated = .visibilityChanged(hidden: event.done, ghosted: event.total)
    default: translated = nil
    }
    if let translated {
        box.deliver(translated)
    }
}

/// Carries events from the engine thread to the main thread.
private final class EventBox: @unchecked Sendable {
    // Only touched on the main thread, which is why the type can be Sendable.
    nonisolated(unsafe) var handler: ((EngineEvent) -> Void)?

    func deliver(_ event: EngineEvent) {
        DispatchQueue.main.async { [self] in
            handler?(event)
        }
    }
}
