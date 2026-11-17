import AppKit
import GeometryViewEngine

/// Pictures the person saves or pastes elsewhere.
enum Picture {
    /// `image` as a PNG that says what made it: GeometryView and its version, and how to cite it
    /// (geo_tag_picture), as PNG text that image viewers show.
    static func png(_ image: CGImage) -> Data? {
        let bitmap = NSBitmapImageRep(cgImage: image)
        guard let png = bitmap.representation(using: .png, properties: [:]) else {
            return nil
        }
        return png.withUnsafeBytes { raw -> Data in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else {
                return png
            }
            var tagged = GeoPng()
            guard geo_tag_picture(base, raw.count, &tagged) else {
                return png
            }
            defer { geo_png_free(&tagged) }
            return Data(bytes: tagged.bytes, count: tagged.size)
        }
    }
}
