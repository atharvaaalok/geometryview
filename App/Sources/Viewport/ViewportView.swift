import AppKit
import GeometryEngine
import Metal
import QuartzCore
import UniformTypeIdentifiers

/// The full-bleed 3D surface. It owns the CAMetalLayer and forwards input to the engine; the engine
/// does all the drawing, on its own thread.
@MainActor
final class ViewportView: NSView {
    private let engine: EngineHandle
    private var attached = false
    private var spaceHeld = false
    private var trackingArea: NSTrackingArea?

    /// Called with the file URLs dropped on the viewport.
    var onDropFiles: (([URL]) -> Void)?
    /// Called for each key command, so the window can keep its controls in step.
    var onKeyCommand: ((KeyCommand) -> Void)?
    /// Called when a held key changes what clicks do.
    var onToolChange: ((GeoTool) -> Void)?
    /// Called with the pointer's position in this view, for labels that follow it.
    var onPointerMoved: ((NSPoint?) -> Void)?
    /// Called when files start or stop being dragged over the view.
    var onDragging: ((Bool) -> Void)?

    /// The cursor shown over the view.
    var cursor: NSCursor = .arrow {
        didSet {
            if cursor != oldValue {
                window?.invalidateCursorRects(for: self)
            }
        }
    }

    init(engine: EngineHandle) {
        self.engine = engine
        super.init(frame: .zero)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
        registerForDraggedTypes([.fileURL])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("ViewportView is not used from Interface Builder")
    }

    private var metalLayer: CAMetalLayer {
        guard let layer = layer as? CAMetalLayer else {
            fatalError("ViewportView must be backed by a CAMetalLayer")
        }
        return layer
    }

    override func makeBackingLayer() -> CALayer {
        let layer = CAMetalLayer()
        layer.device = MTLCreateSystemDefaultDevice()
        layer.pixelFormat = .bgra8Unorm
        layer.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
        layer.framebufferOnly = true
        layer.maximumDrawableCount = 2
        layer.displaySyncEnabled = true
        layer.isOpaque = true
        return layer
    }

