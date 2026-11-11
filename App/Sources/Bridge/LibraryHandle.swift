import Foundation
import GeometryViewEngine

/// What the library measured of a file.
struct LibraryFacts: Sendable {
    let size: (Double, Double, Double)
    let hasBox: Bool
    let estimated: Bool
    let parts: UInt32
    let product: String
    let triangles: UInt64
}

/// What the library reports, on the main thread.
enum LibraryEvent: Sendable {
    case facts(path: String, facts: LibraryFacts)
    case picture(path: String, png: String, picture: GeoGalleryPicture)
    case progress(path: String, done: UInt32, total: UInt32)
    case failed(path: String, message: String)
    /// A file's detail picture, twice the pixels, for where it is drawn large.
    case detail(path: String, png: String, picture: GeoGalleryPicture)
    /// A frame of Peek's turntable: `frame` of `frames`.
    case turnFrame(path: String, frame: Int, frames: Int, png: String, picture: GeoGalleryPicture)
}

/// The gallery's library service (library.h) on the Swift side: files wanted, in bands, become facts
/// and pictures, from the caches where it can.
@MainActor
final class LibraryHandle {
    private let library: OpaquePointer
    private let box: LibraryBox

    var onEvent: ((LibraryEvent) -> Void)? {
        didSet { box.handler = onEvent }
    }

    init?() {
        guard let shaders = Bundle.main.path(forResource: "default", ofType: "metallib"),
            let helper = Bundle.main.path(forAuxiliaryExecutable: "geometryview-import"),
            let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first?
                .appendingPathComponent("GeometryView", isDirectory: true)
        else {
            return nil
        }
        try? FileManager.default.createDirectory(at: caches, withIntermediateDirectories: true)
        let box = LibraryBox()
        self.box = box
        let created = shaders.withCString { shaderPath in
            helper.withCString { helperPath in
                caches.path.withCString { cachePath -> OpaquePointer? in
                    var descriptor = GeoLibraryDescriptor(
                        shader_library_path: shaderPath,
                        import_helper_path: helperPath,
                        cache_directory: cachePath,
                        callback: receiveLibraryEvent,
                        context: Unmanaged.passUnretained(box).toOpaque(),
                        slots: 0,
                        workers: 0
                    )
                    return geo_library_create(&descriptor)
                }
            }
        }
        guard let created else {
            return nil
        }
        library = created
    }

    isolated deinit {
        geo_library_destroy(library)
    }

    struct Want {
        let path: String
        let band: GeoLibraryBand
        let order: Double
        let bytes: UInt64
        let modified: Double
        let folderColor: Int32
    }

    func want(_ wants: [Want]) {
        let paths = wants.map { strdup($0.path) }
        defer { paths.forEach { free($0) } }
        var list = wants.enumerated().map { index, want in
            GeoLibraryWant(
                path: paths[index],
                band: want.band,
                order: want.order,
                bytes: want.bytes,
                modified: want.modified,
                folder_color: want.folderColor
            )
        }
        geo_library_want(library, &list, UInt32(list.count))
    }

    func setPicture(style: GeoStyle, colorMode: GeoColorMode, pixels: UInt32, lineScale: Float) {
        var spec = GeoPictureSpec(
            style: style,
            color_mode: colorMode,
            pixels: pixels,
            pixels_per_point: 2,
            line_scale: lineScale,
            supersample: 2  // drawn twice as large and averaged down: smooth edges
        )
        geo_library_set_picture(library, &spec)
    }

    /// The files drawn large: each gets a detail picture, twice the pixels.
    func detail(paths: [String]) {
        let copies = paths.map { strdup($0) }
        defer {
            for copy in copies {
                free(copy)
            }
        }
        var pointers = copies.map { UnsafePointer<CChar>($0) }
        geo_library_detail(library, &pointers, UInt32(pointers.count))
    }

    func setPrefetch(_ allowed: Bool) {
        geo_library_set_prefetch(library, allowed)
    }

    /// Forgets what it has told: files wanted next are told again, from the caches.
    func reset() {
        geo_library_reset(library)
    }

    /// Peek's turntable: `frames` pictures of the file at `path`, `pixels` square, turned evenly
    /// about the upright at one scale, ahead of any other; nil stops it.
    func turn(path: String?, frames: Int, pixels: Int) {
        geo_library_turn(library, path, UInt32(frames), UInt32(pixels))
    }

