import AppKit

/// Renders the window's layer tree to a PNG for visual checks and golden tests.
///
/// Launching with `--snapshot` writes `chrome.png` to the app's temporary directory and quits. The
/// 3D surface is not included, because `CAMetalLayer` contents do not render through
/// `CALayer.render(in:)`; `gcli render` covers the 3D output.
@MainActor
enum ChromeSnapshot {
    static var isRequested: Bool { CommandLine.arguments.contains("--snapshot") }

    static func capture(_ window: NSWindow) {
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
