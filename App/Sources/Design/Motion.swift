import AppKit

/// Motion tokens. Every animated change in the chrome uses these, so it all moves alike, and none
/// of it moves when the system asks for reduced motion.
@MainActor
enum Motion {
    static let fast: CFTimeInterval = 0.16
    static let slow: CFTimeInterval = 0.22
    static let ease = CAMediaTimingFunction(controlPoints: 0.3, 0.7, 0.2, 1)

    static var reduced: Bool { NSWorkspace.shared.accessibilityDisplayShouldReduceMotion }

    /// Runs layer changes in `changes` with the interface's easing.
    static func animate(_ duration: CFTimeInterval = fast, _ changes: () -> Void) {
        CATransaction.begin()
        CATransaction.setAnimationDuration(reduced ? 0 : duration)
        CATransaction.setAnimationTimingFunction(ease)
        changes()
        CATransaction.commit()
    }
}
