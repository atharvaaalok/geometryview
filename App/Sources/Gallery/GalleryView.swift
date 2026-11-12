import AppKit
import GeometryViewEngine
import QuartzCore

/// Layers reused from one paint to the next, by kind, within one container: the content, which
/// scrolls, or the items fixed in the view (Peek).
@MainActor
private final class LayerPool {
    let parent: CALayer
    var rects: [CALayer] = []
    var texts: [CATextLayer] = []
    var pictures: [CALayer] = []
    var pictureFiles: [UInt32] = []
    var lines: [CAShapeLayer] = []
    var usedRects = 0
    var usedTexts = 0
    var usedPictures = 0
    var usedLines = 0

    init(parent: CALayer) { self.parent = parent }

    func begin() {
        usedRects = 0
        usedTexts = 0
        usedPictures = 0
        usedLines = 0
    }

    func finish() {
        for layer in rects[usedRects...] { layer.isHidden = true }
        for layer in texts[usedTexts...] { layer.isHidden = true }
        for layer in pictures[usedPictures...] { layer.isHidden = true }
        for layer in lines[usedLines...] { layer.isHidden = true }
        // In the display list's order: layers are reused for other items from paint to paint, and
        // their order among siblings, not only their zPosition, decides what covers what wherever
        // the tree is drawn (a snapshot's render(in:) reads only the order).
        if let layers = parent.sublayers,
            zip(layers, layers.dropFirst()).contains(where: { $0.zPosition > $1.zPosition })
        {
            parent.sublayers = layers.sorted { $0.zPosition < $1.zPosition }
        }
    }

    func rect() -> CALayer { take(&rects, &usedRects) { CALayer() } }
    func text() -> CATextLayer { take(&texts, &usedTexts) { CATextLayer() } }
    func line() -> CAShapeLayer { take(&lines, &usedLines) { CAShapeLayer() } }

    func picture(file: UInt32) -> CALayer {
        let layer = take(&pictures, &usedPictures) { CALayer() }
        if pictureFiles.count < pictures.count {
            pictureFiles.append(file)
        }
        pictureFiles[usedPictures - 1] = file
        return layer
    }

    private func take<L: CALayer>(_ pool: inout [L], _ used: inout Int, make: () -> L) -> L {
        if used == pool.count {
            let layer = make()
            layer.anchorPoint = .zero
            parent.addSublayer(layer)
            pool.append(layer)
        }
        let layer = pool[used]
        layer.isHidden = false
        // Placed afresh: a frame set through a heading's old offset would land off by as much.
        layer.transform = CATransform3DIdentity
        used += 1
        return layer
    }
}

/// Paints the gallery's display list (gallery.h) with Core Animation layers, over the style's sheet,
/// and turns the pointer and keys into the gallery's actions. Everything it draws was placed by the
/// layout core, so the browser's painter draws the same.
///
/// The core says what a layout plays against the one before (transition and tracks); this plays it
/// with Core Animation, the old view's layers handed to a copy beneath the new.
@MainActor
final class GalleryView: NSView {
    var gallery: GalleryHandle
    /// Pictures by file, decoded off the main thread as they arrive.
    private var images: [UInt32: CGImage] = [:]
    /// Called with what a key or click asks of the window: open a file, leave the gallery.
    var onAction: ((GeoGalleryAction) -> Void)?
    /// Called when what is in view changed: another folder, a scroll, a new layout.
    var onViewChanged: (() -> Void)?
    /// Called when typing changed the search.
    var onSearchChanged: (() -> Void)?
    /// Called with Esc while a poster shows.
    var onLeavePoster: (() -> Void)?
    /// Paints every item, not only those near the view: for a poster's picture.
    private var paintEverything = false

    /// Where the content sits in the window: right of the Library card, above the toolbar.
    var contentInsets = NSEdgeInsets(top: 96, left: 340, bottom: 84, right: 18) {
        didSet { needsLayout = true }
    }

    private(set) var style: GeoStyle = GeoStyleIllustration
    private let sheet = CALayer()
    /// The view's area: what scrolls, what is fixed over it, and during a move a copy of the old
    /// view beneath; a zoom stays inside it.
    private let stage = CALayer()
    private let clip = CALayer()
    private var content = CALayer()
    private let fixed = CALayer()
    private var ghost: CALayer?
    private var scrollY: CGFloat = 0
    private var painted: (top: CGFloat, bottom: CGFloat)?
    private var tracking: NSTrackingArea?

