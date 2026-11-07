import AppKit

/// The base of every clickable control in the sticker interface.
///
/// A control claims every click inside its bounds, so its labels never take one. Clicks on a
/// control's label used to reach the 3D view, which then held a press it never saw released
/// (`--chrome-check` finds this).
///
/// Hover is not latched from enter and exit events. It is worked out from where the pointer really
/// is whenever it could have gone stale, as when the control moves or hides under a still pointer,
/// so it cannot stick.
@MainActor
class ChromeControl: NSView {
    var onPress: (() -> Void)?
    var isEnabled = true {
        didSet {
            if !isEnabled {
                isPressing = false
            }
            stateChanged()
        }
    }
    private(set) var isHovering = false
    private(set) var isPressing = false
    private var tracking: NSTrackingArea?

    /// Redraws the control for a change of hover, press or enablement.
    func stateChanged() {}

    override func hitTest(_ point: NSPoint) -> NSView? {
        guard !isHidden, NSMouseInRect(point, frame, superview?.isFlipped ?? false) else {
            return nil
        }
        return self
    }

    // MARK: Hover

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking {
            removeTrackingArea(tracking)
        }
        // Active whatever window is key, so a change of focus cannot strand a hover.
        let area = NSTrackingArea(
            rect: bounds,
            options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect],
            owner: self
        )
        addTrackingArea(area)
        tracking = area
        // The control may have moved under a still pointer, which sends no enter or exit.
        syncHover()
    }

    override func viewDidHide() {
        super.viewDidHide()
        syncHover()
    }

    override func viewDidUnhide() {
        super.viewDidUnhide()
        syncHover()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        syncHover()
    }

    override func mouseEntered(with event: NSEvent) {
        setHovering(true)
    }

    override func mouseExited(with event: NSEvent) {
        setHovering(false)
    }

    private func syncHover() {
        guard let window, !isHiddenOrHasHiddenAncestor else {
            setHovering(false)
            return
        }
        let point = convert(window.mouseLocationOutsideOfEventStream, from: nil)
        setHovering(bounds.contains(point))
    }

    private func setHovering(_ hovering: Bool) {
        guard hovering != isHovering else {
            return
        }
        isHovering = hovering
        if !hovering {
            isPressing = false
        }
        stateChanged()
    }

    // MARK: Pressing

    override func mouseDown(with event: NSEvent) {
        guard isEnabled else {
            return
        }
        isPressing = true
        stateChanged()
    }

    override func mouseDragged(with event: NSEvent) {
        guard isEnabled else {
            return
        }
        let inside = bounds.contains(convert(event.locationInWindow, from: nil))
        if inside != isPressing {
            isPressing = inside
            stateChanged()
        }
    }

    override func mouseUp(with event: NSEvent) {
        let pressed = isPressing
        isPressing = false
        stateChanged()
        if isEnabled, pressed, bounds.contains(convert(event.locationInWindow, from: nil)) {
            onPress?()
        }
    }

    override func rightMouseDown(with event: NSEvent) {}
    override func otherMouseDown(with event: NSEvent) {}

    override func accessibilityPerformPress() -> Bool {
        guard isEnabled else {
            return false
        }
        onPress?()
        return true
    }
}
