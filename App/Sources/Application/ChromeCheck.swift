import AppKit
import GeometryViewEngine

/// Clicks the interface's own controls with real events, and checks that none of them reaches the
/// 3D view.
///
/// Launching with `--chrome-check` and a file waits for the scene, then clicks every segment of
/// every pill in the toolbar and the look card, on its label and beside it, and every plain label
/// on a card. The clicks go through `NSWindow.sendEvent`, which hit-tests and dispatches them
/// exactly as live input. After each click, once the view has settled, pointer moves over an empty
/// corner of the view go to the engine, as the view forwards them. Had the click left the engine
/// holding a press, those moves would turn the camera and draw frames. Last, it checks that the
/// pointer leaving the view clears the hover, that a tree row's hide and isolate buttons and the
/// footer's Show all work, and that presses at the top of the window reach what moves it. Prints
/// the result and quits.
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
        movePointerOutside()
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
                    checkRowButtons()
                }
            }
        }
        probe(0)
    }

    // The real pointer resting over the window would move the tree's hover off the row being
    // checked, so the window moves aside, within its screen, if it is under the pointer.
    private func movePointerOutside() {
        let pointer = NSEvent.mouseLocation
        guard window.frame.contains(pointer), let screen = window.screen else {
            return
        }
        let visible = screen.visibleFrame
        var frame = window.frame
        let leftRoom = pointer.x - visible.minX, rightRoom = visible.maxX - pointer.x
        frame.size.width = min(frame.width, max(leftRoom, rightRoom) - 2)
        frame.origin.x = leftRoom > rightRoom ? pointer.x - 1 - frame.width : pointer.x + 1
        window.setFrame(frame, display: true)
        if window.frame.contains(pointer) {
            leaks.append("the pointer rests over the window, where the check cannot avoid it")
        }
    }

    // A press at the top of the window, or on the file's name, must reach what moves the window.
    private func checkWindowDragging() {
        guard let root = window.contentView else {
            return
        }
        let points = [
            ("the top edge", NSPoint(x: root.bounds.midX, y: root.bounds.maxY - 6)),
            ("the file's name", NSPoint(x: 100, y: root.bounds.maxY - 22)),
        ]
        for (name, point) in points {
            let hit = root.hitTest(root.convert(point, to: root.superview))
            if !(hit is WindowDragArea || hit is Masthead) {
                let found = hit.map { String(describing: type(of: $0)) } ?? "nothing"
                leaks.append("a press on \(name) reaches \(found), not what moves the window")
            }
        }
    }

    // A tree row's buttons, shown as though the pointer were on the row, must reach the engine:
    // hide hides the row's part and shows it again, and isolate leaves only it until the footer's
    // Show all brings everything back.
    private func checkRowButtons() {
        guard let root = window.contentView,
            let table = Self.views(TreeTableView.self, in: root).first
        else {
            finish()
            return
        }
        // The buttons of row 1, the first under the root, as the pointer would show them.
        func button(_ index: Int) -> RowButton? {
            table.setHoveredRow(1)
            guard let cell = table.view(atColumn: 0, row: 1, makeIfNecessary: false) else {
                return nil
            }
            // The buttons a part's row shows: ghost, hide and isolate (the Library's remove stays hidden).
            let buttons = Self.views(RowButton.self, in: cell).filter { !$0.isHidden }
            return buttons.count == 3 ? buttons[index] : nil
        }
        let showAll = { () -> NSView? in
            Self.views(VisibilityFooter.self, in: root).first.flatMap {
                Self.views(PillButton.self, in: $0).first
            }
        }
        let steps: [(String, () -> NSView?, Bool)] = [
            ("the row's hide button", { button(1) }, true),
            ("the row's show button", { button(1) }, false),
            ("the row's isolate button", { button(2) }, true),
            ("the footer's Show all", showAll, false),
        ]
        func run(_ index: Int) {
            guard index < steps.count else {
                finish()
                return
            }
            let (name, find, hides) = steps[index]
            guard let view = find(), !view.isHiddenOrHasHiddenAncestor else {
                leaks.append("\(name) is not there to click")
                finish()
                return
            }
            let point = view.convert(NSPoint(x: view.bounds.midX, y: view.bounds.midY), to: nil)
            send(.leftMouseDown, at: point)
            send(.leftMouseUp, at: point)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [self] in
                if (engine.stats().hidden_count > 0) != hides {
                    leaks.append("\(name) did not \(hides ? "hide" : "show") anything")
                }
                run(index + 1)
            }
        }
        run(0)
    }

    private static func views<View: NSView>(_ type: View.Type, in view: NSView) -> [View] {
        if let match = view as? View {
            return [match]
        }
        return view.subviews.flatMap { views(type, in: $0) }
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
        checkWindowDragging()
        if leaks.isEmpty {
            print(
                "chrome-check: PASS, no click reached the 3D view, hover left with the pointer, "
                    + "the top of the window moves it, and the tree's buttons work"
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