    private var contentPool: LayerPool
    private let fixedPool: LayerPool
    /// Headings that stay at the top while their block scrolls under them.
    private var sticky: [(layer: CALayer, top: CGFloat, until: CGFloat)] = []
    /// Each painted item's main layer, by index, for its track in a move.
    private var layerOf: [Int: CALayer] = [:]
    /// Lanes, which scroll sideways on their own: each a box clipped to it, whose bounds' origin is
    /// its scroll, so its items keep their content points; with its layers.
    private var lanes: [UInt32: GeoGalleryLane] = [:]
    private var laneScroll: [UInt32: CGFloat] = [:]
    private var laneLayers: [UInt32: (box: CALayer, pool: LayerPool)] = [:]
    private var laneDrag: (lane: UInt32, x: CGFloat, from: CGFloat)?
    /// The style's sheet drawn at a size, for rectangles filled with it (Peek's picture).
    private var sheetImages: [String: CGImage] = [:]
    /// Where each item was painted, by index: the frame of its layer, for the checks.
    private(set) var paintedFrames: [Int: CGRect] = [:]

    init(gallery: GalleryHandle) {
        self.gallery = gallery
        contentPool = LayerPool(parent: content)
        fixedPool = LayerPool(parent: fixed)
        super.init(frame: .zero)
        wantsLayer = true
        layer?.masksToBounds = true
        layer?.addSublayer(sheet)
        stage.masksToBounds = true
        stage.anchorPoint = .zero
        layer?.addSublayer(stage)
        clip.anchorPoint = .zero
        stage.addSublayer(clip)
        clip.addSublayer(content)
        content.anchorPoint = .zero
        fixed.anchorPoint = .zero
        stage.addSublayer(fixed)
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
        sheetImages.removeAll()
        drawSheet()
        reload()
    }

    func setImage(_ image: CGImage?, for file: UInt32) {
        images[file] = image
    }

    func clearImages() {
        images.removeAll()
    }

    /// Lays out again if anything changed, and paints what is in view, playing what the change
    /// asks. With `reveal`, scrolls first so the selection shows: after a key or a switch of views.
    func reload(reveal: Bool = false) {
        gallery.setViewport(width: area.width, height: area.height)
        let changed = gallery.layout()
        var motion: (GeoGalleryTransition, [GeoGalleryTrack], LayerPool)?
        if changed {
            painted = nil
            let transition = gallery.transition
            if transition.kind != GeoTransitionNone.rawValue,
                !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
            {
                motion = (transition, gallery.tracks, handOldViewToGhost())
            }
        }
        if reveal, let top = gallery.reveal {
            scrollY = top
        }
        clampScroll()
        gallery.setScroll(scrollY)
        paint()
        if let (transition, tracks, old) = motion {
            play(transition, tracks: tracks, old: old)
        }
        // Along its lane too, if the selection is in one and scrolled out of it.
        if reveal, let along = gallery.revealLane(), let lane = lanes[along.lane] {
            scrollLane(lane, to: along.x)
        }
        if changed {
            onViewChanged?()
        }
    }

    /// Shows another view, if enabled, flying what both views show; the selection kept in view.
    @discardableResult
    func setView(_ view: UInt32) -> Bool {
        guard gallery.setView(view) else {
            return false
        }
        reload(reveal: true)
        return true
    }

    /// Paints again what is in view, as pictures arrive.
    func repaint() {
        painted = nil
        paint()
    }

    /// Back to the top, as a new folder opens; its lanes at their beginnings.
    func scrollToTop() {
        for id in laneScroll.keys {
            gallery.setLaneScroll(id, 0)
        }
        laneScroll.removeAll()
        scrollY = 0
        gallery.setScroll(0)
        painted = nil
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        sheet.frame = bounds
        stage.frame = area
        clip.frame = stage.bounds
        fixed.frame = stage.bounds
        drawSheet()
        CATransaction.commit()
        reload()
    }

    // MARK: The sheet

