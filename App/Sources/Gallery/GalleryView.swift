import AppKit
import GeometryViewEngine
import QuartzCore

/// Paints the gallery's display list (gallery.h) with Core Animation layers, over the style's sheet,
/// and turns the pointer and keys into the gallery's actions. Everything it draws was placed by the
/// layout core, so the browser's painter draws the same.
@MainActor
final class GalleryView: NSView {
    var gallery: GalleryHandle
    /// Pictures by file, decoded off the main thread as they arrive.
    private var images: [UInt32: CGImage] = [:]
    /// Called with what a key or click asks of the window: open a file, leave the gallery.
    var onAction: ((GeoGalleryAction) -> Void)?
    /// Called when what is in view changed: another folder, a scroll, a new layout.
    var onViewChanged: (() -> Void)?

    /// Where the content sits in the window: right of the Library card, above the toolbar.
    var contentInsets = NSEdgeInsets(top: 96, left: 340, bottom: 84, right: 18) {
        didSet { needsLayout = true }
    }

    private(set) var style: GeoStyle = GeoStyleIllustration
    private let sheet = CALayer()
    private let clip = CALayer()
    private let content = CALayer()
    private var scrollY: CGFloat = 0
    private var painted: (top: CGFloat, bottom: CGFloat)?
    private var tracking: NSTrackingArea?

    // Layers reused from one paint to the next, by kind.
    private var rectLayers: [CALayer] = []
    private var textLayers: [CATextLayer] = []
    private var pictureLayers: [CALayer] = []
    private var lineLayers: [CAShapeLayer] = []
    /// Where each item was painted, by index: the frame of its layer, for the checks.
    private(set) var paintedFrames: [Int: CGRect] = [:]

    init(gallery: GalleryHandle) {
        self.gallery = gallery
        super.init(frame: .zero)
        wantsLayer = true
        layer?.masksToBounds = true
        layer?.addSublayer(sheet)
        clip.masksToBounds = true
        layer?.addSublayer(clip)
        clip.addSublayer(content)
        content.anchorPoint = .zero
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("not used") }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    /// Where the content shows, in the view.
    var contentArea: CGRect { area }

    private var area: CGRect {
        CGRect(
            x: contentInsets.left,
            y: contentInsets.top,
            width: max(120, bounds.width - contentInsets.left - contentInsets.right),
            height: max(120, bounds.height - contentInsets.top - contentInsets.bottom)
        )
    }

    func setStyle(_ style: GeoStyle) {
        guard style != self.style else {
            return
        }
        self.style = style
        gallery.setStyle(style.rawValue)
        drawSheet()
        reload()
    }

    func setImage(_ image: CGImage?, for file: UInt32) {
        images[file] = image
    }

    func clearImages() {
        images.removeAll()
    }

    /// Lays out again if anything changed, and paints what is in view.
    func reload() {
        gallery.setViewport(width: area.width, height: area.height)
        let changed = gallery.layout()
        clampScroll()
        if changed {
            painted = nil
        }
        paint()
        if changed {
            onViewChanged?()
        }
    }

    /// Paints again what is in view, as pictures arrive.
    func repaint() {
        painted = nil
        paint()
    }

    /// Back to the top, as a new folder opens.
    func scrollToTop() {
        scrollY = 0
        gallery.setScroll(0)
        painted = nil
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        sheet.frame = bounds
        clip.frame = area
        drawSheet()
        CATransaction.commit()
        reload()
    }

    // MARK: The sheet

