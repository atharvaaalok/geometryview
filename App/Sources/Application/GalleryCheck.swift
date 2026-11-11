import AppKit
import GeometryViewEngine

/// The gallery's checks, from the command line:
///
///   --gallery <folder>   opens a connected folder's gallery at launch
///   --gallery-check      then measures every alignment group as painted, in Illustration,
///                        Blueprint and Studio, in the folder and its first subfolders; counts the
///                        3D frames drawn while the gallery covers the view; and checks that IBM
///                        Plex Mono's ascent is the 1.025 em the layout reckons with. Writes
///                        gallery-check.json to the temporary folder and quits.
///   --gallery-layout     writes the folder's display list, in Illustration, as
///                        gallery-layout.json, for comparing with the browser's, and quits
///
/// The folder must be inside a connected folder, as the app reaches only those.
@MainActor
final class GalleryCheck {
    static var folder: String? {
        let arguments = CommandLine.arguments
        guard let at = arguments.firstIndex(of: "--gallery"), at + 1 < arguments.count else {
            return nil
        }
        return arguments[at + 1]
    }
    static var isCheck: Bool { CommandLine.arguments.contains("--gallery-check") }
    /// The views the gallery may show: Plates and Sheet, or all five with `--gallery-views all`.
    static var views: UInt32 {
        let arguments = CommandLine.arguments
        if let at = arguments.firstIndex(of: "--gallery-views"), at + 1 < arguments.count,
            arguments[at + 1] == "all"
        {
            return 0b11111
        }
        return (1 << GeoViewPlates.rawValue) | (1 << GeoViewSheet.rawValue)
    }
    static var isLayout: Bool { CommandLine.arguments.contains("--gallery-layout") }

    private let controller: GalleryController
    private let engine: EngineHandle
    private let useStyle: (GeoStyle) -> Void

    init(
        controller: GalleryController,
        engine: EngineHandle,
        useStyle: @escaping (GeoStyle) -> Void
    ) {
        self.controller = controller
        self.engine = engine
        self.useStyle = useStyle
    }

    func run(root: URL, folder: URL) async {
        var report: [String: Any] = ["folder": folder.path]
        // Plex Mono's ascent, which the layout's baselines assume.
        let font = Typography.font(10)
        report["ascentEm"] = Double(font.ascender / 10)
        report["descentEm"] = Double(-font.descender / 10)

        if Self.isLayout {
            useStyle(GeoStyleIllustration)
            controller.show(root: root, folder: folder)
            let waited = await settle()
            report["layout"] = layoutDump()
            report["layoutWaited"] = waited
            write(
                [
                    "width": Double(controller.view.contentArea.width),
                    "height": Double(controller.view.contentArea.height),
                    "folder": controller.gallery.folder, "items": layoutDump(),
                ],
                to: "gallery-layout.json"
            )
            if !Self.isCheck {
                NSApp.terminate(nil)
                return
            }
        }

        // The folder, then its first subfolders, once the scan has listed them.
        controller.show(root: root, folder: folder)
        let scanStart = Date()
        while controller.scanning, Date().timeIntervalSince(scanStart) < 60 {
            try? await Task.sleep(for: .milliseconds(100))
        }
        report["scanSeconds"] = Date().timeIntervalSince(scanStart)
        var folders = [folder]
        for sub in controller.subfolders().prefix(3) {
            folders.append(root.appendingPathComponent(sub))
        }
        var runs: [[String: Any]] = []
        var worst: Float = 0
        var worstDeviation: Float = 0
        var failures = 0
        for style in [GeoStyleIllustration, GeoStyleBlueprint, GeoStyleStudio] {
            useStyle(style)
            for path in folders {
                controller.show(root: root, folder: path)
                let waited = await settle()
                if path == folders.first {
                    snapshot("gallery-\(style.rawValue).png")
                }
                let (lines, columns) = controller.view.measured()
                let result = controller.gallery.check(
                    lines: lines,
                    columns: columns,
                    tolerance: 0.5
                )
                let over = result.over.map {
                    [
                        "group": $0.group, "column": $0.column, "spread": Double($0.spread),
                        "items": $0.items,
                    ]
                        as [String: Any]
                }
                let painted = lines.filter { !$0.isNaN }.count
                // How far each item was painted from where the core laid it.
                var deviation: Float = 0
                for (index, item) in controller.gallery.items.enumerated() {
                    if !lines[index].isNaN {
                        deviation = max(deviation, abs(lines[index] - item.line))
                    }
                    if item.column != 0, !columns[index].isNaN {
                        deviation = max(deviation, abs(columns[index] - item.column_line))
                    }
                }
                worstDeviation = max(worstDeviation, deviation)
                worst = max(worst, result.over.map(\.spread).max() ?? 0)
                failures += result.over.count
                runs.append([
                    "style": Int(style.rawValue), "folder": controller.gallery.folder,
                    "groupsChecked": Int(result.checked), "over": over, "itemsPainted": painted,
                    "waitedSeconds": waited, "waitingInView": controller.waitingInView,
                    "maxDeviation": Double(deviation),
                ])
                print(
                    "gallery-check: style \(style.rawValue) \(controller.gallery.folder.isEmpty ? "/" : controller.gallery.folder): \(result.checked) groups, \(result.over.count) over 0.5 pt, waited \(String(format: "%.1f", waited)) s"
                )
            }
        }
        // No 3D frames while the gallery covers the view.
        let before = engine.stats().frames_rendered
        try? await Task.sleep(for: .seconds(3))
        let after = engine.stats().frames_rendered
        report["framesWhileCovered"] = Int(after - before)
        report["runs"] = runs
        report["groupsOver"] = failures
        report["worstSpread"] = Double(worst)
        report["maxDeviation"] = Double(worstDeviation)
        if let stats = controller.stats() {
            report["library"] = [
                "helpers": stats.helpers, "imports": stats.imports, "pictures": stats.pictures,
                "lookups": stats.lookups, "failures": stats.failures,
            ]
        }
        write(report, to: "gallery-check.json")
        print(
            "gallery-check: \(failures) groups over 0.5 pt, \(after - before) 3D frames while covered"
        )
        NSApp.terminate(nil)
    }