    /// The style's sheet, as the engine draws it: a wash from top to bottom, a vignette, and a
    /// drafting grid where the style has one.
    private func drawSheet() {
        let scale = window?.backingScaleFactor ?? 2
        guard let image = sheetImage(bounds.size, scale: scale) else {
            return
        }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        sheet.contents = image
        sheet.contentsScale = scale
        CATransaction.commit()
    }

    /// The style's sheet at a size in points.
    private func sheetImage(_ size: CGSize, scale: CGFloat) -> CGImage? {
        guard size.width > 0, size.height > 0 else {
            return nil
        }
        let s = geo_style_sheet(style)
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
            return nil
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
        return context.makeImage()
    }

    // MARK: Painting

    private func clampScroll() {
        let most = max(0, gallery.contentHeight - area.height)
        scrollY = min(max(0, scrollY), most)
    }

    /// Paints the items within a screen of what is in view, reusing layers; scrolling within that
    /// only moves the content, and the headings that stay at the top.
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
        placeSticky()
        let view = (top: scrollY, bottom: scrollY + area.height)
        if let painted, painted.top <= view.top, painted.bottom >= view.bottom {
            return
        }
        let window =
            paintEverything
            ? (top: -CGFloat.infinity, bottom: CGFloat.infinity)
            : (top: scrollY - area.height, bottom: scrollY + 2 * area.height)
        painted = window
        let scale = self.window?.backingScaleFactor ?? 2
        contentPool.begin()
        fixedPool.begin()
        lanes = Dictionary(gallery.lanes.map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
        for (_, layer) in laneLayers {
            layer.pool.begin()
        }
        sticky.removeAll(keepingCapacity: true)
        layerOf.removeAll(keepingCapacity: true)
        paintedFrames.removeAll(keepingCapacity: true)
        let items = gallery.items
        for (index, item) in items.enumerated() {
            let isFixed = item.flags & GeoItemFixed.rawValue != 0
            let isSticky = !isFixed && (item.sticky_top != 0 || item.sticky_until != 0)
            if !isFixed {
                // A heading that stays at the top is painted wherever its block shows.
                let lowY = CGFloat(min(item.y, item.y + item.height))
                let highY =
                    CGFloat(max(item.y, item.y + item.height))
                    + (isSticky ? CGFloat(item.sticky_until) : 0)
                guard highY >= window.top - 8, lowY <= window.bottom + 8 else {
                    continue
                }
            }
            var pool = isFixed ? fixedPool : contentPool
            if item.lane != 0 {
                // In a lane: painted a lane's width either side of what shows.
                guard let lane = lanes[item.lane] else {
                    continue
                }
                let scroll = laneScroll[item.lane] ?? 0
                let (lx, lw) = (CGFloat(lane.rect.0), CGFloat(lane.rect.2))
                let left = CGFloat(min(item.x, item.x + item.width))
                let right = CGFloat(max(item.x, item.x + item.width))
                if right < lx + scroll - lw || left > lx + scroll + 2 * lw {
                    continue
                }
                pool = laneLayer(lane, z: CGFloat(index)).pool
            }
            // Headings stay above what scrolls under them.
            let z = CGFloat(isSticky ? items.count + index : index)
            // What a search did not find, at 22%; a picture being drawn again, at 45%.
            let fade: Float =
                (item.flags & GeoItemFading.rawValue != 0 ? 0.22 : 1)
                * (item.flags & GeoItemStale.rawValue != 0 ? 0.45 : 1)
            var layers: [CALayer] = []
            switch item.kind {
            case GeoItemRect.rawValue:
                if item.flags & GeoItemSticker.rawValue != 0, item.shadow != 0 {
                    let shadow = pool.rect()
                    shadow.frame = CGRect(
                        x: CGFloat(item.x) + 3,
                        y: CGFloat(item.y) + 3,
                        width: CGFloat(item.width),
                        height: CGFloat(item.height)
                    )
                    shadow.backgroundColor = Self.color(item.shadow)
                    shadow.contents = nil
                    shadow.masksToBounds = false
                    shadow.borderWidth = 0
                    shadow.cornerRadius = CGFloat(item.radius)
                    shadow.sublayers = nil
                    shadow.zPosition = z - 0.5
                    shadow.opacity = fade
                    layers.append(shadow)
                }
                let layer = pool.rect()
                paintRect(layer, item, scale: scale)
                layer.opacity *= fade
                layer.zPosition = z
                paintedFrames[index] = layer.frame
                layers.append(layer)
            case GeoItemText.rawValue:
                let layer = pool.text()
                paintText(layer, item, scale: scale)
                layer.opacity = fade
                layer.zPosition = z
                paintedFrames[index] = layer.frame
                layers.append(layer)
            case GeoItemPicture.rawValue:
                let layer = pool.picture(file: item.file)
                layer.frame = CGRect(
                    x: CGFloat(item.x),
                    y: CGFloat(item.y),
                    width: CGFloat(item.width),
                    height: CGFloat(item.height)
                )
                layer.contents = images[item.file]
                layer.contentsGravity = .resize
                layer.minificationFilter = .trilinear
                layer.opacity = fade
                layer.zPosition = z
                paintedFrames[index] = layer.frame
                layers.append(layer)
            case GeoItemLine.rawValue:
                let layer = pool.line()
                let path = CGMutablePath()
                path.move(to: CGPoint(x: CGFloat(item.x), y: CGFloat(item.y)))
                path.addLine(
                    to: CGPoint(x: CGFloat(item.x + item.width), y: CGFloat(item.y + item.height))
                )
                layer.frame = pool.parent.bounds
                layer.path = path
                layer.strokeColor = Self.color(item.stroke)
                layer.lineWidth = CGFloat(item.line_width)
                layer.lineDashPattern = item.flags & GeoItemDashed.rawValue != 0 ? [4, 3] : nil
                layer.fillColor = nil
                layer.opacity = fade
                layer.zPosition = z
                paintedFrames[index] = CGRect(
                    x: CGFloat(min(item.x, item.x + item.width)),
                    y: CGFloat(min(item.y, item.y + item.height)),
                    width: CGFloat(abs(item.width)),
                    height: CGFloat(abs(item.height))
                )
                layers.append(layer)
            default:
                break
            }
            if isSticky {
                for layer in layers {
                    sticky.append((layer, CGFloat(item.sticky_top), CGFloat(item.sticky_until)))
                }
            }
            layerOf[index] = layers.last
        }
        contentPool.finish()
        fixedPool.finish()
        for (id, layer) in laneLayers {
            layer.pool.finish()
            layer.box.isHidden = lanes[id] == nil
        }
        placeSticky()
    }