    /// The style's sheet, as the engine draws it: a wash from top to bottom, a vignette, and a
    /// drafting grid where the style has one.
    private func drawSheet() {
        let size = bounds.size
        guard size.width > 0, size.height > 0 else {
            return
        }
        let s = geo_style_sheet(style)
        let scale = window?.backingScaleFactor ?? 2
        let width = Int(size.width * scale)
        let height = Int(size.height * scale)
        guard
            let context = CGContext(
                data: nil,
                width: width,
                height: height,
                bitsPerComponent: 8,
                bytesPerRow: 0,
                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
            )
        else {
            return
        }
        let space = CGColorSpace(name: CGColorSpace.sRGB)!
        let top = [CGFloat(s.top.0), CGFloat(s.top.1), CGFloat(s.top.2), 1]
        let bottom = [CGFloat(s.bottom.0), CGFloat(s.bottom.1), CGFloat(s.bottom.2), 1]
        if let wash = CGGradient(
            colorSpace: space,
            colorComponents: top + bottom,
            locations: nil,
            count: 2
        ) {
            context.drawLinearGradient(
                wash,
                start: CGPoint(x: 0, y: CGFloat(height)),
                end: .zero,
                options: []
            )
        }
        if s.vignette > 0,
            let dark = CGGradient(
                colorSpace: space,
                colorComponents: [0, 0, 0, 0, 0, 0, 0, CGFloat(s.vignette)],
                locations: [0.3, 1.0],
                count: 2
            )
        {
            let middle = CGPoint(x: CGFloat(width) / 2, y: CGFloat(height) / 2)
            context.drawRadialGradient(
                dark,
                startCenter: middle,
                startRadius: 0,
                endCenter: middle,
                endRadius: CGFloat(height),
                options: .drawsAfterEndLocation
            )
        }
        if s.grid_pitch > 0 {
            let pitch = CGFloat(s.grid_pitch) * scale
            for major in [false, true] {
                context.setStrokeColor(
                    red: CGFloat(s.grid.0),
                    green: CGFloat(s.grid.1),
                    blue: CGFloat(s.grid.2),
                    alpha: CGFloat(s.grid_alpha) * (major ? 1.6 : 1)
                )
                context.setLineWidth(1)
                let step = major ? pitch * 5 : pitch
                var x = step
                while x < CGFloat(width) {
                    context.strokeLineSegments(between: [
                        CGPoint(x: x.rounded() + 0.5, y: 0),
                        CGPoint(x: x.rounded() + 0.5, y: CGFloat(height)),
                    ])
                    x += step
                }
                var y = step
                while y < CGFloat(height) {
                    let flipped = CGFloat(height) - y.rounded() - 0.5
                    context.strokeLineSegments(between: [
                        CGPoint(x: 0, y: flipped), CGPoint(x: CGFloat(width), y: flipped),
                    ])
                    y += step
                }
            }
        }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        sheet.contents = context.makeImage()
        sheet.contentsScale = scale
        CATransaction.commit()
    }

    // MARK: Painting

    private func clampScroll() {
        let most = max(0, gallery.contentHeight - area.height)
        scrollY = min(max(0, scrollY), most)
    }