    /// Waits for the pictures in view, at most two minutes. Returns the seconds waited.
    private func settle() async -> Double {
        let start = Date()
        while controller.waitingInView > 0, Date().timeIntervalSince(start) < 120 {
            try? await Task.sleep(for: .milliseconds(200))
        }
        // A last burst of pictures to lay out and paint.
        try? await Task.sleep(for: .milliseconds(400))
        controller.view.reload()
        controller.view.repaint()
        return Date().timeIntervalSince(start)
    }

    private func layoutDump() -> [[String: Any]] {
        let gallery = controller.gallery
        return gallery.items.map { item in
            var row: [String: Any] = [
                "kind": Int(item.kind), "x": Double(item.x), "y": Double(item.y),
                "w": Double(item.width),
                "h": Double(item.height), "key": Int(item.key), "group": Int(item.group),
                "line": Double(item.line), "column": Int(item.column),
                "columnLine": Double(item.column_line),
            ]
            if item.kind == GeoItemText.rawValue {
                row["text"] = gallery.text(of: item)
                row["baseline"] = Double(item.baseline)
            }
            if item.kind == GeoItemPicture.rawValue {
                row["file"] = gallery.path(of: item.file)
            }
            return row
        }
    }

    /// The window as it shows, drawn from its layers, to look at.
    private func snapshot(_ name: String) {
        guard let root = controller.view.window?.contentView, let layer = root.layer else {
            return
        }
        let scale = controller.view.window?.backingScaleFactor ?? 2
        guard
            let context = CGContext(
                data: nil,
                width: Int(root.bounds.width * scale),
                height: Int(root.bounds.height * scale),
                bitsPerComponent: 8,
                bytesPerRow: 0,
                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
            )
        else {
            return
        }
        context.scaleBy(x: scale, y: scale)
        layer.render(in: context)
        if let image = context.makeImage() {
            let output = FileManager.default.temporaryDirectory.appendingPathComponent(name)
            try? NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])?
                .write(to: output)
        }
    }

    private func write(_ report: [String: Any], to name: String) {
        let output = FileManager.default.temporaryDirectory.appendingPathComponent(name)
        if let data = try? JSONSerialization.data(
            withJSONObject: report,
            options: [.prettyPrinted, .sortedKeys]
        ) {
            try? data.write(to: output)
            print("Wrote \(output.path)")
        }
    }
}