    /// A lane's box, clipped to it, its bounds' origin the lane's scroll, stacked where its first
    /// item is.
    private func laneLayer(_ lane: GeoGalleryLane, z: CGFloat) -> (box: CALayer, pool: LayerPool) {
        let layer: (box: CALayer, pool: LayerPool)
        if let existing = laneLayers[lane.id] {
            layer = existing
        } else {
            let box = CALayer()
            box.anchorPoint = .zero
            box.masksToBounds = true
            content.addSublayer(box)
            layer = (box, LayerPool(parent: box))
            laneLayers[lane.id] = layer
        }
        if layer.pool.usedRects + layer.pool.usedTexts + layer.pool.usedPictures
            + layer.pool.usedLines == 0
        {
            let (x, y) = (CGFloat(lane.rect.0), CGFloat(lane.rect.1))
            let (w, h) = (CGFloat(lane.rect.2), CGFloat(lane.rect.3))
            layer.box.position = CGPoint(x: x, y: y)
            layer.box.bounds = CGRect(x: x + (laneScroll[lane.id] ?? 0), y: y, width: w, height: h)
            layer.box.zPosition = z
            layer.box.isHidden = false
        }
        return layer
    }

    private func lane(at point: CGPoint) -> GeoGalleryLane? {
        lanes.values.first { lane in
            point.x >= CGFloat(lane.rect.0) && point.x <= CGFloat(lane.rect.0 + lane.rect.2)
                && point.y >= CGFloat(lane.rect.1) && point.y <= CGFloat(lane.rect.1 + lane.rect.3)
        }
    }

    /// Scrolls a lane sideways, within its content, painting further along it as it goes.
    private func scrollLane(_ lane: GeoGalleryLane, to x: CGFloat) {
        let most = max(0, CGFloat(lane.content_width - lane.rect.2))
        let to = min(max(0, x), most)
        guard to != (laneScroll[lane.id] ?? 0) else {
            return
        }
        laneScroll[lane.id] = to
        gallery.setLaneScroll(lane.id, to)
        if let box = laneLayers[lane.id]?.box {
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            box.bounds.origin.x = CGFloat(lane.rect.0) + to
            CATransaction.commit()
        }
        painted = nil
        reload()
        onViewChanged?()
    }