    override var isOpaque: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        window?.acceptsMouseMovedEvents = true
        window?.makeFirstResponder(self)
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        updateDrawableSize()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        updateDrawableSize()
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let trackingArea {
            removeTrackingArea(trackingArea)
        }
        let area = NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
            owner: self
        )
        addTrackingArea(area)
        trackingArea = area
    }

    private func updateDrawableSize() {
        guard let window, bounds.width > 0, bounds.height > 0 else {
            return
        }
        let scale = window.backingScaleFactor
        let width = (bounds.width * scale).rounded()
        let height = (bounds.height * scale).rounded()
        metalLayer.contentsScale = scale
        metalLayer.drawableSize = CGSize(width: width, height: height)
        engine.resize(width: UInt32(width), height: UInt32(height), scale: Float(scale))
        if !attached {
            attached = true
            engine.attach(layer: metalLayer)
        }
    }

    // MARK: Pointer input

    private func post(_ kind: GeoPointerKind, _ button: GeoPointerButton, _ event: NSEvent) {
        post(kind, button, event, delta: 0)
    }

    private func post(
        _ kind: GeoPointerKind,
        _ button: GeoPointerButton,
        _ event: NSEvent,
        delta: Float
    ) {
        let point = convert(event.locationInWindow, from: nil)
        let scale = window?.backingScaleFactor ?? 1
        engine.post(
            GeoPointerEvent(
                kind: kind,
                button: button,
                x: Float(point.x * scale),
                y: Float((bounds.height - point.y) * scale),
                delta: delta,
                modifiers: modifiers(of: event),
                timestamp: event.timestamp
            )
        )
    }

    private func modifiers(of event: NSEvent) -> UInt32 {
        let flags = event.modifierFlags
        var result: UInt32 = 0
        if flags.contains(.shift) { result |= UInt32(GeoModifierShift) }
        if flags.contains(.control) { result |= UInt32(GeoModifierControl) }
        if flags.contains(.option) { result |= UInt32(GeoModifierOption) }
        if flags.contains(.command) { result |= UInt32(GeoModifierCommand) }
        if spaceHeld { result |= UInt32(GeoModifierSpace) }
        return result
    }

    override func mouseMoved(with event: NSEvent) {
        post(GeoPointerMove, GeoButtonNone, event)
        onPointerMoved?(convert(event.locationInWindow, from: nil))
    }

    override func mouseExited(with event: NSEvent) {
        onPointerMoved?(nil)
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: cursor)
    }
    override func mouseDragged(with event: NSEvent) { post(GeoPointerMove, GeoButtonNone, event) }
    override func rightMouseDragged(with event: NSEvent) {
        post(GeoPointerMove, GeoButtonNone, event)
    }
    override func otherMouseDragged(with event: NSEvent) {
        post(GeoPointerMove, GeoButtonNone, event)
    }
    override func mouseDown(with event: NSEvent) { post(GeoPointerDown, GeoButtonLeft, event) }
    override func mouseUp(with event: NSEvent) { post(GeoPointerUp, GeoButtonLeft, event) }
    override func rightMouseDown(with event: NSEvent) {
        post(GeoPointerDown, GeoButtonRight, event)
    }
    override func rightMouseUp(with event: NSEvent) { post(GeoPointerUp, GeoButtonRight, event) }
    override func otherMouseDown(with event: NSEvent) {
        post(GeoPointerDown, GeoButtonMiddle, event)
    }
    override func otherMouseUp(with event: NSEvent) { post(GeoPointerUp, GeoButtonMiddle, event) }

    override func scrollWheel(with event: NSEvent) {
        // Momentum would keep zooming after the fingers lift, which is the inertia this viewer avoids.
        guard event.momentumPhase.isEmpty else {
            return
        }
        let perLine: CGFloat = event.hasPreciseScrollingDeltas ? 2 : 30
        post(GeoPointerScroll, GeoButtonNone, event, delta: Float(event.scrollingDeltaY * perLine))
    }

    override func magnify(with event: NSEvent) {
        post(GeoPointerMagnify, GeoButtonNone, event, delta: Float(event.magnification * 400))
    }

    // MARK: Keyboard

    private static let spaceKeyCode: UInt16 = 49
    // The key holding a tool, remembered by code so its release is caught whatever is typed since.
    private var heldKeyCode: UInt16?

    override func keyDown(with event: NSEvent) {
        if event.keyCode == Self.spaceKeyCode {
            spaceHeld = true
            return
        }
        guard !event.isARepeat, let key = event.charactersIgnoringModifiers?.lowercased() else {
            if !event.isARepeat {
                super.keyDown(with: event)
            }
            return
        }
        let modifiers = event.modifierFlags.intersection(KeyBindings.relevant)
        if modifiers.isEmpty, let held = KeyBindings.held.first(where: { $0.key == key }) {
            heldKeyCode = event.keyCode
            engine.setTool(held.tool)
            onToolChange?(held.tool)
            return
        }
        guard
            let binding = KeyBindings.pressed.first(where: {
                $0.key == key && $0.modifiers == modifiers
            })
        else {
            super.keyDown(with: event)
            return
        }
        onKeyCommand?(binding.command)
    }

    override func keyUp(with event: NSEvent) {
        if event.keyCode == Self.spaceKeyCode {
            spaceHeld = false
        } else if event.keyCode == heldKeyCode {
            releaseHeldTool()
        } else {
            super.keyUp(with: event)
        }
    }

    /// Drops a held tool, as when the window loses focus while its key is down.
    func releaseHeldTool() {
        guard heldKeyCode != nil else {
            return
        }
        heldKeyCode = nil
        engine.setTool(GeoToolSelect)
        onToolChange?(GeoToolSelect)
    }

    override func resignFirstResponder() -> Bool {
        releaseHeldTool()
        spaceHeld = false
        return super.resignFirstResponder()
    }

    // MARK: Dropping files

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        let accepted = !fileURLs(from: sender).isEmpty
        onDragging?(accepted)
        return accepted ? .copy : []
    }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        onDragging?(false)
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        onDragging?(false)
        let urls = fileURLs(from: sender)
        guard !urls.isEmpty else {
            return false
        }
        onDropFiles?(urls)
        return true
    }

    private func fileURLs(from info: NSDraggingInfo) -> [URL] {
        let options: [NSPasteboard.ReadingOptionKey: Any] = [.urlReadingFileURLsOnly: true]
        return info.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: options)
            as? [URL] ?? []
    }
}