    /// Lists a folder's STEP and STL files and folders, off the main thread, keeping the listing.
    func scan(_ root: URL) async -> [ScanEntry]? {
        let library = UnsafeSendable(value: library)
        return await Task.detached(priority: .userInitiated) {
            let collector = ScanCollector()
            var summary = GeoScanSummary()
            var error = [CChar](repeating: 0, count: 256)
            let ok = geo_library_scan(
                library.value,
                root.path,
                collectScan,
                Unmanaged.passUnretained(collector).toOpaque(),
                &summary,
                &error,
                256
            )
            return ok ? collector.entries : nil
        }.value
    }

    /// The listing kept from a folder's last scan, at once.
    func keptListing(_ root: URL) -> [ScanEntry]? {
        let collector = ScanCollector()
        let found = geo_library_kept_listing(
            library,
            root.path,
            collectScan,
            Unmanaged.passUnretained(collector).toOpaque()
        )
        return found ? collector.entries : nil
    }

    func stats() -> GeoLibraryStats {
        geo_library_stats(library)
    }
}

private struct UnsafeSendable<T>: @unchecked Sendable {
    let value: T
}

private final class ScanCollector: @unchecked Sendable {
    var entries: [ScanEntry] = []
}

private func collectScan(
    context: UnsafeMutableRawPointer?,
    entries: UnsafePointer<GeoScanEntry>?,
    count: UInt32
)
    -> Bool
{
    guard let context, let entries else {
        return true
    }
    let collector = Unmanaged<ScanCollector>.fromOpaque(context).takeUnretainedValue()
    for i in 0..<Int(count) {
        let e = entries[i]
        collector.entries.append(
            ScanEntry(
                path: String(cString: e.path),
                folder: e.folder,
                bytes: e.bytes,
                modified: e.modified
            )
        )
    }
    return true
}

private func receiveLibraryEvent(
    context: UnsafeMutableRawPointer?,
    event: UnsafePointer<GeoLibraryEvent>?
) {
    guard let context, let event = event?.pointee else {
        return
    }
    let box = Unmanaged<LibraryBox>.fromOpaque(context).takeUnretainedValue()
    let path = String(cString: event.path)
    switch event.kind {
    case GeoLibraryFacts:
        let f = event.facts
        let size = f.max
        let low = f.min
        box.deliver(
            .facts(
                path: path,
                facts: LibraryFacts(
                    size: (size.0 - low.0, size.1 - low.1, size.2 - low.2),
                    hasBox: f.has_box,
                    estimated: f.estimated,
                    parts: f.parts,
                    product: f.name.map { String(cString: $0) } ?? "",
                    triangles: f.triangles
                )
            )
        )
    case GeoLibraryPicture, GeoLibraryTurnFrame, GeoLibraryDetail:
        let p = event.picture
        let picture = GeoGalleryPicture(
            width: p.width,
            height: p.height,
            mm_per_pixel: p.world_per_pixel,
            part: (
                Float(p.part_rect.0), Float(p.part_rect.1), Float(p.part_rect.2),
                Float(p.part_rect.3)
            ),
            ink: (
                Float(p.ink_rect.0), Float(p.ink_rect.1), Float(p.ink_rect.2), Float(p.ink_rect.3)
            )
        )
        let png = String(cString: p.png_path)
        if event.kind == GeoLibraryDetail {
            box.deliver(.detail(path: path, png: png, picture: picture))
        } else if event.kind == GeoLibraryTurnFrame {
            box.deliver(
                .turnFrame(
                    path: path,
                    frame: Int(event.done),
                    frames: Int(event.total),
                    png: png,
                    picture: picture
                )
            )
        } else {
            box.deliver(.picture(path: path, png: png, picture: picture))
        }
    case GeoLibraryProgress:
        box.deliver(.progress(path: path, done: event.done, total: event.total))
    case GeoLibraryFailed:
        box.deliver(.failed(path: path, message: event.message.map { String(cString: $0) } ?? ""))
    default:
        break
    }
}

private final class LibraryBox: @unchecked Sendable {
    // Only touched on the main thread, which is why the type can be Sendable.
    nonisolated(unsafe) var handler: ((LibraryEvent) -> Void)?

    func deliver(_ event: LibraryEvent) {
        DispatchQueue.main.async { [self] in
            handler?(event)
        }
    }
}
