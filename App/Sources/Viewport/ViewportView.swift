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

    /// Posts an event that carries no position.
    private func post(_ kind: GeoPointerKind) {
        engine.post(
            GeoPointerEvent(
                kind: kind,
                button: GeoButtonNone,
                x: 0,
                y: 0,
                delta: 0,
                modifiers: 0,
                timestamp: ProcessInfo.processInfo.systemUptime
            )
        )
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

    // MARK: Only input aimed at the view

    /// The buttons this view sent a press for and has not yet released.
    private var pressedButtons: Set<UInt32> = []
    /// Whether the engine was last told the pointer is over the view.
    private var pointerInside = false

    /// Whether the event is over the 3D view itself, rather than the interface floating above it.
    /// The view hears moves wherever the pointer is, and has been handed clicks meant for a label.
    private func isUncovered(_ event: NSEvent) -> Bool {
        guard let root = window?.contentView else {
            return false
        }
        let point = root.superview?.convert(event.locationInWindow, from: nil)
        return root.hitTest(point ?? event.locationInWindow) === self
    }

    override func mouseMoved(with event: NSEvent) {
        // Nothing is pressed while the pointer moves, so a press still held lost its release.
        cancelLostPress()
        guard isUncovered(event) else {
            leave()
            return
        }
        pointerInside = true
        post(GeoPointerMove, GeoButtonNone, event)
        onPointerMoved?(convert(event.locationInWindow, from: nil))
    }

    override func mouseExited(with event: NSEvent) {
        leave()
    }

    /// Tells the engine the pointer is no longer over the view, so nothing stays hovered.
    private func leave() {
        guard pointerInside else {
            return
        }
        pointerInside = false
        post(GeoPointerLeave)
        onPointerMoved?(nil)
    }

    private func cancelLostPress() {
        guard !pressedButtons.isEmpty else {
            return
        }
        pressedButtons.removeAll()
        post(GeoPointerCancel)
    }

    private func press(_ button: GeoPointerButton, _ event: NSEvent) {
        guard isUncovered(event) else {
            return
        }
        pressedButtons.insert(button.rawValue)
        post(GeoPointerDown, button, event)
    }

    private func release(_ button: GeoPointerButton, _ event: NSEvent) {
        guard pressedButtons.remove(button.rawValue) != nil else {
            return
        }
        post(GeoPointerUp, button, event)
    }

    // A drag keeps going over the interface: only its press had to be aimed at the view.
    private func drag(_ event: NSEvent) {
        guard !pressedButtons.isEmpty else {
            return
        }
        post(GeoPointerMove, GeoButtonNone, event)
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: cursor)
    }
    override func mouseDown(with event: NSEvent) { press(GeoButtonLeft, event) }
    override func mouseDragged(with event: NSEvent) { drag(event) }
    override func mouseUp(with event: NSEvent) { release(GeoButtonLeft, event) }
    override func rightMouseDown(with event: NSEvent) { press(GeoButtonRight, event) }
    override func rightMouseDragged(with event: NSEvent) { drag(event) }
    override func rightMouseUp(with event: NSEvent) { release(GeoButtonRight, event) }
    override func otherMouseDown(with event: NSEvent) { press(GeoButtonMiddle, event) }
    override func otherMouseDragged(with event: NSEvent) { drag(event) }
    override func otherMouseUp(with event: NSEvent) { release(GeoButtonMiddle, event) }

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
        guard
            let action = KeyMap.shared.action(
                for: KeyChord(key: key, modifiers: event.modifierFlags)
            )
        else {
            super.keyDown(with: event)
            return
        }
        if let tool = action.heldTool {
            heldKeyCode = event.keyCode
            engine.setTool(tool)
            onToolChange?(tool)
        } else if let command = action.command {
            onKeyCommand?(command)
        }
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