    /// Paints the items within a screen of what is in view, reusing layers; scrolling within that
    /// only moves the content.
    private func paint() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }
        content.frame = CGRect(
            x: 0,
            y: -scrollY,
            width: area.width,
            height: max(gallery.contentHeight, area.height)
        )
        let view = (top: scrollY, bottom: scrollY + area.height)
        if let painted, painted.top <= view.top, painted.bottom >= view.bottom {
            return
        }
        let window = (top: scrollY - area.height, bottom: scrollY + 2 * area.height)
        painted = window
        let scale = self.window?.backingScaleFactor ?? 2
        var rects = 0
        var texts = 0
        var pictures = 0
        var lines = 0
        paintedFrames.removeAll(keepingCapacity: true)
        for (index, item) in gallery.items.enumerated() {
            let lowY = min(item.y, item.y + item.height)
            let highY = max(item.y, item.y + item.height)
            guard CGFloat(highY) >= window.top - 8, CGFloat(lowY) <= window.bottom + 8 else {
                continue
            }
            let z = CGFloat(index)
            switch item.kind {
            case GeoItemRect.rawValue:
                if item.flags & GeoItemSticker.rawValue != 0, item.shadow != 0 {
                    let shadow = reuse(&rectLayers, &rects) { CALayer() }
                    shadow.frame = CGRect(
                        x: CGFloat(item.x) + 3,
                        y: CGFloat(item.y) + 3,
                        width: CGFloat(item.width),
                        height: CGFloat(item.height)
                    )
                    shadow.backgroundColor = Self.color(item.shadow)
                    shadow.borderWidth = 0
                    shadow.cornerRadius = CGFloat(item.radius)
                    shadow.zPosition = z - 0.5
                }
                let layer = reuse(&rectLayers, &rects) { CALayer() }
                paintRect(layer, item, scale: scale)
                layer.zPosition = z
                paintedFrames[index] = layer.frame
            case GeoItemText.rawValue:
                let layer = reuse(&textLayers, &texts) { CATextLayer() }
                paintText(layer, item, scale: scale)
                layer.zPosition = z
                paintedFrames[index] = layer.frame
            case GeoItemPicture.rawValue:
                let layer = reuse(&pictureLayers, &pictures) { CALayer() }
                layer.frame = CGRect(
                    x: CGFloat(item.x),
                    y: CGFloat(item.y),
                    width: CGFloat(item.width),
                    height: CGFloat(item.height)
                )
                layer.contents = images[item.file]
                layer.contentsGravity = .resize
                layer.minificationFilter = .trilinear
                layer.zPosition = z
                paintedFrames[index] = layer.frame
            case GeoItemLine.rawValue:
                let layer = reuse(&lineLayers, &lines) { CAShapeLayer() }
                let path = CGMutablePath()
                path.move(to: CGPoint(x: CGFloat(item.x), y: CGFloat(item.y)))
                path.addLine(
                    to: CGPoint(x: CGFloat(item.x + item.width), y: CGFloat(item.y + item.height))
                )
                layer.frame = content.bounds
                layer.path = path
                layer.strokeColor = Self.color(item.stroke)
                layer.lineWidth = CGFloat(item.line_width)
                layer.lineDashPattern = item.flags & GeoItemDashed.rawValue != 0 ? [4, 3] : nil
                layer.fillColor = nil
                layer.zPosition = z
                paintedFrames[index] = CGRect(
                    x: CGFloat(min(item.x, item.x + item.width)),
                    y: CGFloat(min(item.y, item.y + item.height)),
                    width: CGFloat(abs(item.width)),
                    height: CGFloat(abs(item.height))
                )
            default:
                break
            }
        }
        hideRest(rectLayers, from: rects)
        hideRest(textLayers, from: texts)
        hideRest(pictureLayers, from: pictures)
        hideRest(lineLayers, from: lines)
    }

    private func reuse<L: CALayer>(_ pool: inout [L], _ used: inout Int, make: () -> L) -> L {
        if used == pool.count {
            let layer = make()
            layer.anchorPoint = .zero
            content.addSublayer(layer)
            pool.append(layer)
        }
        let layer = pool[used]
        layer.isHidden = false
        used += 1
        return layer
    }

    private func hideRest<L: CALayer>(_ pool: [L], from used: Int) {
        for layer in pool[used...] {
            layer.isHidden = true
        }
    }

    private func paintRect(_ layer: CALayer, _ item: GeoGalleryItem, scale: CGFloat) {
        layer.frame = CGRect(
            x: CGFloat(item.x),
            y: CGFloat(item.y),
            width: CGFloat(item.width),
            height: CGFloat(item.height)
        )
        layer.cornerRadius = CGFloat(item.radius)
        layer.borderWidth = item.stroke != 0 ? CGFloat(item.line_width) : 0
        layer.borderColor = item.stroke != 0 ? Self.color(item.stroke) : nil
        if item.flags & GeoItemHatched.rawValue != 0 {
            layer.backgroundColor = Self.hatch(Self.color(item.stroke), scale: scale)
            layer.opacity = 0.8
        } else {
            layer.backgroundColor = item.fill != 0 ? Self.color(item.fill) : nil
            layer.opacity = 1
        }
        if item.flags & GeoItemDashed.rawValue != 0 {
            // Construction lines: a dashed outline, drawn by a shape layer of its own.
            layer.borderWidth = 0
            let dash = CAShapeLayer()
            dash.path = CGPath(rect: CGRect(origin: .zero, size: layer.bounds.size), transform: nil)
            dash.strokeColor = Self.color(item.stroke)
            dash.fillColor = nil
            dash.lineWidth = CGFloat(item.line_width)
            dash.lineDashPattern = [4, 3]
            layer.sublayers = [dash]
        } else if layer.sublayers != nil {
            layer.sublayers = nil
        }
    }

    private func paintText(_ layer: CATextLayer, _ item: GeoGalleryItem, scale: CGFloat) {
        let weight: Typography.Weight =
            item.weight >= 600 ? .semibold : item.weight >= 500 ? .medium : .regular
        let font = Typography.font(CGFloat(item.font_size), weight)
        let text = gallery.text(of: item)
        layer.string = NSAttributedString(
            string: text,
            attributes: [
                .font: font, .foregroundColor: NSColor(cgColor: Self.color(item.fill)) ?? .black,
                .kern: CGFloat(item.tracking),
            ]
        )
        layer.alignmentMode = item.align == 1 ? .center : item.align == 2 ? .right : .left
        layer.isWrapped = false
        layer.truncationMode = .none
        layer.contentsScale = scale
        // The line's box starts its ascent above the baseline, as the core reckons it.
        let ascent = font.ascender
        layer.frame = CGRect(
            x: CGFloat(item.x),
            y: CGFloat(item.baseline) - ascent,
            width: CGFloat(item.width) + 1,
            height: ascent - font.descender + 1
        )
    }

    static func color(_ rgba: UInt32) -> CGColor {
        CGColor(
            srgbRed: CGFloat(rgba & 0xFF) / 255,
            green: CGFloat((rgba >> 8) & 0xFF) / 255,
            blue: CGFloat((rgba >> 16) & 0xFF) / 255,
            alpha: CGFloat(rgba >> 24) / 255
        )
    }

    /// Hatching at 45°, every 7 points, for a file that could not be read.
    private static func hatch(_ colour: CGColor, scale: CGFloat) -> CGColor {
        let image = NSImage(size: NSSize(width: 7, height: 7), flipped: false) { rect in
            guard let context = NSGraphicsContext.current?.cgContext else {
                return false
            }
            context.setStrokeColor(colour)
            context.setLineWidth(1)
            context.strokeLineSegments(between: [
                CGPoint(x: -1, y: -1), CGPoint(x: rect.width + 1, y: rect.height + 1),
            ])
            return true
        }
        return NSColor(patternImage: image).cgColor
    }

    // MARK: Checks

    /// Where each item's aligned line and left edge were painted, in content points: a text's
    /// baseline from its layer and font, a picture's part from its frame. NaN for items not painted.
    func measured() -> (lines: [Float], columns: [Float]) {
        let items = gallery.items
        var lines = [Float](repeating: .nan, count: items.count)
        var columns = [Float](repeating: .nan, count: items.count)
        for (index, item) in items.enumerated() {
            guard let frame = paintedFrames[index] else {
                continue
            }
            switch item.kind {
            case GeoItemText.rawValue:
                let weight: Typography.Weight =
                    item.weight >= 600 ? .semibold : item.weight >= 500 ? .medium : .regular
                let font = Typography.font(CGFloat(item.font_size), weight)
                lines[index] = Float(frame.minY + font.ascender)
            case GeoItemPicture.rawValue:
                // Its part's bottom, as far into the picture as the layout put it.
                let fraction = item.height != 0 ? (item.line - item.y) / item.height : 1
                lines[index] = Float(frame.minY) + fraction * Float(frame.height)
            case GeoItemLine.rawValue:
                lines[index] = Float(frame.minY)
            default:
                lines[index] = Float(frame.maxY)
            }
            columns[index] = Float(frame.minX) + (item.column_line - item.x)
        }
        return (lines, columns)
    }

    // MARK: Input

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking {
            removeTrackingArea(tracking)
        }
        let area = NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
            owner: self
        )
        addTrackingArea(area)
        tracking = area
    }

    private func contentPoint(_ event: NSEvent) -> CGPoint? {
        let point = convert(event.locationInWindow, from: nil)
        guard area.contains(point) else {
            return nil
        }
        return CGPoint(x: point.x - area.minX, y: point.y - area.minY + scrollY)
    }

    override func mouseMoved(with event: NSEvent) {
        let hit = contentPoint(event).map { gallery.hit($0) }
        if gallery.hover(kind: hit?.target_kind ?? GeoTargetNone.rawValue, target: hit?.target ?? 0)
        {
            reload()
        }
        let pointing = hit.map { $0.target_kind != GeoTargetNone.rawValue } ?? false
        (pointing ? NSCursor.pointingHand : NSCursor.arrow).set()
    }

    override func mouseExited(with event: NSEvent) {
        if gallery.hover(kind: GeoTargetNone.rawValue, target: 0) {
            reload()
        }
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        guard let point = contentPoint(event) else {
            return
        }
        let hit = gallery.hit(point)
        act(gallery.click(kind: hit.target_kind, target: hit.target, twice: event.clickCount >= 2))
    }

    override func scrollWheel(with event: NSEvent) {
        let step =
            event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : event.scrollingDeltaY * 12
        scrollY -= step
        clampScroll()
        gallery.setScroll(scrollY)
        paint()
        onViewChanged?()
    }

    override func keyDown(with event: NSEvent) {
        let command = event.modifierFlags.contains(.command)
        let key: GeoGalleryKey?
        switch event.keyCode {
        case 123: key = GeoKeyLeft
        case 124: key = GeoKeyRight
        case 125: key = GeoKeyDown
        case 126: key = command ? GeoKeyFolderUp : GeoKeyUp
        case 36, 76: key = GeoKeyEnter
        case 49: key = GeoKeySpace
        case 53: key = GeoKeyEscape
        default: key = nil
        }
        guard let key else {
            super.keyDown(with: event)
            return
        }
        act(gallery.key(key))
    }

    override func cancelOperation(_ sender: Any?) {
        act(gallery.key(GeoKeyEscape))
    }

    private func act(_ action: GeoGalleryAction) {
        switch action.kind {
        case GeoActionNone.rawValue:
            return
        case GeoActionEnterFolder.rawValue:
            scrollToTop()
            reload()
        case GeoActionHandled.rawValue:
            reload()
        default:
            break
        }
        onAction?(action)
    }
}