    /// Moves each heading down by how far the view has scrolled past it, until the next takes over.
    private func placeSticky() {
        for (layer, top, until) in sticky {
            let dy = min(max(scrollY - top, 0), until)
            layer.transform =
                dy > 0 ? CATransform3DMakeTranslation(0, dy, 0) : CATransform3DIdentity
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
        layer.contents = nil
        layer.masksToBounds = false
        if item.flags & GeoItemRuler.rawValue != 0 {
            // A ruler: its line on top, minor ticks every `radius` points, major every `tracking`.
            layer.borderWidth = 0
            layer.cornerRadius = 0
            layer.backgroundColor = nil
            layer.opacity = 1
            let w = CGFloat(item.width)
            let ink = Self.color(item.stroke)
            let shapes = [
                (CGFloat(item.radius), 0.75, 3.5, Float(0.5)),
                (CGFloat(item.tracking), 1.1, 6.5, Float(1)),
            ]
            .map { pitch, tickW, tickH, alpha -> CAShapeLayer in
                let path = CGMutablePath()
                if pitch > 0.5 {
                    var x: CGFloat = 0
                    while x < w {
                        path.addRect(CGRect(x: x, y: 1, width: tickW, height: tickH))
                        x += pitch
                    }
                }
                let shape = CAShapeLayer()
                shape.path = path
                shape.fillColor = ink
                shape.opacity = alpha
                return shape
            }
            let line = CALayer()
            line.frame = CGRect(x: 0, y: 0, width: w, height: CGFloat(item.line_width))
            line.backgroundColor = ink
            layer.sublayers = shapes + [line]
            return
        } else if item.flags & GeoItemHatched.rawValue != 0 {
            layer.backgroundColor = Self.hatch(Self.color(item.stroke), scale: scale)
            layer.opacity = 0.8
        } else if item.flags & GeoItemSheet.rawValue != 0 {
            // The style's own sheet, as the gallery's ground.
            let size = layer.bounds.size
            let key = "\(Int(size.width))x\(Int(size.height))@\(scale)"
            if sheetImages[key] == nil {
                sheetImages[key] = sheetImage(size, scale: scale)
            }
            layer.backgroundColor = nil
            layer.contents = sheetImages[key]
            layer.contentsGravity = .resize
            layer.masksToBounds = true
            layer.opacity = 1
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

    // MARK: Motion

    /// Hands the view as last painted to a copy beneath the content, for the old view's part in a
    /// move, and starts the content afresh. Returns the old layers' pool.
    private func handOldViewToGhost() -> LayerPool {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }
        ghost?.removeFromSuperlayer()
        let copy = CALayer()
        copy.anchorPoint = .zero
        copy.frame = stage.bounds
        copy.masksToBounds = true
        let old = content
        let oldPool = contentPool
        old.removeFromSuperlayer()
        copy.addSublayer(old)
        stage.insertSublayer(copy, below: clip)
        ghost = copy
        content = CALayer()
        content.anchorPoint = .zero
        clip.addSublayer(content)
        contentPool = LayerPool(parent: content)
        sticky.removeAll()
        laneLayers.removeAll()  // the old lanes' boxes went with the old view
        return oldPool
    }

    private func play(_ t: GeoGalleryTransition, tracks: [GeoGalleryTrack], old: LayerPool) {
        guard let ghost else {
            return
        }
        let ease = CAMediaTimingFunction(controlPoints: 0.3, 0.7, 0.2, 1)
        let size = stage.bounds.size
        let (px, py, pw, ph) = (
            CGFloat(t.rect.0), CGFloat(t.rect.1), CGFloat(t.rect.2), CGFloat(t.rect.3)
        )
        func move(_ tx: CGFloat, _ ty: CGFloat, _ s: CGFloat) -> NSValue {
            NSValue(
                caTransform3D: CATransform3DScale(CATransform3DMakeTranslation(tx, ty, 0), s, s, 1)
            )
        }
        // The whole view put into the plate, and the plate blown up to fill the view.
        let into: NSValue = {
            let s = min(pw / size.width, ph / size.height)
            return move(px + (pw - size.width * s) / 2, py + (ph - size.height * s) / 2, s)
        }()
        let outOf: NSValue = {
            let s = min(size.width / max(pw, 1), size.height / max(ph, 1))
            return move((size.width - pw * s) / 2 - px * s, (size.height - ph * s) / 2 - py * s, s)
        }()
        let identity = NSValue(caTransform3D: CATransform3DIdentity)
        let duration = CFTimeInterval(t.duration) / 1000
        func animate(
            _ layer: CALayer,
            _ key: String,
            _ values: [Any],
            times: [NSNumber]? = nil,
            duration: CFTimeInterval,
            delay: CFTimeInterval = 0,
            timing: CAMediaTimingFunction = ease,
            keep: Bool = false
        ) {
            let a = CAKeyframeAnimation(keyPath: key)
            a.values = values
            a.keyTimes = times
            a.duration = duration
            a.timingFunction = timing
            a.beginTime = delay > 0 ? layer.convertTime(CACurrentMediaTime(), from: nil) + delay : 0
            a.fillMode = keep ? .both : .backwards
            a.isRemovedOnCompletion = !keep
            layer.add(a, forKey: "gallery.\(key)")
        }
        CATransaction.begin()
        CATransaction.setCompletionBlock { [weak self, weak ghost] in
            ghost?.removeFromSuperlayer()
            if let self, self.ghost === ghost {
                self.ghost = nil
            }
        }
        switch t.kind {
        case GeoTransitionInto.rawValue:
            // The folder grows out of its plate; the plates around it rush past.
            animate(clip, "transform", [into, identity], duration: duration)
            animate(clip, "opacity", [0, 1, 1], times: [0, 0.4, 1], duration: duration)
            animate(ghost, "transform", [identity, outOf], duration: duration, keep: true)
        case GeoTransitionOut.rawValue:
            // The folder shrinks back into its plate; the plates around it settle.
            animate(clip, "transform", [outOf, identity], duration: duration)
            animate(clip, "opacity", [0.3, 1], duration: duration)
            animate(ghost, "transform", [identity, into], duration: duration, keep: true)
        default:
            // A switch: parts in both views fly from where they were, the rest fade in, the old
            // view fades out.
            let items = gallery.items
            let easeOut = CAMediaTimingFunction(name: .easeOut)
            for track in tracks {
                let index = Int(track.item)
                guard let layer = layerOf[index], index < items.count else {
                    continue
                }
                let item = items[index]
                let trackDuration = CFTimeInterval(track.duration) / 1000
                let delay = CFTimeInterval(track.delay) / 1000
                if track.kind == GeoTrackFly.rawValue {
                    let dx = CGFloat(track.from.0) - CGFloat(item.x)
                    let dy = CGFloat(track.from.1) - (CGFloat(item.y) - scrollY)
                    let s = item.width > 0 ? CGFloat(track.from.2) / CGFloat(item.width) : 1
                    animate(
                        layer,
                        "transform",
                        [move(dx, dy, s), identity],
                        duration: trackDuration
                    )
                    for (i, file) in old.pictureFiles.enumerated()
                    where file == item.file && i < old.pictures.count {
                        old.pictures[i].isHidden = true
                    }
                } else {
                    animate(
                        layer,
                        "opacity",
                        [0, layer.opacity],
                        duration: trackDuration,
                        delay: delay,
                        timing: easeOut
                    )
                }
            }
            animate(clip, "opacity", [0.001, 1], duration: 0.16, timing: easeOut)
            animate(
                ghost,
                "opacity",
                [1, 0],
                duration: CFTimeInterval(t.fade_out) / 1000,
                timing: easeOut,
                keep: true
            )
        }
        CATransaction.commit()
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
            case GeoItemRect.rawValue where item.flags & GeoItemRuler.rawValue != 0:
                lines[index] = Float(frame.minY) + item.line_width / 2  // a ruler's line, its top
            default:
                lines[index] = Float(frame.maxY)
            }
            columns[index] = Float(frame.minX) + (item.column_line - item.x)
        }
        return (lines, columns)
    }

