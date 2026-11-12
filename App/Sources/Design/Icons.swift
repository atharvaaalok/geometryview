import AppKit

/// The chrome's line icons: the web viewer's own, read from the icons.js the bundle carries, so
/// the app and the page draw the same strokes. Each is drawn as a template image, which takes its
/// view's tint, at a size in points for 16 units, stroked 1.5 units wide as the page strokes it.
@MainActor
enum Icons {
    enum Name: String, CaseIterable {
        case eye, eyeSlash, ghost, scope, undo, redo, seeThrough, scissors, palette, cube, fit
        case chevron, assembly, search, folder, folderPlus, file, plus, close, parts, more, check
        case plates, sheet, ladders, atlas, catalogue
    }

    /// Each icon's SVG markup, by name. icons.js keeps one icon on each line, `  name: \`markup\`,`,
    /// which Launcher/test/icons.test.mjs checks.
    private static let markup: [String: String] = {
        guard let url = Bundle.main.url(forResource: "icons", withExtension: "js"),
            let text = try? String(contentsOf: url, encoding: .utf8)
        else {
            return [:]
        }
        var icons: [String: String] = [:]
        for line in text.split(separator: "\n") {
            if let match = line.wholeMatch(of: /\ \ (\w+): `([^`$]*)`,/) {
                icons[String(match.1)] = String(match.2)
            }
        }
        return icons
    }()

    private static var cache: [String: NSImage] = [:]

    /// `name` at `size` points, turned a quarter clockwise if `turned`, as an open chevron is.
    static func image(_ name: Name, size: CGFloat, turned: Bool = false) -> NSImage {
        let key = "\(name.rawValue) \(size) \(turned)"
        if let image = cache[key] {
            return image
        }
        let body = (markup[name.rawValue] ?? "").replacingOccurrences(of: "currentColor", with: "#000")
        let svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\" width=\"16\" "
            + "height=\"16\" fill=\"none\" stroke=\"#000\" stroke-width=\"1.5\" "
            + "stroke-linecap=\"round\" stroke-linejoin=\"round\">\(body)</svg>"
        let drawing = NSImage(data: Data(svg.utf8)) ?? NSImage()
        drawing.size = NSSize(width: size, height: size)
        let image: NSImage
        if turned {
            image = NSImage(size: drawing.size, flipped: false) { rect in
                let transform = NSAffineTransform()
                transform.translateX(by: rect.midX, yBy: rect.midY)
                transform.rotate(byDegrees: -90)
                transform.translateX(by: -rect.midX, yBy: -rect.midY)
                transform.concat()
                drawing.draw(in: rect)
                return true
            }
        } else {
            image = drawing
        }
        image.isTemplate = true
        cache[key] = image
        return image
    }
}
