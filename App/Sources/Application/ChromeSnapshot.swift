import AppKit

/// Writes what the window shows to a PNG, for visual checks and golden tests.
///
/// Launching with `--snapshot` writes `chrome.png` to the app's temporary directory and quits. The
/// engine draws its current view offscreen, and the window's own layers are drawn over that, so the
/// picture is the window as seen, without needing screen recording permission.
@MainActor
enum ChromeSnapshot {
    static var isRequested: Bool { CommandLine.arguments.contains("--snapshot") }

    static func capture(_ window: NSWindow, engine: EngineHandle) {
        let figure = FileManager.default.temporaryDirectory.appendingPathComponent("figure.png")
        try? FileManager.default.removeItem(at: figure)
        engine.writeSnapshot(to: figure.path)
        waitForFile(figure, attempts: 40) {
            compose(window, figure: NSImage(contentsOf: figure))
        }
    }

    private static func waitForFile(
        _ url: URL,
        attempts: Int,
        then: @escaping @MainActor () -> Void
    ) {
        if FileManager.default.fileExists(atPath: url.path) || attempts == 0 {
            then()
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            waitForFile(url, attempts: attempts - 1, then: then)
        }
    }

    private static func compose(_ window: NSWindow, figure: NSImage?) {
        guard let root = window.contentView, let layer = root.layer else {
            return
        }
        let scale = window.backingScaleFactor
        let width = Int(root.bounds.width * scale)
        let height = Int(root.bounds.height * scale)
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
        context.setFillColor(Palette.paper.cgColor)
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        if let image = figure?.cgImage(forProposedRect: nil, context: nil, hints: nil) {
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        }
        context.scaleBy(x: scale, y: scale)
        layer.render(in: context)

        guard let image = context.makeImage() else {
            return
        }
        let output = FileManager.default.temporaryDirectory.appendingPathComponent("chrome.png")
        let representation = NSBitmapImageRep(cgImage: image)
        try? representation.representation(using: .png, properties: [:])?.write(to: output)
        print("snapshot: \(output.path)")
    }
}