    // MARK: The poster

    /// The poster as a picture, `scale` pixels a point: the window's width, and as tall as it is or
    /// as the poster runs, whichever is taller. The live view's content is lent to a page of its own
    /// for the drawing and handed back before anything shows.
    func posterImage(scale: CGFloat = 2) -> CGImage? {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }
        scrollY = 0
        gallery.setScroll(0)
        paintEverything = true
        painted = nil
        paint()
        paintEverything = false
        painted = nil
        let size = CGSize(
            width: bounds.width,
            height: max(bounds.height, area.minY + gallery.contentHeight + contentInsets.bottom)
        )
        guard let sheetPicture = sheetImage(size, scale: scale),
            let context = CGContext(
                data: nil,
                width: Int(size.width * scale),
                height: Int(size.height * scale),
                bitsPerComponent: 8,
                bytesPerRow: 0,
                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
            )
        else {
            return nil
        }
        context.draw(
            sheetPicture,
            in: CGRect(x: 0, y: 0, width: size.width * scale, height: size.height * scale)
        )
        // A page laid out top down, as this view is, holding the content where the view shows it.
        let page = CALayer()
        page.anchorPoint = .zero
        page.frame = CGRect(origin: .zero, size: size)
        page.isGeometryFlipped = true
        let holder = CALayer()
        holder.anchorPoint = .zero
        holder.frame = CGRect(
            x: area.minX,
            y: area.minY,
            width: area.width,
            height: size.height - area.minY
        )
        page.addSublayer(holder)
        let lent = content
        let frame = lent.frame
        lent.removeFromSuperlayer()
        holder.addSublayer(lent)
        lent.frame = CGRect(
            x: 0,
            y: 0,
            width: frame.width,
            height: max(frame.height, holder.bounds.height)
        )
        // Laid out top down, as the view is: the page's geometry is turned over, and the context
        // with it, so the layers' contents stand upright.
        context.translateBy(x: 0, y: size.height * scale)
        context.scaleBy(x: scale, y: -scale)
        page.render(in: context)
        lent.removeFromSuperlayer()
        clip.addSublayer(lent)
        lent.frame = frame
        paint()
        return context.makeImage()
    }

    // MARK: For the checks

    /// Scrolls to `top`, as the wheel does.
    func scroll(to top: CGFloat) {
        scrollY = top
        clampScroll()
        gallery.setScroll(scrollY)
        paint()
        onViewChanged?()
    }

    /// A key, as typed; a click on a target.
    func press(_ key: GeoGalleryKey) { act(gallery.key(key), reveal: true) }
    func click(kind: UInt32, target: UInt32) {
        act(gallery.click(kind: kind, target: target, twice: false))
    }

    /// The first heading staying at the top: where it sits from the view's top, as painted.
    func stuckHeading() -> (top: CGFloat, fromViewTop: CGFloat)? {
        for (layer, top, until) in sticky where !(layer is CATextLayer) && !(layer is CAShapeLayer)
        {
            let dy = scrollY - top
            if dy > 0, dy < until {
                // From the layer's own corner, which its transform moves.
                return (top, layer.convert(CGPoint.zero, to: stage).y)
            }
        }
        return nil
    }

    /// Layers painted at a search's 22%, and Peek's layers over the view.
    var fadedLayers: Int {
        (content.sublayers ?? []).filter { !$0.isHidden && abs($0.opacity - 0.22) < 0.01 }.count
    }
    var fixedLayers: Int { (fixed.sublayers ?? []).filter { !$0.isHidden }.count }

    /// The moves playing: each animation's key, delay and duration in milliseconds.
    func animations() -> [String] {
        var layers: [CALayer] = [clip] + (ghost.map { [$0] } ?? []) + (content.sublayers ?? [])
        layers = layers.filter { !$0.isHidden }
        var out: [String] = []
        for layer in layers {
            for key in layer.animationKeys() ?? [] where key.hasPrefix("gallery.") {
                guard let a = layer.animation(forKey: key) else {
                    continue
                }
                let delay =
                    a.beginTime > 0
                    ? a.beginTime - layer.convertTime(CACurrentMediaTime(), from: nil) : 0
                out.append(
                    "\(key.dropFirst(8)) \(Int((max(0, delay) * 1000).rounded()))+\(Int((a.duration * 1000).rounded()))"
                )
            }
        }
        return out
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
        guard !gallery.poster else {
            return
        }
        let hit = contentPoint(event).map { gallery.hit($0) }
        if gallery.hover(kind: hit?.target_kind ?? GeoTargetNone.rawValue, target: hit?.target ?? 0)
        {
            reload()
        }
        let pointing =
            hit.map {
                [GeoTargetFolder.rawValue, GeoTargetFile.rawValue, GeoTargetMore.rawValue].contains(
                    $0.target_kind
                ) || ($0.target_kind == GeoTargetPeek.rawValue && $0.target == 1)
            } ?? false
        (pointing ? NSCursor.pointingHand : NSCursor.arrow).set()
    }

    override func mouseExited(with event: NSEvent) {
        if gallery.hover(kind: GeoTargetNone.rawValue, target: 0) {
            reload()
        }
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        guard !gallery.poster, let point = contentPoint(event) else {
            return
        }
        laneDrag = lane(at: point).map { ($0.id, point.x, laneScroll[$0.id] ?? 0) }
        let hit = gallery.hit(point)
        act(gallery.click(kind: hit.target_kind, target: hit.target, twice: event.clickCount >= 2))
    }

    /// A drag along a lane scrolls it.
    override func mouseDragged(with event: NSEvent) {
        guard let drag = laneDrag, let point = contentPoint(event), let lane = lanes[drag.lane]
        else {
            return
        }
        scrollLane(lane, to: drag.from - (point.x - drag.x))
    }

    override func mouseUp(with event: NSEvent) {
        laneDrag = nil
    }

    override func scrollWheel(with event: NSEvent) {
        // Sideways, or shifted, over a lane: the lane scrolls.
        let sideways =
            abs(event.scrollingDeltaX) > abs(event.scrollingDeltaY)
            || event.modifierFlags.contains(.shift)
        if sideways, let point = contentPoint(event), let lane = lane(at: point) {
            let raw = abs(event.scrollingDeltaX) > 0 ? event.scrollingDeltaX : event.scrollingDeltaY
            let delta = event.hasPreciseScrollingDeltas ? raw : raw * 12
            scrollLane(lane, to: (laneScroll[lane.id] ?? 0) - delta)
            return
        }
        let step =
            event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : event.scrollingDeltaY * 12
        scrollY -= step
        clampScroll()
        gallery.setScroll(scrollY)
        paint()
        onViewChanged?()
    }

    /// Keys: arrows, ⏎, Space and Esc are the gallery's; typing searches, digits too, and Backspace
    /// edits the search, never going up a folder. ⌘1–⌘5 come through the View menu.
    override func keyDown(with event: NSEvent) {
        let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let command = modifiers.contains(.command)
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
        if gallery.poster {
            if event.keyCode == 53 {
                onLeavePoster?()
            }
            return  // a poster takes no keys but Esc
        }
        if let key {
            act(gallery.key(key), reveal: true)
            onSearchChanged?()
            return
        }
        let plain = modifiers.isDisjoint(with: [.command, .control, .option])
        if event.keyCode == 51, plain {
            // Backspace
            search(String(gallery.search.dropLast()))
            return
        }
        if plain, let characters = event.characters, characters.count == 1,
            let scalar = characters.unicodeScalars.first,
            !CharacterSet.controlCharacters.contains(scalar), scalar.value < 0xF700
        {
            search(gallery.search + characters)
            return
        }
        super.keyDown(with: event)
    }

    override func cancelOperation(_ sender: Any?) {
        if gallery.poster {
            onLeavePoster?()
            return
        }
        act(gallery.key(GeoKeyEscape))
        onSearchChanged?()
    }

    /// Searches the folder in view; empty clears it.
    func search(_ query: String) {
        guard query != gallery.search else {
            return
        }
        gallery.search = query
        reload()
        onSearchChanged?()
    }

    private func act(_ action: GeoGalleryAction, reveal: Bool = false) {
        switch action.kind {
        case GeoActionNone.rawValue:
            return
        case GeoActionEnterFolder.rawValue:
            scrollToTop()
            reload()
        case GeoActionHandled.rawValue, GeoActionPeek.rawValue:
            reload(reveal: reveal)
        default:
            break
        }
        onAction?(action)
    }
}
