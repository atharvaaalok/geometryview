import AppKit
import GeometryEngine

/// Clicks the interface's own controls with real events, and checks that none of them reaches the
/// 3D view.
///
/// Launching with `--chrome-check` and a file waits for the scene, then clicks every segment of
/// every pill in the toolbar and the look card, on its label and beside it, and every plain label
/// on a card. The clicks go through `NSWindow.sendEvent`, which hit-tests and dispatches them
/// exactly as live input. After each click, once the view has settled, pointer moves over an empty
/// corner of the view go to the engine, as the view forwards them. Had the click left the engine
/// holding a press, those moves would turn the camera and draw frames. Last, it checks that the
/// pointer leaving the view clears the hover. Prints the result and quits.
@MainActor
final class ChromeCheck {
    static var isRequested: Bool { CommandLine.arguments.contains("--chrome-check") }

    private let window: MainWindow
    private let engine: EngineHandle
    private var targets: [(view: NSView, point: NSPoint)] = []
    private var leaks: [String] = []

    init(window: MainWindow, engine: EngineHandle) {
        self.window = window
        self.engine = engine
    }

    func start(lookCard: NSView) {
        lookCard.isHidden = false
        guard let root = window.contentView else {
            return
        }
        root.layoutSubtreeIfNeeded()
        for segment in Self.pills(in: root).flatMap(\.subviews)
        where segment is ChromeControl && !segment.isHidden {
            let y = segment.bounds.midY
            for x in [segment.bounds.midX, segment.bounds.minX + 3] {
                targets.append((segment, segment.convert(NSPoint(x: x, y: y), to: nil)))
            }
        }
        let segmentClicks = targets.count
        for label in Self.cardLabels(in: root) {
            targets.append(
                (label, label.convert(NSPoint(x: label.bounds.midX, y: label.bounds.midY), to: nil))
            )
        }
        print(
            "chrome-check: \(segmentClicks) clicks on segments, \(targets.count - segmentClicks) on card labels"
        )
        next(0)
    }

    private func next(_ index: Int) {
        guard index < targets.count else {
            checkLeave()
            return
        }
        let target = targets[index]
        send(.leftMouseDown, at: target.point)
        send(.leftMouseDragged, at: NSPoint(x: target.point.x + 1, y: target.point.y))
        send(.leftMouseUp, at: target.point)
        waitUntilSettled(previous: engine.stats().frames_rendered, quietChecks: 0, checks: 0) {
            self.moveOverEmptyCorner { drawn in
                if drawn > 1 {
                    let name =
                        (target.view as? NSTextField)?.stringValue
                        ?? target.view.accessibilityLabel() ?? "a segment"
                    self.leaks.append(
                        "a click on \(name) at \(Int(target.point.x)), \(Int(target.point.y)) "
                            + "left the 3D view holding a press"
                    )
                }
                self.next(index + 1)
            }
        }
    }

    // Finds the model by sweeping down the middle of the view, then checks that the pointer
    // leaving the view clears its hover.
    private func checkLeave() {
        let scale = window.backingScaleFactor
        let size = (window.contentView?.bounds.size ?? .zero)
        func probe(_ step: Int) {
            guard step < 40 else {
                leaks.append("the pointer never found the model, so leaving was not checked")
                finish()
                return
            }
            let x = size.width * 0.6 * scale
            let y = CGFloat(step) * size.height * scale / 40
            post(GeoPointerMove, x: x, y: y)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [self] in
                guard engine.stats().hovered_id != 0 else {
                    probe(step + 1)
                    return
                }
                // Left where it was, so only the leaving can clear the hover.
                post(GeoPointerLeave, x: x, y: y)
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [self] in
                    if engine.stats().hovered_id != 0 {
                        leaks.append("the hover stayed after the pointer left the view")
                    }
                    finish()
                }
            }
        }
        probe(0)
    }

    private func post(_ kind: GeoPointerKind, x: CGFloat, y: CGFloat) {
        engine.post(
            GeoPointerEvent(
                kind: kind,
                button: GeoButtonNone,
                x: Float(x),
                y: Float(y),
                delta: 0,
                modifiers: 0,
                timestamp: ProcessInfo.processInfo.systemUptime
            )
        )
    }

    private func finish() {
        if leaks.isEmpty {
            print(
                "chrome-check: PASS, no click reached the 3D view and hover left with the pointer"
            )
        } else {
            print("chrome-check: FAIL, \(leaks.count) problem\(leaks.count == 1 ? "" : "s"):")
            for leak in leaks {
                print("chrome-check:   \(leak)")
            }
        }
        NSApp.terminate(nil)
    }

    // Waits until no frame has been drawn for a quarter of a second, so flights are done.
    private func waitUntilSettled(
        previous: UInt64,
        quietChecks: Int,
        checks: Int,
        then: @escaping @MainActor () -> Void
    ) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [self] in
            let frames = engine.stats().frames_rendered
            let quiet = frames == previous ? quietChecks + 1 : 0
            if quiet >= 5 || checks > 200 {
                then()
            } else {
                waitUntilSettled(
                    previous: frames,
                    quietChecks: quiet,
                    checks: checks + 1,
                    then: then
                )
            }
        }
    }

    // Reports how many frames moves over an empty corner drew. The moves are spaced in time, as a
    // hand moves, since moves that arrive together are drawn as one frame. One frame is allowed,
    // for a hover that changed on the way in.
    private func moveOverEmptyCorner(then: @escaping @MainActor (UInt64) -> Void) {
        guard let root = window.contentView else {
            return
        }
        let before = engine.stats().frames_rendered
        // Straight to the engine, as the view forwards a move, in pixels from the top left.
        let scale = window.backingScaleFactor
        let corner = CGPoint(x: (root.bounds.maxX - 80) * scale, y: (root.bounds.maxY - 70) * scale)
        func move(_ step: Int) {
            guard step < 8 else {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [self] in
                    then(engine.stats().frames_rendered - before)
                }
                return
            }
            post(GeoPointerMove, x: corner.x - CGFloat(step % 2) * 30 * scale, y: corner.y)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.025) { move(step + 1) }
        }
        move(0)
    }

    private func send(_ type: NSEvent.EventType, at point: NSPoint) {
        guard
            let event = NSEvent.mouseEvent(
                with: type,
                location: point,
                modifierFlags: [],
                timestamp: ProcessInfo.processInfo.systemUptime,
                windowNumber: window.windowNumber,
                context: nil,
                eventNumber: 0,
                clickCount: type == .mouseMoved ? 0 : 1,
                pressure: type == .leftMouseDown ? 1 : 0
            )
        else {
            return
        }
        window.sendEvent(event)
    }

    // The plain labels of the visible cards, outside tables.
    private static func cardLabels(in view: NSView, onCard: Bool = false) -> [NSTextField] {
        if view.isHidden || view is NSTableView || view is SegmentedPill {
            return []
        }
        if onCard, let label = view as? NSTextField, !label.isEditable, !label.isSelectable,
            !label.stringValue.isEmpty
        {
            return [label]
        }
        return view.subviews.flatMap { cardLabels(in: $0, onCard: onCard || view is StickerCard) }
    }

    private static func pills(in view: NSView) -> [SegmentedPill] {
        if let pill = view as? SegmentedPill {
            return pill.isHiddenOrHasHiddenAncestor ? [] : [pill]
        }
        return view.subviews.flatMap { pills(in: $0) }
    }
}
