import AppKit
import GeometryEngine

/// Drives a scripted drag through the live app, for measuring real frame times and latency.
///
/// Launching with `--exercise` rotates the opened model for a few seconds at the display's rate, posts
/// a hover sweep, prints the results and quits. Events enter at the engine's public interface with a
/// real timestamp, so this measures everything after AppKit delivers input.
@MainActor
final class Exercise {
    static var isRequested: Bool { CommandLine.arguments.contains("--exercise") }

    private static let dragSeconds =
        Double(ProcessInfo.processInfo.environment["EXERCISE_SECONDS"] ?? "") ?? 6.0
    private static let pixelsPerSecond = 500.0
    private static let refineWaitSeconds =
        Double(ProcessInfo.processInfo.environment["EXERCISE_WAIT"] ?? "") ?? 3.0

    private let engine: EngineHandle
    private let center: CGPoint
    private let scale: CGFloat
    private var timer: Timer?
    private var startTime = 0.0
    private var startFrames: UInt64 = 0
    private var pressed = false

    init(engine: EngineHandle, size: CGSize) {
        self.engine = engine
        scale = NSScreen.main?.backingScaleFactor ?? 2
        center = CGPoint(x: size.width * scale * 0.5, y: size.height * scale * 0.5)
    }

    func start(window: NSWindow) {
        if CommandLine.arguments.contains("--external"), let screen = NSScreen.screens.last {
            window.setFrameOrigin(screen.visibleFrame.origin)
        }
        if CommandLine.arguments.contains("--fullscreen") {
            window.toggleFullScreen(nil)
        }
        // Let the first frames settle, so launch work is not counted. Entering fullscreen animates.
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { [self] in
            press()
            startTime = ProcessInfo.processInfo.systemUptime
            startFrames = engine.stats().frames_rendered
            timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 240.0, repeats: true) { _ in
                MainActor.assumeIsolated {
                    self.tick()
                }
            }
        }
    }

    // Sweeps the pointer across the view until it finds the model, hovers it, then clicks it, so the
    // pick path is proven in the live app with a real drawable, not only in tests.
    private func checkPicking() {
        let x = center.x
        var step = 0
        func probe() {
            let y = CGFloat(step) * center.y * 2 / 40
            post(GeoPointerMove, GeoButtonNone, x: x, y: y)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [self] in
                let hovered = engine.stats().hovered_id
                if hovered != 0 {
                    click(x: x, y: y, hovered: hovered)
                } else if step < 40 {
                    step += 1
                    probe()
                } else {
                    print("exercise: the pointer never found the model")
                    quit()
                }
            }
        }
        probe()
    }

    private func click(x: CGFloat, y: CGFloat, hovered: UInt32) {
        post(GeoPointerDown, GeoButtonLeft, x: x, y: y)
        post(GeoPointerUp, GeoButtonLeft, x: x, y: y)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [self] in
            print(
                "exercise: hovered face \(hovered), selected \(engine.stats().selected_count)"
            )
            zoomIn(x: x, y: y)
        }
    }

    // Zooms far into the clicked point and waits, which is what makes refinement start.
    private func zoomIn(x: CGFloat, y: CGFloat) {
        for _ in 0..<12 {
            engine.post(
                GeoPointerEvent(
                    kind: GeoPointerScroll,
                    button: GeoButtonNone,
                    x: Float(x),
                    y: Float(y),
                    delta: 200,
                    modifiers: 0,
                    timestamp: ProcessInfo.processInfo.systemUptime
                )
            )
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.refineWaitSeconds) { [self] in
            checkChromeIsolation()
        }
    }

    // Once the view is idle, changing only the chrome must draw no 3D frame.
    private func checkChromeIsolation() {
        let before = engine.stats().frames_rendered
        var step = 0
        func next() {
            guard step < 12, let window = NSApp.windows.first as? MainWindow else {
                let drawn = engine.stats().frames_rendered - before
                print("exercise: 12 chrome-only changes drew \(drawn) 3D frames")
                quit()
                return
            }
            window.exerciseChrome(step: step)
            step += 1
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { next() }
        }
        next()
    }

    private func quit() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [engine] in
            engine.reportLatency()
            geo_trace_memory("after interaction")
            NSApp.terminate(nil)
        }
    }

    private func press() {
        post(GeoPointerDown, GeoButtonLeft, x: center.x, y: center.y)
        pressed = true
    }

    private func post(_ kind: GeoPointerKind, _ button: GeoPointerButton, x: CGFloat, y: CGFloat) {
        engine.post(
            GeoPointerEvent(
                kind: kind,
                button: button,
                x: Float(x),
                y: Float(y),
                delta: 0,
                modifiers: 0,
                timestamp: ProcessInfo.processInfo.systemUptime
            )
        )
    }

    private func tick() {
        let elapsed = ProcessInfo.processInfo.systemUptime - startTime
        guard elapsed < Self.dragSeconds else {
            finish(elapsed)
            return
        }
        let offset = CGFloat(elapsed * Self.pixelsPerSecond) * scale
        post(GeoPointerMove, GeoButtonNone, x: center.x + offset, y: center.y + offset * 0.3)
    }

    private func finish(_ elapsed: Double) {
        timer?.invalidate()
        post(GeoPointerUp, GeoButtonLeft, x: center.x, y: center.y)
        let stats = engine.stats()
        let frames = stats.frames_rendered - startFrames
        print(
            String(
                format:
                    "exercise: %llu frames in %.2f s, %.1f fps, last CPU encode %.2f ms, "
                    + "mean GPU %.2f ms, p99 %.2f ms, %llu allocations in the last frame",
                frames,
                elapsed,
                Double(frames) / elapsed,
                stats.last_frame_seconds * 1000,
                stats.gpu_frame_seconds * 1000,
                stats.gpu_frame_p99_seconds * 1000,
                stats.last_frame_allocations
            )
        )
        let screen = NSApp.windows.first?.screen
        print(
            "exercise: on \(screen?.localizedName ?? "unknown"), "
                + "\(screen?.maximumFramesPerSecond ?? 0) Hz, "
                + "scale \(screen?.backingScaleFactor ?? 0)"
        )
        checkPicking()
    }
}
