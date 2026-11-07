import AppKit
import GeometryEngine
import Metal
import QuartzCore

/// The full-bleed 3D surface. It owns the CAMetalLayer and forwards input to the engine; the engine
/// does all the drawing, on its own thread.
@MainActor
final class ViewportView: NSView {
    private let engine: EngineHandle
    private var attached = false

    init(engine: EngineHandle) {
        self.engine = engine
        super.init(frame: .zero)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
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

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        updateDrawableSize()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        updateDrawableSize()
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
        if !attached {
            attached = true
            engine.attach(layer: metalLayer)
        }
        engine.resize(width: UInt32(width), height: UInt32(height), scale: Float(scale))
    }
}
